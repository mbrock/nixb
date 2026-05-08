#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::http {

struct header
{
    std::string name;
    std::string value;
};

struct response_head
{
    std::string version;
    int status = 0;
    std::string reason;
    std::vector<header> headers;
};

struct request
{
    std::string method = "GET";
    std::string target = "/";
    std::string host;
    std::vector<header> headers;
    std::string body;
};

enum class response_body_mode
{
    bytes,
    sse,
};

struct server_sent_event
{
    std::string type = "message";
    std::string data;
    std::string id;
    std::optional<int> retry_ms;
};

enum class event_kind
{
    response_head,
    body,
    sse,
    complete,
};

struct event
{
    event_kind kind;
    nxt::http::response_head response;
    std::string body;
    server_sent_event sse;
};

struct parse_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class server_sent_event_parser
{
public:
    [[nodiscard]] std::vector<server_sent_event> feed(std::string_view body)
    {
        std::vector<server_sent_event> events;
        for (auto c : body) {
            if (c == '\r') {
                process_line(events, line_);
                line_.clear();
                previous_was_cr_ = true;
                continue;
            }

            if (c == '\n') {
                if (previous_was_cr_) {
                    previous_was_cr_ = false;
                    continue;
                }

                process_line(events, line_);
                line_.clear();
                continue;
            }

            previous_was_cr_ = false;
            line_ += c;
        }

        return events;
    }

    [[nodiscard]] std::vector<server_sent_event> close()
    {
        std::vector<server_sent_event> events;
        previous_was_cr_ = false;
        if (!line_.empty()) {
            process_line(events, line_);
            line_.clear();
        }
        dispatch(events);
        return events;
    }

private:
    void
    process_line(std::vector<server_sent_event> & events, std::string_view line)
    {
        if (line.empty()) {
            dispatch(events);
            return;
        }

        if (line.front() == ':')
            return;

        auto colon = line.find(':');
        auto field = colon == std::string_view::npos ? line
                                                     : line.substr(0, colon);
        auto value = colon == std::string_view::npos
                         ? std::string_view{}
                         : line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ')
            value.remove_prefix(1);

        if (field == "data") {
            pending_.data += value;
            pending_.data += '\n';
        } else if (field == "event") {
            pending_.type = value.empty() ? "message" : std::string{value};
        } else if (field == "id") {
            if (value.find('\0') == std::string_view::npos) {
                pending_.id = value;
                pending_has_id_ = true;
            }
        } else if (field == "retry") {
            auto retry = 0;
            auto * first = value.data();
            auto * last = value.data() + value.size();
            auto [ptr, ec] = std::from_chars(first, last, retry);
            if (!value.empty() && ec == std::errc{} && ptr == last)
                pending_.retry_ms = retry;
        }
    }

    void dispatch(std::vector<server_sent_event> & events)
    {
        if (pending_has_id_)
            last_id_ = pending_.id;

        if (!pending_.data.empty()) {
            if (pending_.data.back() == '\n')
                pending_.data.pop_back();

            if (!pending_has_id_)
                pending_.id = last_id_;

            events.push_back(pending_);
        }

        pending_ = server_sent_event{};
        pending_has_id_ = false;
    }

    std::string line_;
    bool previous_was_cr_ = false;
    server_sent_event pending_;
    bool pending_has_id_ = false;
    std::string last_id_;
};

class response_parser
{
public:
    explicit response_parser(
        response_body_mode body_mode = response_body_mode::bytes)
        : body_mode_(body_mode)
    {
    }

    [[nodiscard]] std::vector<event> feed(std::string_view bytes)
    {
        if (state_ == state::done && !bytes.empty())
            throw parse_error{"response parser received bytes after completion"};

        buffer_.append(bytes);

        std::vector<event> events;
        while (advance(events)) {
        }
        return events;
    }

    [[nodiscard]] std::vector<event> close()
    {
        std::vector<event> events;

        switch (state_) {
        case state::until_eof:
            if (!buffer_.empty()) {
                emit_body(events, std::exchange(buffer_, {}));
            }
            finish(events);
            break;

        case state::done:
            break;

        default:
            throw parse_error{"incomplete HTTP response"};
        }

        return events;
    }

    [[nodiscard]] bool done() const noexcept
    {
        return state_ == state::done;
    }

private:
    enum class state
    {
        status_line,
        headers,
        fixed_body,
        chunk_size,
        chunk_data,
        chunk_crlf,
        chunk_trailer,
        until_eof,
        done,
    };

    [[nodiscard]] bool advance(std::vector<event> & events)
    {
        switch (state_) {
        case state::status_line:
            return parse_status_line();

        case state::headers:
            return parse_header_line(events);

        case state::fixed_body:
            return parse_fixed_body(events);

        case state::chunk_size:
            return parse_chunk_size();

        case state::chunk_data:
            return parse_chunk_data(events);

        case state::chunk_crlf:
            return parse_chunk_crlf();

        case state::chunk_trailer:
            return parse_chunk_trailer(events);

        case state::until_eof:
            return parse_until_eof(events);

        case state::done:
            return false;
        }

        return false;
    }

    [[nodiscard]] bool parse_status_line()
    {
        auto line = take_line();
        if (!line)
            return false;

        auto first_space = line->find(' ');
        if (first_space == std::string::npos)
            throw parse_error{"malformed HTTP status line"};

        auto second_space = line->find(' ', first_space + 1);
        if (second_space == std::string::npos)
            throw parse_error{"malformed HTTP status line"};

        head_.version = line->substr(0, first_space);
        auto status_text = std::string_view{*line}.substr(
            first_space + 1, second_space - first_space - 1);

        int status = 0;
        auto * first = status_text.data();
        auto * last = status_text.data() + status_text.size();
        auto [ptr, ec] = std::from_chars(first, last, status);
        if (ec != std::errc{} || ptr != last)
            throw parse_error{"malformed HTTP status code"};

        head_.status = status;
        head_.reason = line->substr(second_space + 1);
        state_ = state::headers;
        return true;
    }

    [[nodiscard]] bool parse_header_line(std::vector<event> & events)
    {
        auto line = take_line();
        if (!line)
            return false;

        if (line->empty()) {
            choose_body_state();
            events.push_back(event{
                .kind = event_kind::response_head,
                .response = head_,
                .body = {},
                .sse = {},
            });

            if (state_ == state::done)
                events.push_back(event{
                    .kind = event_kind::complete,
                    .response = {},
                    .body = {},
                    .sse = {},
                });

            return true;
        }

        auto colon = line->find(':');
        if (colon == std::string::npos)
            throw parse_error{"malformed HTTP header"};

        head_.headers.push_back(header{
            .name = trim(line->substr(0, colon)),
            .value = trim(line->substr(colon + 1)),
        });
        return true;
    }

    [[nodiscard]] bool parse_fixed_body(std::vector<event> & events)
    {
        if (remaining_ == 0) {
            finish(events);
            return true;
        }

        if (buffer_.empty())
            return false;

        auto n = std::min(remaining_, buffer_.size());
        emit_body(events, take_bytes(n));
        remaining_ -= n;

        if (remaining_ == 0)
            finish(events);

        return true;
    }

    [[nodiscard]] bool parse_chunk_size()
    {
        auto line = take_line();
        if (!line)
            return false;

        auto semi = line->find(';');
        auto size_text = trim(line->substr(0, semi));
        if (size_text.empty())
            throw parse_error{"malformed chunk size"};

        std::size_t size = 0;
        auto * first = size_text.data();
        auto * last = size_text.data() + size_text.size();
        auto [ptr, ec] = std::from_chars(first, last, size, 16);
        if (ec != std::errc{} || ptr != last)
            throw parse_error{"malformed chunk size"};

        if (size == 0) {
            state_ = state::chunk_trailer;
            return true;
        }

        remaining_ = size;
        state_ = state::chunk_data;
        return true;
    }

    [[nodiscard]] bool parse_chunk_data(std::vector<event> & events)
    {
        if (buffer_.size() < remaining_)
            return false;

        emit_body(events, take_bytes(remaining_));
        remaining_ = 0;
        state_ = state::chunk_crlf;
        return true;
    }

    [[nodiscard]] bool parse_chunk_crlf()
    {
        if (buffer_.size() < 2)
            return false;

        if (buffer_[0] != '\r' || buffer_[1] != '\n')
            throw parse_error{"malformed chunk terminator"};

        buffer_.erase(0, 2);
        state_ = state::chunk_size;
        return true;
    }

    [[nodiscard]] bool parse_chunk_trailer(std::vector<event> & events)
    {
        auto line = take_line();
        if (!line)
            return false;

        if (line->empty())
            finish(events);

        return true;
    }

    [[nodiscard]] bool parse_until_eof(std::vector<event> & events)
    {
        if (buffer_.empty())
            return false;

        emit_body(events, std::exchange(buffer_, {}));
        return false;
    }

    void choose_body_state()
    {
        if (has_header_token("transfer-encoding", "chunked")) {
            state_ = state::chunk_size;
            return;
        }

        if (auto content_length = header_value("content-length")) {
            remaining_ = parse_decimal_size(*content_length);
            state_ = remaining_ == 0 ? state::done : state::fixed_body;
            return;
        }

        state_ = state::until_eof;
    }

    void finish(std::vector<event> & events)
    {
        if (body_mode_ == response_body_mode::sse)
            flush_sse(events);

        state_ = state::done;
        events.push_back(event{
            .kind = event_kind::complete,
            .response = {},
            .body = {},
            .sse = {},
        });
    }

    void emit_body(std::vector<event> & events, std::string body)
    {
        if (body.empty())
            return;

        if (body_mode_ == response_body_mode::bytes) {
            events.push_back(event{
                .kind = event_kind::body,
                .response = {},
                .body = std::move(body),
                .sse = {},
            });
            return;
        }

        feed_sse(events, body);
    }

    void feed_sse(std::vector<event> & events, std::string_view body)
    {
        for (auto c : body) {
            if (c == '\r') {
                process_sse_line(events, sse_line_);
                sse_line_.clear();
                sse_previous_was_cr_ = true;
                continue;
            }

            if (c == '\n') {
                if (sse_previous_was_cr_) {
                    sse_previous_was_cr_ = false;
                    continue;
                }

                process_sse_line(events, sse_line_);
                sse_line_.clear();
                continue;
            }

            sse_previous_was_cr_ = false;
            sse_line_ += c;
        }
    }

    void flush_sse(std::vector<event> & events)
    {
        sse_previous_was_cr_ = false;
        if (!sse_line_.empty()) {
            process_sse_line(events, sse_line_);
            sse_line_.clear();
        }
        dispatch_sse(events);
    }

    void
    process_sse_line(std::vector<event> & events, std::string_view line)
    {
        if (line.empty()) {
            dispatch_sse(events);
            return;
        }

        if (line.front() == ':')
            return;

        auto colon = line.find(':');
        auto field = colon == std::string_view::npos ? line
                                                     : line.substr(0, colon);
        auto value = colon == std::string_view::npos
                         ? std::string_view{}
                         : line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ')
            value.remove_prefix(1);

        if (field == "data") {
            pending_sse_.data += value;
            pending_sse_.data += '\n';
        } else if (field == "event") {
            pending_sse_.type = value.empty() ? "message" : std::string{value};
        } else if (field == "id") {
            if (value.find('\0') == std::string_view::npos) {
                pending_sse_.id = value;
                pending_sse_has_id_ = true;
            }
        } else if (field == "retry") {
            auto retry = 0;
            auto * first = value.data();
            auto * last = value.data() + value.size();
            auto [ptr, ec] = std::from_chars(first, last, retry);
            if (!value.empty() && ec == std::errc{} && ptr == last)
                pending_sse_.retry_ms = retry;
        }
    }

    void dispatch_sse(std::vector<event> & events)
    {
        if (pending_sse_has_id_)
            last_sse_id_ = pending_sse_.id;

        if (!pending_sse_.data.empty()) {
            if (pending_sse_.data.back() == '\n')
                pending_sse_.data.pop_back();

            if (!pending_sse_has_id_)
                pending_sse_.id = last_sse_id_;

            events.push_back(event{
                .kind = event_kind::sse,
                .response = {},
                .body = {},
                .sse = pending_sse_,
            });
        }

        pending_sse_ = server_sent_event{};
        pending_sse_has_id_ = false;
    }

    [[nodiscard]] std::optional<std::string> take_line()
    {
        auto pos = buffer_.find("\r\n");
        if (pos == std::string::npos)
            return std::nullopt;

        auto line = buffer_.substr(0, pos);
        buffer_.erase(0, pos + 2);
        return line;
    }

    [[nodiscard]] std::string take_bytes(std::size_t n)
    {
        auto bytes = buffer_.substr(0, n);
        buffer_.erase(0, n);
        return bytes;
    }

    [[nodiscard]] std::optional<std::string_view>
    header_value(std::string_view name) const
    {
        for (const auto & h : head_.headers) {
            if (iequals(h.name, name))
                return h.value;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool
    has_header_token(std::string_view name, std::string_view token) const
    {
        auto value = header_value(name);
        if (!value)
            return false;

        std::string_view rest = *value;
        while (true) {
            auto comma = rest.find(',');
            auto part = trim(rest.substr(0, comma));
            if (iequals(part, token))
                return true;
            if (comma == std::string_view::npos)
                return false;
            rest.remove_prefix(comma + 1);
        }
    }

    [[nodiscard]] static std::size_t parse_decimal_size(std::string_view text)
    {
        text = trim(text);
        std::size_t value = 0;
        auto * first = text.data();
        auto * last = text.data() + text.size();
        auto [ptr, ec] = std::from_chars(first, last, value);
        if (text.empty() || ec != std::errc{} || ptr != last)
            throw parse_error{"malformed content-length"};
        return value;
    }

    [[nodiscard]] static std::string trim(std::string_view text)
    {
        while (!text.empty()
               && std::isspace(static_cast<unsigned char>(text.front())))
            text.remove_prefix(1);
        while (!text.empty()
               && std::isspace(static_cast<unsigned char>(text.back())))
            text.remove_suffix(1);
        return std::string{text};
    }

    [[nodiscard]] static bool
    iequals(std::string_view lhs, std::string_view rhs)
    {
        return lhs.size() == rhs.size()
            && std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), [](
                             char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a))
                       == std::tolower(static_cast<unsigned char>(b));
               });
    }

    state state_ = state::status_line;
    response_body_mode body_mode_ = response_body_mode::bytes;
    std::string buffer_;
    response_head head_;
    std::size_t remaining_ = 0;
    std::string sse_line_;
    bool sse_previous_was_cr_ = false;
    server_sent_event pending_sse_;
    bool pending_sse_has_id_ = false;
    std::string last_sse_id_;
};

[[nodiscard]] inline std::string serialize(const request & req)
{
    std::string out;
    out += req.method;
    out += ' ';
    out += req.target;
    out += " HTTP/1.1\r\n";

    if (!req.host.empty()) {
        out += "Host: ";
        out += req.host;
        out += "\r\n";
    }

    bool has_content_length = false;
    bool has_connection = false;
    auto header_name_is = [](std::string_view lhs, std::string_view rhs) {
        return lhs.size() == rhs.size()
            && std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), [](
                             char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a))
                       == std::tolower(static_cast<unsigned char>(b));
               });
    };

    for (const auto & h : req.headers) {
        has_content_length =
            has_content_length || header_name_is(h.name, "content-length");
        has_connection =
            has_connection || header_name_is(h.name, "connection");

        out += h.name;
        out += ": ";
        out += h.value;
        out += "\r\n";
    }

    if (!has_content_length) {
        out += "Content-Length: ";
        out += std::to_string(req.body.size());
        out += "\r\n";
    }

    if (!has_connection)
        out += "Connection: close\r\n";

    out += "\r\n";
    out += req.body;
    return out;
}

} // namespace nxt::http
