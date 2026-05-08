#include <nxt/http-coro.hpp>

#include <boost/ut.hpp>
#include <array>
#include <string>
#include <variant>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

struct chunks
{
    std::vector<std::string_view> values;
    std::size_t next = 0;

    bool feed(http_coro::byte_stream & stream)
    {
        if (!stream.remaining().empty())
            return true;
        if (next == values.size())
            return false;

        stream.feed(values[next++]);
        return true;
    }
};

template<typename Yield, typename Return, typename OnYield>
Return drive(
    http_coro::machine<Yield, Return> & machine,
    http_coro::byte_stream & stream,
    chunks & input,
    OnYield on_yield,
    bool close_at_end = false)
{
    input.feed(stream);
    while (true) {
        auto step = machine.next(stream);

        if (step.kind == http_coro::step_kind::yielded) {
            on_yield(*step.yielded);
            continue;
        }

        if (step.kind == http_coro::step_kind::returned)
            return *step.returned;

        if (input.feed(stream))
            continue;

        if (close_at_end) {
            stream.close();
            close_at_end = false;
            continue;
        }

        throw std::runtime_error{
            "test input ended before coroutine returned"};
    }
}

http_coro::body_done drive_body(
    http_coro::machine<http_coro::body_chunk, http_coro::body_done> &
        machine,
    http_coro::byte_stream & stream,
    chunks & input,
    std::string & body,
    bool close_at_end = false)
{
    return drive(
        machine,
        stream,
        input,
        [&](http_coro::body_chunk chunk) { body += chunk.bytes; },
        close_at_end);
}

suite http_coro_tests = [] {
    "parses headers, then caller selects content-length decoder"_test = [] {
        std::array<char, 8192> scratch{};
        http_coro::byte_stream stream{scratch};
        chunks input{{
            "HTTP/1.1 200",
            " OK\r\nContent-Length: 11\r\nX-Test: yes\r\n\r\nhe",
            "llo world",
        }};

        auto headers = http_coro::parse_headers(stream);
        auto status = 0;
        auto header_count = 0;
        auto done = drive(
            headers, stream, input, [&](http_coro::header_event event) {
                if (auto * line =
                        std::get_if<http_coro::status_line>(&event)) {
                    status = line->status;
                    expect(line->reason == "OK");
                } else {
                    ++header_count;
                }
            });

        expect(status == 200_i);
        expect(header_count == 2_i);
        expect(
            done.body.kind == http_coro::body_framing_kind::content_length);
        expect(done.body.content_length == 11_ul);
        expect(done.leftover == "he");

        auto body_parser = http_coro::decode_content_length(
            stream, done.body.content_length);
        std::string body;
        auto body_done = drive_body(body_parser, stream, input, body);

        expect(body == "hello world");
        expect(body_done.leftover.empty());
    };

    "parses chunked body with a separate decoder"_test = [] {
        std::array<char, 8192> scratch{};
        http_coro::byte_stream stream{scratch};
        chunks input{{
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
            "5\r\nhe",
            "llo\r\n6;ignored=true\r\n world\r\n",
            "0\r\n\r\n",
        }};

        auto headers = http_coro::parse_headers(stream);
        auto done =
            drive(headers, stream, input, [](http_coro::header_event) {});

        expect(done.body.kind == http_coro::body_framing_kind::chunked);

        auto body_parser = http_coro::decode_chunked(stream);
        std::string body;
        auto body_done = drive_body(body_parser, stream, input, body);

        expect(body == "hello world");
        expect(body_done.leftover.empty());
    };

    "uses scratch only for split lines"_test = [] {
        std::array<char, 8192> scratch{};
        http_coro::byte_stream stream{scratch};
        chunks input{{
            "HTTP/1.1 20",
            "0 OK\r\nTransfer-",
            "Encoding: chunk",
            "ed\r\n\r\n",
            "b",
            "\r\nhello world\r\n0\r\n\r\n",
        }};

        auto headers = http_coro::parse_headers(stream);
        std::string transfer_encoding;
        auto done = drive(
            headers, stream, input, [&](http_coro::header_event event) {
                auto * field = std::get_if<http_coro::header_field>(&event);
                if (field && field->name == "Transfer-Encoding")
                    transfer_encoding = field->value;
            });

        expect(transfer_encoding == "chunked");
        expect(done.body.kind == http_coro::body_framing_kind::chunked);

        auto body_parser = http_coro::decode_chunked(stream);
        std::string body;
        (void) drive_body(body_parser, stream, input, body);

        expect(body == "hello world");
    };

    "streams until eof with a separate decoder"_test = [] {
        std::array<char, 8192> scratch{};
        http_coro::byte_stream stream{scratch};
        chunks input{{
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n",
            "data: one\n\n",
            "data: two\n\n",
        }};

        auto headers = http_coro::parse_headers(stream);
        auto done =
            drive(headers, stream, input, [](http_coro::header_event) {});

        expect(done.body.kind == http_coro::body_framing_kind::until_eof);

        auto body_parser = http_coro::decode_until_eof(stream);
        std::string body;
        auto body_done = drive_body(body_parser, stream, input, body, true);

        expect(body == "data: one\n\ndata: two\n\n");
        expect(body_done.leftover.empty());
    };

    "rejects malformed content length while observing headers"_test = [] {
        std::array<char, 8192> scratch{};
        http_coro::byte_stream stream{scratch};
        chunks input{{"HTTP/1.1 200 OK\r\nContent-Length: wat\r\n\r\n"}};
        auto headers = http_coro::parse_headers(stream);

        bool caught = false;
        try {
            (void) drive(
                headers, stream, input, [](http_coro::header_event) {});
        } catch (const http_coro::parse_error &) {
            caught = true;
        }

        expect(caught);
    };

    "rejects over-limit header values"_test = [] {
        std::array<char, 128> scratch{};
        http_coro::byte_stream stream{scratch};
        http_coro::limits limits{
            .max_status_line = 64,
            .max_header_name = 64,
            .max_header_value = 8,
            .max_header_line = 64,
            .max_header_bytes = 128,
            .max_chunk_line = 16,
        };
        chunks input{{"HTTP/1.1 200 OK\r\nX-Test: too-long-value\r\n\r\n"}};
        auto headers = http_coro::parse_headers(stream, limits);

        bool caught = false;
        try {
            (void) drive(
                headers, stream, input, [](http_coro::header_event) {});
        } catch (const http_coro::parse_error &) {
            caught = true;
        }

        expect(caught);
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
