#pragma once

#include <nxt/http.hpp>
#include <nxtio/async.hpp>
#include <nxtio/http.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::io::llm {

struct protocol_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct stream_complete : std::exception
{
    [[nodiscard]] const char * what() const noexcept override
    {
        return "stream complete";
    }
};

struct openai_responses_request
{
    std::string api_key;
    std::string model = "gpt-5-mini";
    std::string input;
    std::size_t max_output_tokens = 512;
    std::string reasoning_effort = "minimal";
    bool store = false;
};

struct stream_event
{
    std::string type;
    nlohmann::json payload;
    std::string raw;
};

[[nodiscard]] inline nlohmann::json
openai_responses_body(const openai_responses_request & request)
{
    auto body = nlohmann::json{
        {"model", request.model},
        {"input", request.input},
        {"stream", true},
        {"store", request.store},
        {"max_output_tokens", request.max_output_tokens},
    };

    if (!request.reasoning_effort.empty()) {
        body["reasoning"] = {
            {"effort", request.reasoning_effort},
        };
    }

    return body;
}

[[nodiscard]] inline std::string
openai_responses_http_request(const openai_responses_request & request)
{
    auto body = openai_responses_body(request).dump();
    auto wire = std::string{};
    wire += "POST /v1/responses HTTP/1.1\r\n";
    wire += "Host: api.openai.com\r\n";
    wire += "User-Agent: nxtllm/0\r\n";
    wire += "Accept: text/event-stream\r\n";
    wire += "Content-Type: application/json\r\n";
    wire += "Authorization: Bearer ";
    wire += request.api_key;
    wire += "\r\n";
    wire += "Content-Length: ";
    wire += std::to_string(body.size());
    wire += "\r\n";
    wire += "Connection: close\r\n";
    wire += "\r\n";
    wire += body;
    return wire;
}

[[nodiscard]] inline bool response_status_is_success(std::string_view status)
{
    auto first_space = status.find(' ');
    if (first_space == std::string_view::npos)
        return false;
    auto code = status.substr(first_space + 1, 3);
    return code.size() == 3 && code[0] == '2'
        && std::ranges::all_of(code, [](char c) { return c >= '0' && c <= '9'; });
}

template<typename OnEvent>
nxt::task<bool>
emit_sse_event(nxt::http::server_sent_event const & sse, OnEvent & on_event)
{
    if (sse.data == "[DONE]")
        co_return true;

    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(sse.data);
    } catch (const nlohmann::json::exception & e) {
        throw protocol_error{
            "OpenAI Responses stream sent invalid JSON: "
            + std::string{e.what()}};
    }

    co_await on_event(stream_event{
        .type = sse.type,
        .payload = std::move(payload),
        .raw = sse.data,
    });

    co_return sse.type == "response.completed"
        || sse.type == "response.incomplete" || sse.type == "response.failed";
}

template<typename OnEvent>
nxt::task<bool>
emit_sse_events(std::vector<nxt::http::server_sent_event> events, OnEvent & on_event)
{
    for (const auto & event : events) {
        if (co_await emit_sse_event(event, on_event))
            co_return true;
    }
    co_return false;
}

template<typename Transport, typename OnEvent>
nxt::task<> stream_openai_responses_over(
    Transport & transport,
    const openai_responses_request & request,
    OnEvent on_event)
{
    if (request.api_key.empty())
        throw protocol_error{"OPENAI_API_KEY is empty"};
    if (request.input.empty())
        throw protocol_error{"OpenAI Responses input is empty"};

    co_await transport.write_all(openai_responses_http_request(request));

    auto head_buffer = std::array<char, 16 * 1024>{};
    auto head = co_await nxt::io::http::async_grab(
        transport, head_buffer, nxt::io::http::slurp("\r\n\r\n"));
    auto response = nxt::io::http::parse_response_head(head.bytes);

    auto body_buffer = std::array<char, 16 * 1024>{};
    auto line_buffer = std::array<char, 1024>{};

    auto read_error_body =
        [&]() -> nxt::task<std::string> {
        auto body = std::string{};
        auto collect = [&](std::span<const std::byte> chunk) -> nxt::task<> {
            body += nxt::io::http::as_text(chunk);
            co_return;
        };

        if (response.chunked) {
            co_await nxt::io::http::read_chunked(
                transport, line_buffer, body_buffer, head.leftover, collect);
        } else if (response.content_length) {
            co_await nxt::io::http::read_content_length(
                transport,
                body_buffer,
                head.leftover,
                *response.content_length,
                collect);
        } else {
            co_await nxt::io::http::read_until_eof(
                transport, body_buffer, head.leftover, collect);
        }

        co_return body;
    };

    if (!response_status_is_success(response.status_line)) {
        auto body = co_await read_error_body();
        throw protocol_error{
            "OpenAI Responses HTTP error: "
            + std::string{response.status_line} + ": " + body};
    }

    auto sse = nxt::http::server_sent_event_parser{};
    auto on_chunk = [&](std::span<const std::byte> chunk) -> nxt::task<> {
        if (co_await emit_sse_events(
                sse.feed(nxt::io::http::as_text(chunk)), on_event))
            throw stream_complete{};
    };

    try {
        if (response.chunked) {
            co_await nxt::io::http::read_chunked(
                transport, line_buffer, body_buffer, head.leftover, on_chunk);
        } else if (response.content_length) {
            co_await nxt::io::http::read_content_length(
                transport,
                body_buffer,
                head.leftover,
                *response.content_length,
                on_chunk);
        } else {
            co_await nxt::io::http::read_until_eof(
                transport, body_buffer, head.leftover, on_chunk);
        }
    } catch (const stream_complete &) {
        co_return;
    }

    co_await emit_sse_events(sse.close(), on_event);
}

} // namespace nxt::io::llm
