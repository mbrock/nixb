#include <nxtio/http.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nxtio/async.hpp>

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

private:
    std::vector<std::string_view> chunks_;
    std::size_t chunk_ = 0;
    std::size_t offset_ = 0;
};

suite http_io_tests = [] {
    using namespace nxt::io;
    using namespace std::literals;

    "http slurping"_test = [] {
        http::parse_buffer input(
            "HTTP/1.1 200 Hello\r\n"
            "Server: Comanche\r\n"
            "Content-Type: text/slop\r\n"
            "\r\n"
            "hello"sv);

        std::vector<std::string> headers;
        for (auto header : input.grab(http::slurp("\r\n\r\n"sv))
                               | std::views::transform(http::as_char)
                               | std::views::split("\r\n"sv))
            headers.emplace_back(header.begin(), header.end());

        expect(headers.size() == 3_ul);
        expect(headers[0] == "HTTP/1.1 200 Hello");
        expect(headers[1] == "Server: Comanche");
        expect(headers[2] == "Content-Type: text/slop");
    };

    "async grab refills until the slurp cut appears"_test = [] {
        scripted_transport transport{{
            "HTTP/1.1 200 Hello\r\nSer",
            "ver: Comanche\r\n\r\nhello",
        }};
        std::array<char, 4096> buffer{};

        auto head = nxt::sync_wait(
            http::async_grab(transport, buffer, http::slurp("\r\n\r\n"sv)));

        auto text = http::as_text(head.bytes);
        expect(text == "HTTP/1.1 200 Hello\r\nServer: Comanche");
        expect(http::as_text(head.leftover) == "hello");
    };

    "content-length reader yields leftover then refill chunks"_test = [] {
        scripted_transport transport{{
            "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhe",
            "llo world",
        }};
        std::array<char, 4096> head_buffer{};
        std::array<char, 4> body_buffer{};

        auto head = nxt::sync_wait(
            http::async_grab(
                transport, head_buffer, http::slurp("\r\n\r\n"sv)));

        expect(
            http::as_text(head.bytes)
            == "HTTP/1.1 200 OK\r\nContent-Length: 11");
        expect(http::as_text(head.leftover) == "he");

        std::string body;
        auto on_chunk =
            [&](std::span<const std::byte> chunk) -> nxt::task<> {
            body += http::as_text(chunk);
            co_return;
        };

        nxt::sync_wait(
            http::read_content_length(
                transport, body_buffer, head.leftover, 11, on_chunk));

        expect(body == "hello world");
    };

    "chunked reader yields chunks across leftover and refills"_test = [] {
        scripted_transport transport{{
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhe",
            "llo\r\n6\r\n world\r\n0\r\n\r\n",
        }};
        std::array<char, 4096> head_buffer{};
        std::array<char, 64> line_buffer{};
        std::array<char, 3> body_buffer{};

        auto head = nxt::sync_wait(
            http::async_grab(
                transport, head_buffer, http::slurp("\r\n\r\n"sv)));

        expect(
            http::as_text(head.bytes)
            == "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
        expect(http::as_text(head.leftover) == "5\r\nhe");

        std::string body;
        auto on_chunk =
            [&](std::span<const std::byte> chunk) -> nxt::task<> {
            body += http::as_text(chunk);
            co_return;
        };

        nxt::sync_wait(
            http::read_chunked(
                transport,
                line_buffer,
                body_buffer,
                head.leftover,
                on_chunk));

        expect(body == "hello world");
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
