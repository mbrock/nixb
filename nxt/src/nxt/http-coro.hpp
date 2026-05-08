#pragma once

#include <algorithm>
#include <charconv>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace nxt::http_coro {

struct limits
{
    std::size_t max_status_line = 1024;
    std::size_t max_header_name = 128;
    std::size_t max_header_value = 4096;
    std::size_t max_header_line = 8192;
    std::size_t max_header_bytes = 32768;
    std::size_t max_chunk_line = 128;
};

struct parse_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class byte_stream
{
public:
    // The stream borrows caller-owned storage. Yielded views point either
    // into the current input chunk or this scratch span, and are valid
    // until the next coroutine step or feed.
    explicit byte_stream(std::span<char> scratch)
        : scratch_(scratch)
    {
    }

    byte_stream(const byte_stream &) = delete;
    byte_stream & operator=(const byte_stream &) = delete;

    void feed(std::string_view bytes)
    {
        if (!input_.empty() && !bytes.empty())
            throw parse_error{
                "fed bytes before parser consumed prior input"};
        input_ = bytes;
    }

    void close() noexcept
    {
        closed_ = true;
    }

    [[nodiscard]] bool closed() const noexcept
    {
        return closed_;
    }

    [[nodiscard]] std::string_view remaining() const noexcept
    {
        return input_;
    }

    void begin_token() noexcept
    {
        scratch_used_ = 0;
    }

    void append_scratch(std::string_view bytes, std::size_t max_size)
    {
        if (bytes.empty())
            return;
        if (scratch_used_ + bytes.size() > max_size)
            throw parse_error{"HTTP token exceeds configured limit"};
        if (scratch_used_ + bytes.size() > scratch_.size())
            throw parse_error{"HTTP scratch buffer is too small"};

        std::ranges::copy(bytes, scratch_.begin() + scratch_used_);
        scratch_used_ += bytes.size();
    }

    [[nodiscard]] std::string_view scratch_view() const noexcept
    {
        return {scratch_.data(), scratch_used_};
    }

    [[nodiscard]] std::optional<std::string_view>
    take_line(std::size_t max_size)
    {
        auto pos = input_.find("\r\n");
        if (pos != std::string_view::npos) {
            if (scratch_used_ + pos > max_size)
                throw parse_error{"HTTP token exceeds configured limit"};

            std::string_view line;
            if (scratch_used_ == 0) {
                line = input_.substr(0, pos);
            } else {
                append_scratch(input_.substr(0, pos), max_size);
                line = scratch_view();
            }

            input_.remove_prefix(pos + 2);
            return line;
        }

        append_scratch(input_, max_size);
        input_ = {};
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string_view>
    take_some(std::size_t max_size)
    {
        if (input_.empty())
            return std::nullopt;

        auto n = std::min(max_size, input_.size());
        auto bytes = input_.substr(0, n);
        input_.remove_prefix(n);
        return bytes;
    }

    [[nodiscard]] std::coroutine_handle<> take_waiting() noexcept
    {
        return std::exchange(waiting_, {});
    }

private:
    friend struct more_awaitable;

    std::span<char> scratch_;
    std::string_view input_;
    std::coroutine_handle<> waiting_;
    bool closed_ = false;
    std::size_t scratch_used_ = 0;
};

struct more_awaitable
{
    byte_stream & stream;

    [[nodiscard]] bool await_ready() const noexcept
    {
        return !stream.input_.empty() || stream.closed_;
    }

    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        stream.waiting_ = handle;
    }

    void await_resume() const noexcept {}
};

[[nodiscard]] inline more_awaitable more(byte_stream & stream) noexcept
{
    return more_awaitable{stream};
}

template<typename T>
class value_task
{
public:
    struct promise_type
    {
        std::optional<T> value;
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        value_task get_return_object()
        {
            return value_task{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }

        auto final_suspend() noexcept
        {
            struct final_awaiter
            {
                [[nodiscard]] bool await_ready() noexcept
                {
                    return false;
                }

                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> handle) noexcept
                {
                    auto continuation = handle.promise().continuation;
                    return continuation ? continuation
                                        : std::noop_coroutine();
                }

                void await_resume() noexcept {}
            };

            return final_awaiter{};
        }

        void return_value(T next_value)
        {
            value = next_value;
        }

        void unhandled_exception() noexcept
        {
            exception = std::current_exception();
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit value_task(handle_type handle)
        : handle_(handle)
    {
    }

    value_task(value_task && other) noexcept
        : handle_(std::exchange(other.handle_, {}))
    {
    }

    value_task(const value_task &) = delete;
    value_task & operator=(const value_task &) = delete;

    ~value_task()
    {
        if (handle_)
            handle_.destroy();
    }

    [[nodiscard]] bool await_ready() const noexcept
    {
        return !handle_ || handle_.done();
    }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> continuation)
    {
        handle_.promise().continuation = continuation;
        return handle_;
    }

    T await_resume()
    {
        auto & promise = handle_.promise();
        if (promise.exception)
            std::rethrow_exception(promise.exception);
        return *promise.value;
    }

private:
    handle_type handle_;
};

inline value_task<std::string_view>
read_line(byte_stream & stream, std::size_t max_size)
{
    stream.begin_token();
    while (true) {
        if (auto line = stream.take_line(max_size))
            co_return *line;
        if (stream.closed())
            throw parse_error{"incomplete HTTP line"};
        co_await more(stream);
    }
}

inline value_task<std::string_view>
read_some(byte_stream & stream, std::size_t max_size)
{
    while (true) {
        if (auto bytes = stream.take_some(max_size))
            co_return *bytes;
        if (stream.closed())
            throw parse_error{"incomplete HTTP body"};
        co_await more(stream);
    }
}

enum class step_kind {
    need_input,
    yielded,
    returned,
};

template<typename Yield, typename Return>
struct step
{
    step_kind kind;
    std::optional<Yield> yielded;
    std::optional<Return> returned;
};

template<typename Yield, typename Return>
class machine
{
public:
    struct promise_type
    {
        std::optional<Yield> yielded;
        std::optional<Return> returned;
        std::exception_ptr exception;

        machine get_return_object()
        {
            return machine{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }

        std::suspend_always final_suspend() noexcept
        {
            return {};
        }

        std::suspend_always yield_value(Yield value) noexcept
        {
            yielded = value;
            return {};
        }

        void return_value(Return value)
        {
            returned = value;
        }

        void unhandled_exception() noexcept
        {
            exception = std::current_exception();
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit machine(handle_type handle)
        : handle_(handle)
    {
    }

    machine(machine && other) noexcept
        : handle_(std::exchange(other.handle_, {}))
    {
    }

    machine(const machine &) = delete;
    machine & operator=(const machine &) = delete;

    ~machine()
    {
        if (handle_)
            handle_.destroy();
    }

    [[nodiscard]] step<Yield, Return> next(byte_stream & stream)
    {
        auto resume = stream.take_waiting();
        if (!resume)
            resume = handle_;

        resume.resume();

        auto & promise = handle_.promise();
        if (promise.exception)
            std::rethrow_exception(std::exchange(promise.exception, {}));

        if (promise.yielded) {
            auto yielded = std::exchange(promise.yielded, {});
            return {
                .kind = step_kind::yielded,
                .yielded = std::move(yielded),
                .returned = std::nullopt,
            };
        }

        if (handle_.done()) {
            return {
                .kind = step_kind::returned,
                .yielded = std::nullopt,
                .returned = promise.returned,
            };
        }

        return {
            .kind = step_kind::need_input,
            .yielded = std::nullopt,
            .returned = std::nullopt,
        };
    }

private:
    handle_type handle_;
};

struct status_line
{
    std::string_view version;
    int status = 0;
    std::string_view reason;
};

struct header_field
{
    std::string_view name;
    std::string_view value;
};

using header_event = std::variant<status_line, header_field>;

enum class body_framing_kind {
    content_length,
    chunked,
    until_eof,
};

struct body_framing
{
    body_framing_kind kind = body_framing_kind::until_eof;
    std::size_t content_length = 0;
};

struct headers_done
{
    body_framing body;
    std::string_view leftover;
};

struct body_chunk
{
    std::string_view bytes;
};

struct body_done
{
    std::string_view leftover;
};

[[nodiscard]] inline std::string_view
trim_ows(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] inline char lower_ascii(char ch) noexcept
{
    if (ch >= 'A' && ch <= 'Z')
        return static_cast<char>(ch - 'A' + 'a');
    return ch;
}

[[nodiscard]] inline bool
ascii_iequals(std::string_view lhs, std::string_view rhs) noexcept
{
    return lhs.size() == rhs.size()
           && std::equal(
               lhs.begin(),
               lhs.end(),
               rhs.begin(),
               rhs.end(),
               [](char a, char b) {
                   return lower_ascii(a) == lower_ascii(b);
               });
}

[[nodiscard]] inline bool is_field_name_char(char ch) noexcept
{
    auto c = static_cast<unsigned char>(ch);
    return c > 32 && c < 127 && ch != ':' && ch != '\r' && ch != '\n';
}

[[nodiscard]] inline std::optional<header_field>
parse_header_field(std::string_view line, const limits & lim)
{
    auto colon = line.find(':');
    if (colon == std::string_view::npos)
        return std::nullopt;

    auto name = trim_ows(line.substr(0, colon));
    auto value = trim_ows(line.substr(colon + 1));
    if (name.empty() || name.size() > lim.max_header_name
        || !std::ranges::all_of(name, is_field_name_char))
        return std::nullopt;
    if (value.size() > lim.max_header_value)
        throw parse_error{"HTTP header value exceeds configured limit"};

    return header_field{
        .name = name,
        .value = value,
    };
}

[[nodiscard]] inline std::size_t
parse_size(std::string_view text, int base, std::string_view message)
{
    std::size_t value = 0;
    auto * first = text.data();
    auto * last = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(first, last, value, base);
    if (text.empty() || ec != std::errc{} || ptr != last)
        throw parse_error{std::string{message}};
    return value;
}

[[nodiscard]] inline status_line parse_status_line(std::string_view line)
{
    auto first_space = line.find(' ');
    if (first_space == std::string_view::npos)
        throw parse_error{"malformed HTTP status line"};

    auto second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos)
        throw parse_error{"malformed HTTP status line"};

    auto status_text =
        line.substr(first_space + 1, second_space - first_space - 1);
    auto status = 0;
    auto * first = status_text.data();
    auto * last = status_text.data() + status_text.size();
    auto [ptr, ec] = std::from_chars(first, last, status);
    if (ec != std::errc{} || ptr != last)
        throw parse_error{"malformed HTTP status code"};

    return status_line{
        .version = line.substr(0, first_space),
        .status = status,
        .reason = line.substr(second_space + 1),
    };
}

class framing_observer
{
public:
    void observe(header_field field)
    {
        if (ascii_iequals(field.name, "content-length")) {
            auto value = parse_size(
                trim_ows(field.value), 10, "malformed content-length");
            if (content_length_ && *content_length_ != value)
                throw parse_error{"conflicting content-length headers"};
            content_length_ = value;
            return;
        }

        if (ascii_iequals(field.name, "transfer-encoding")) {
            if (!ascii_iequals(trim_ows(field.value), "chunked"))
                throw parse_error{"unsupported transfer-encoding"};
            chunked_ = true;
        }
    }

    [[nodiscard]] body_framing body() const
    {
        if (chunked_ && content_length_)
            throw parse_error{
                "HTTP response has both transfer-encoding and content-length"};
        if (chunked_)
            return {
                .kind = body_framing_kind::chunked, .content_length = 0};
        if (content_length_)
            return {
                .kind = body_framing_kind::content_length,
                .content_length = *content_length_,
            };
        return {.kind = body_framing_kind::until_eof, .content_length = 0};
    }

private:
    std::optional<std::size_t> content_length_;
    bool chunked_ = false;
};

inline machine<header_event, headers_done>
parse_headers(byte_stream & stream, limits lim = {})
{
    auto status =
        parse_status_line(co_await read_line(stream, lim.max_status_line));
    co_yield header_event{status};

    framing_observer framing;
    std::size_t header_bytes = 0;
    while (true) {
        auto line = co_await read_line(stream, lim.max_header_line);
        header_bytes += line.size() + 2;
        if (header_bytes > lim.max_header_bytes)
            throw parse_error{"HTTP headers exceed configured limit"};

        if (line.empty()) {
            co_return headers_done{
                .body = framing.body(),
                .leftover = stream.remaining(),
            };
        }

        auto field = parse_header_field(line, lim);
        if (!field)
            throw parse_error{"malformed HTTP header"};

        framing.observe(*field);
        co_yield header_event{*field};
    }
}

inline machine<body_chunk, body_done>
decode_content_length(byte_stream & stream, std::size_t content_length)
{
    auto remaining = content_length;
    while (remaining > 0) {
        auto bytes = co_await read_some(stream, remaining);
        remaining -= bytes.size();
        co_yield body_chunk{.bytes = bytes};
    }

    co_return body_done{.leftover = stream.remaining()};
}

inline machine<body_chunk, body_done>
decode_chunked(byte_stream & stream, limits lim = {})
{
    while (true) {
        auto chunk_line = co_await read_line(stream, lim.max_chunk_line);
        auto semi = chunk_line.find(';');
        auto size_text = trim_ows(chunk_line.substr(0, semi));
        auto remaining = parse_size(size_text, 16, "malformed chunk size");

        if (remaining == 0) {
            while (true) {
                auto trailer =
                    co_await read_line(stream, lim.max_header_line);
                if (trailer.empty())
                    co_return body_done{.leftover = stream.remaining()};
            }
        }

        while (remaining > 0) {
            auto bytes = co_await read_some(stream, remaining);
            remaining -= bytes.size();
            co_yield body_chunk{.bytes = bytes};
        }

        if (!(co_await read_line(stream, 0)).empty())
            throw parse_error{"malformed chunk terminator"};
    }
}

inline machine<body_chunk, body_done> decode_until_eof(byte_stream & stream)
{
    while (true) {
        if (auto bytes = stream.take_some(static_cast<std::size_t>(-1))) {
            co_yield body_chunk{.bytes = *bytes};
            continue;
        }

        if (stream.closed())
            co_return body_done{.leftover = stream.remaining()};

        co_await more(stream);
    }
}

} // namespace nxt::http_coro
