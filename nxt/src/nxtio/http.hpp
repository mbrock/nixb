#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>

#include "nxtio/async.hpp"

namespace nxt::io::http {

struct protocol_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

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
    explicit slurper(std::span<const std::byte> needle)
        : needle_(needle)
    {
    }

    [[nodiscard]] std::span<const std::byte>
    operator()(std::span<const std::byte> bytes) const
    {
        auto match = std::ranges::search(bytes, needle_);
        auto size = static_cast<std::size_t>(
            std::distance(bytes.begin(), match.begin()));

        return bytes.first(size);
    }

    std::size_t kerf() const
    {
        return needle_.size_bytes();
    }


private:
    std::span<const std::byte> needle_;
};

inline slurper slurp(std::string_view needle)
{
    return slurper{std::as_bytes(std::span(needle))};
}

inline std::string_view as_text(std::span<const std::byte> bytes)
{
    return std::string_view{
        reinterpret_cast<const char *>(bytes.data()), bytes.size_bytes()};
}

inline char as_char(const std::byte byte)
{
    return static_cast<char>(byte);
}

class bytes_as_chars
{
public:
    std::string_view operator()(std::span<const std::byte> bytes) const
    {
        return as_text(bytes);
    }
};

template<typename Transport, byte_slicer Slicer>
nxt::task<std::span<const std::byte>> async_grab(
    Transport & transport, std::span<char> buffer, const Slicer & slicer)
{
    auto used = std::size_t{0};
    while (true) {
        auto bytes = std::as_bytes(buffer.first(used));
        if (slicer(bytes).size() + slicer.kerf() <= bytes.size()) {
            parse_buffer input{bytes};
            co_return input.grab(slicer);
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

} // namespace nxt::io::http
