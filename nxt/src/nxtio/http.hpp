#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "nxtio/async.hpp"

namespace nxt::io::http {

struct protocol_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct url
{
    bool tls = false;
    std::string host;
    std::string port;
    std::string target = "/";
};

struct response_head
{
    std::string_view status_line;
    std::optional<std::size_t> content_length;
    bool chunked = false;
};

url parse_url(std::string_view text);
std::uint16_t parse_port(std::string_view text);
bool is_default_port(const url & url);
response_head parse_response_head(std::span<const std::byte> bytes);
std::string_view as_text(std::span<const std::byte> bytes);
char as_char(std::byte byte);

template<typename F>
concept byte_slicer = requires(F slicer, std::span<const std::byte> bytes) {
    { slicer(bytes) } -> std::same_as<std::span<const std::byte>>;
    { slicer.kerf() } -> std::convertible_to<std::size_t>;
};

class parse_buffer
{
public:
    explicit parse_buffer(std::span<const std::byte> text)
        : rest_(text)
    {
    }

    explicit parse_buffer(std::string_view sv)
        : rest_(std::as_bytes(std::span(sv)))
    {
    }

    [[nodiscard]] std::span<const std::byte> remaining() const noexcept
    {
        return rest_;
    }

    void consume(std::size_t n)
    {
        if (n > rest_.size())
            throw protocol_error{"parser consumed past end of input"};
        rest_ = rest_.subspan(n);
    }

    template<byte_slicer Slicer>
    std::span<const std::byte> grab(const Slicer & slicer)
    {
        auto slice = slicer(rest_);
        if (slice.data() != rest_.data() || slice.size() > rest_.size())
            throw protocol_error{"slicer returned bytes outside input"};
        if (slice.size() + slicer.kerf() > rest_.size())
            throw protocol_error{"slicer delimiter was absent"};
        consume(slice.size() + slicer.kerf());
        return slice;
    }

private:
    std::span<const std::byte> rest_;
};

class slurper
{
public:
    explicit slurper(std::span<const std::byte> needle);
    [[nodiscard]] std::span<const std::byte>
    operator()(std::span<const std::byte> bytes) const;
    std::size_t kerf() const;

private:
    std::span<const std::byte> needle_;
};

slurper slurp(std::string_view needle);

class bytes_as_chars
{
public:
    std::string_view operator()(std::span<const std::byte> bytes) const
    {
        return as_text(bytes);
    }
};

struct grab_result
{
    std::span<const std::byte> bytes;
    std::span<const std::byte> leftover;
};

template<typename Transport, byte_slicer Slicer>
nxt::task<grab_result> async_grab(
    Transport & transport,
    std::span<char> buffer,
    std::span<const std::byte> initial,
    const Slicer & slicer)
{
    if (initial.size() > buffer.size())
        throw protocol_error{"initial input exceeded grab buffer"};

    if (!initial.empty())
        std::memmove(buffer.data(), initial.data(), initial.size());
    auto used = initial.size();
    while (true) {
        auto bytes = std::as_bytes(buffer.first(used));
        if (slicer(bytes).size() + slicer.kerf() <= bytes.size()) {
            parse_buffer input{bytes};
            auto grabbed = input.grab(slicer);
            co_return grab_result{
                .bytes = grabbed,
                .leftover = input.remaining(),
            };
        }

        if (used == buffer.size())
            throw protocol_error{"buffer filled before delimiter"};

        auto n = co_await transport.read_some(buffer.subspan(used));
        if (n == 0)
            throw protocol_error{"unexpected end of input"};
        if (n > buffer.size() - used)
            throw protocol_error{"transport overfilled read buffer"};

        used += n;
    }
}

template<typename Transport, byte_slicer Slicer>
nxt::task<grab_result> async_grab(
    Transport & transport, std::span<char> buffer, const Slicer & slicer)
{
    return async_grab(
        transport, buffer, std::span<const std::byte>{}, slicer);
}

template<typename Transport, typename OnChunk>
nxt::task<> read_content_length(
    Transport & transport,
    std::span<char> buffer,
    std::span<const std::byte> initial,
    std::size_t content_length,
    OnChunk on_chunk)
{
    if (buffer.empty() && content_length > initial.size())
        throw protocol_error{"empty body read buffer"};

    auto remaining = content_length;
    if (!initial.empty() && remaining > 0) {
        auto n = std::min(remaining, initial.size());
        co_await on_chunk(initial.first(n));
        remaining -= n;
    }

    while (remaining > 0) {
        auto request = buffer.first(std::min(buffer.size(), remaining));
        auto n = co_await transport.read_some(request);
        if (n == 0)
            throw protocol_error{"unexpected end of input"};
        if (n > request.size())
            throw protocol_error{"transport overfilled read buffer"};

        co_await on_chunk(std::as_bytes(request.first(n)));
        remaining -= n;
    }
}

std::size_t parse_chunk_size(std::span<const std::byte> line);

template<typename Transport>
nxt::task<std::span<const std::byte>> read_expected_crlf(
    Transport & transport,
    std::span<char> line_buffer,
    std::span<const std::byte> initial)
{
    auto crlf =
        co_await async_grab(transport, line_buffer, initial, slurp("\r\n"));
    if (!crlf.bytes.empty())
        throw protocol_error{"chunk data was not followed by CRLF"};

    co_return crlf.leftover;
}

template<typename Transport, typename OnChunk>
nxt::task<> read_chunked(
    Transport & transport,
    std::span<char> line_buffer,
    std::span<char> body_buffer,
    std::span<const std::byte> initial,
    OnChunk on_chunk)
{
    if (body_buffer.empty())
        throw protocol_error{"empty chunk read buffer"};

    auto pending = initial;
    while (true) {
        auto line = co_await async_grab(
            transport, line_buffer, pending, slurp("\r\n"));
        auto chunk_size = parse_chunk_size(line.bytes);
        pending = line.leftover;

        if (chunk_size == 0) {
            auto trailers = co_await async_grab(
                transport, line_buffer, pending, slurp("\r\n"));
            if (!trailers.bytes.empty())
                throw protocol_error{"chunk trailers are not supported"};
            co_return;
        }

        auto remaining = chunk_size;
        if (!pending.empty()) {
            auto n = std::min(remaining, pending.size());
            co_await on_chunk(pending.first(n));
            pending = pending.subspan(n);
            remaining -= n;
        }

        while (remaining > 0) {
            auto request =
                body_buffer.first(std::min(body_buffer.size(), remaining));
            auto n = co_await transport.read_some(request);
            if (n == 0)
                throw protocol_error{"unexpected end of input"};
            if (n > request.size())
                throw protocol_error{"transport overfilled read buffer"};

            co_await on_chunk(std::as_bytes(request.first(n)));
            remaining -= n;
        }

        pending =
            co_await read_expected_crlf(transport, line_buffer, pending);
    }
}

template<typename Transport, typename OnChunk>
nxt::task<> read_until_eof(
    Transport & transport,
    std::span<char> buffer,
    std::span<const std::byte> initial,
    OnChunk on_chunk)
{
    if (!initial.empty())
        co_await on_chunk(initial);

    while (true) {
        auto n = co_await transport.read_some(buffer);
        if (n == 0)
            co_return;
        co_await on_chunk(std::as_bytes(buffer.first(n)));
    }
}

} // namespace nxt::io::http
