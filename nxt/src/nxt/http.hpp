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

enum class event_kind
{
    response_head,
    body,
    complete,
};

struct event
{
    event_kind kind;
    nxt::http::response_head response;
    std::string body;
};

struct parse_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class response_parser
{
public:
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
                events.push_back(event{
                    .kind = event_kind::body,
                    .response = {},
                    .body = std::exchange(buffer_, {}),
                });
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
            });

            if (state_ == state::done)
                events.push_back(event{
                    .kind = event_kind::complete,
                    .response = {},
                    .body = {},
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
        events.push_back(event{
            .kind = event_kind::body,
            .response = {},
            .body = take_bytes(n),
        });
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

        events.push_back(event{
            .kind = event_kind::body,
            .response = {},
            .body = take_bytes(remaining_),
        });
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

        events.push_back(event{
            .kind = event_kind::body,
            .response = {},
            .body = std::exchange(buffer_, {}),
        });
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
        state_ = state::done;
        events.push_back(event{
            .kind = event_kind::complete,
            .response = {},
            .body = {},
        });
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
    std::string buffer_;
    response_head head_;
    std::size_t remaining_ = 0;
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
