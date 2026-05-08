#include <nxt/http.hpp>

#include <boost/ut.hpp>
#include <iterator>
#include <string>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

std::vector<http::event> feed_all(
    http::response_parser & parser, std::vector<std::string_view> chunks)
{
    std::vector<http::event> events;
    for (auto chunk : chunks) {
        auto next = parser.feed(chunk);
        events.insert(
            events.end(),
            std::make_move_iterator(next.begin()),
            std::make_move_iterator(next.end()));
    }
    return events;
}

std::string body_from(const std::vector<http::event> & events)
{
    std::string body;
    for (const auto & ev : events) {
        if (ev.kind == http::event_kind::body)
            body += ev.body;
    }
    return body;
}

std::vector<http::server_sent_event>
sse_from(const std::vector<http::event> & events)
{
    std::vector<http::server_sent_event> out;
    for (const auto & ev : events) {
        if (ev.kind == http::event_kind::sse)
            out.push_back(ev.sse);
    }
    return out;
}

suite http_tests = [] {
    "serializes minimal request"_test = [] {
        http::request req{
            .method = "POST",
            .target = "/v1/chat/completions",
            .host = "api.example.test",
            .headers = {
                {"Authorization", "Bearer token"},
                {"Content-Type", "application/json"},
            },
            .body = R"({"stream":true})",
        };

        auto wire = http::serialize(req);
        expect(wire == std::string{
                           "POST /v1/chat/completions HTTP/1.1\r\n"
                           "Host: api.example.test\r\n"
                           "Authorization: Bearer token\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: 15\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           R"({"stream":true})"});
    };

    "parses content-length response across arbitrary boundaries"_test = [] {
        http::response_parser parser;
        auto events = feed_all(
            parser,
            {
                "HTTP/1.1 200",
                " OK\r\nContent-Length: 11\r\nX-Test: yes\r\n\r\nhe",
                "llo world",
            });

        expect(events.size() == 4_ul);
        expect(events[0].kind == http::event_kind::response_head);
        expect(events[0].response.status == 200_i);
        expect(events[0].response.reason == "OK");
        expect(events[0].response.headers.size() == 2_ul);
        expect(body_from(events) == "hello world");
        expect(events.back().kind == http::event_kind::complete);
        expect(parser.done());
    };

    "parses chunked response and strips chunk framing"_test = [] {
        http::response_parser parser;
        auto events = feed_all(
            parser,
            {
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
                "5\r\nhe",
                "llo\r\n6;ignored=true\r\n world\r\n",
                "0\r\n\r\n",
            });

        expect(events.size() == 4_ul);
        expect(events[0].kind == http::event_kind::response_head);
        expect(body_from(events) == "hello world");
        expect(events.back().kind == http::event_kind::complete);
        expect(parser.done());
    };

    "ignores chunk trailers"_test = [] {
        http::response_parser parser;
        auto events = feed_all(
            parser,
            {
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
                "1\r\nx\r\n0\r\n",
                "X-Trailer: ok\r\n\r\n",
            });

        expect(body_from(events) == "x");
        expect(events.back().kind == http::event_kind::complete);
        expect(parser.done());
    };

    "streams body until eof when no length is present"_test = [] {
        http::response_parser parser;
        auto events = feed_all(
            parser,
            {
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n",
                "data: one\n\n",
                "data: two\n\n",
            });

        expect(!parser.done());
        auto closing = parser.close();
        events.insert(
            events.end(),
            std::make_move_iterator(closing.begin()),
            std::make_move_iterator(closing.end()));

        expect(body_from(events) == "data: one\n\ndata: two\n\n");
        expect(events.back().kind == http::event_kind::complete);
        expect(parser.done());
    };

    "parses sse response body across arbitrary boundaries"_test = [] {
        http::response_parser parser{http::response_body_mode::sse};
        auto events = feed_all(
            parser,
            {
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n",
                ": keepalive\n",
                "event: build\nid: 7\ndata: start",
                "\ndata: line two\n\n",
                "data: done\n\n",
            });

        expect(!parser.done());
        auto closing = parser.close();
        events.insert(
            events.end(),
            std::make_move_iterator(closing.begin()),
            std::make_move_iterator(closing.end()));

        auto sse = sse_from(events);
        expect(sse.size() == 2_ul);
        expect(sse[0].type == "build");
        expect(sse[0].id == "7");
        expect(sse[0].data == "start\nline two");
        expect(sse[1].type == "message");
        expect(sse[1].id == "7");
        expect(sse[1].data == "done");
        expect(body_from(events).empty());
        expect(events.back().kind == http::event_kind::complete);
        expect(parser.done());
    };

    "parses chunked sse response after removing chunk framing"_test = [] {
        http::response_parser parser{http::response_body_mode::sse};
        auto events = feed_all(
            parser,
            {
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
                "b\r\ndata: one\n\n\r\n",
                "b\r\ndata: two\n\n\r\n",
                "0\r\n\r\n",
            });

        auto sse = sse_from(events);
        expect(sse.size() == 2_ul);
        expect(sse[0].data == "one");
        expect(sse[1].data == "two");
        expect(events.back().kind == http::event_kind::complete);
        expect(parser.done());
    };

    "flushes final sse response event at eof"_test = [] {
        http::response_parser parser{http::response_body_mode::sse};
        auto events = feed_all(
            parser,
            {
                "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n",
                "data: final",
            });

        auto sse = sse_from(events);
        expect(sse.size() == 1_ul);
        expect(sse[0].data == "final");
        expect(events.back().kind == http::event_kind::complete);
        expect(parser.done());
    };

    "rejects malformed content length"_test = [] {
        http::response_parser parser;
        bool caught = false;
        try {
            (void)parser.feed("HTTP/1.1 200 OK\r\nContent-Length: wat\r\n\r\n");
        } catch (const http::parse_error &) {
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
