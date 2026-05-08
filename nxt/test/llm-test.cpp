#include <nxtio/llm.hpp>

#include <boost/ut.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

class scripted_transport
{
public:
    explicit scripted_transport(std::vector<std::string_view> chunks)
        : chunks_(std::move(chunks))
    {
    }

    nxt::task<std::size_t> read_some(std::span<char> dst)
    {
        if (chunk_ == chunks_.size())
            co_return 0;

        auto src = chunks_[chunk_];
        auto rest = src.substr(offset_);
        auto n = std::min(dst.size(), rest.size());
        std::ranges::copy(rest.substr(0, n), dst.begin());

        offset_ += n;
        if (offset_ == src.size()) {
            ++chunk_;
            offset_ = 0;
        }

        co_return n;
    }

    nxt::task<> write_all(std::string_view bytes)
    {
        written_ += bytes;
        co_return;
    }

    [[nodiscard]] const std::string & written() const noexcept
    {
        return written_;
    }

private:
    std::vector<std::string_view> chunks_;
    std::size_t chunk_ = 0;
    std::size_t offset_ = 0;
    std::string written_;
};

suite llm_tests = [] {
    using namespace std::literals;

    "openai responses request serializes response body"_test = [] {
        auto request = nxt::io::llm::openai_responses_request{
            .api_key = "test-key",
            .model = "gpt-5-mini",
            .input = "Say ok.",
            .max_output_tokens = 64,
            .reasoning_effort = "minimal",
            .store = false,
        };

        auto wire = nxt::io::llm::openai_responses_http_request(request);
        auto body_start = wire.find("\r\n\r\n");
        expect(body_start != std::string::npos);

        auto body = nlohmann::json::parse(wire.substr(body_start + 4));
        expect(body["model"] == "gpt-5-mini");
        expect(body["input"] == "Say ok.");
        expect(body["stream"] == true);
        expect(body["store"] == false);
        expect(body["reasoning"]["effort"] == "minimal");
        expect(wire.find("Authorization: Bearer test-key\r\n") != std::string::npos);
    };

    "openai responses stream emits parsed sse json events"_test = [] {
        auto sse =
            "event: response.output_text.delta\n"
            "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Ok\"}\n"
            "\n"
            "event: response.completed\n"
            "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n"
            "\n"s;

        auto response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: "
            + std::to_string(sse.size()) + "\r\n\r\n" + sse;
        scripted_transport transport{{response}};

        std::vector<nxt::io::llm::stream_event> events;
        auto on_event = [&](nxt::io::llm::stream_event event) -> nxt::task<> {
            events.push_back(std::move(event));
            co_return;
        };

        nxt::sync_wait(nxt::io::llm::stream_openai_responses_over(
            transport,
            nxt::io::llm::openai_responses_request{
                .api_key = "test-key",
                .model = "gpt-5-mini",
                .input = "Say ok.",
                .max_output_tokens = 64,
            },
            on_event));

        expect(events.size() == 2_ul);
        expect(events[0].type == "response.output_text.delta");
        expect(events[0].payload["delta"] == "Ok");
        expect(events[1].type == "response.completed");
        expect(transport.written().starts_with("POST /v1/responses HTTP/1.1\r\n"));
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
