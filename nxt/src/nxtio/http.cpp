#include <nxtio/http.hpp>

#include <algorithm>
#include <charconv>
#include <iterator>
#include <ranges>

namespace nxt::io::http {

namespace {

char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c - 'A' + 'a');
    return c;
}

bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
           && std::ranges::equal(a, b, {}, ascii_lower, ascii_lower);
}

bool icontains(std::string_view haystack, std::string_view needle)
{
    return std::ranges::search(
               haystack, needle, {}, ascii_lower, ascii_lower)
               .begin()
           != haystack.end();
}

std::string_view trim_ascii(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

} // namespace

url parse_url(std::string_view text)
{
    auto tls = false;
    if (text.starts_with("http://")) {
        text.remove_prefix(std::string_view{"http://"}.size());
    } else if (text.starts_with("https://")) {
        text.remove_prefix(std::string_view{"https://"}.size());
        tls = true;
    } else {
        throw protocol_error{
            "only http:// and https:// URLs are supported"};
    }

    auto slash = text.find('/');
    auto authority = text.substr(0, slash);
    auto target = slash == std::string_view::npos ? std::string_view{"/"}
                                                  : text.substr(slash);
    if (authority.empty())
        throw protocol_error{"URL host is empty"};

    url parsed;
    parsed.tls = tls;
    parsed.port = tls ? "443" : "80";
    parsed.target = target;

    auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
        if (parsed.host.empty() || parsed.port.empty())
            throw protocol_error{"invalid URL authority"};
    } else {
        parsed.host = authority;
    }

    return parsed;
}

std::uint16_t parse_port(std::string_view text)
{
    auto port = unsigned{0};
    auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), port, 10);
    if (ec != std::errc{} || ptr != text.data() + text.size()
        || port > 65535)
        throw protocol_error{"invalid URL port"};
    return static_cast<std::uint16_t>(port);
}

bool is_default_port(const url & url)
{
    return (!url.tls && url.port == "80") || (url.tls && url.port == "443");
}

response_head parse_response_head(std::span<const std::byte> bytes)
{
    auto text = as_text(bytes);
    auto head = response_head{};
    auto first = true;

    while (!text.empty()) {
        auto eol = text.find("\r\n");
        auto line =
            eol == std::string_view::npos ? text : text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{}
                                             : text.substr(eol + 2);

        if (first) {
            head.status_line = line;
            first = false;
            continue;
        }

        auto colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;

        auto name = trim_ascii(line.substr(0, colon));
        auto value = trim_ascii(line.substr(colon + 1));

        if (iequals(name, "content-length")) {
            auto parsed = std::size_t{0};
            auto [ptr, ec] = std::from_chars(
                value.data(), value.data() + value.size(), parsed, 10);
            if (ec != std::errc{} || ptr != value.data() + value.size())
                throw protocol_error{"invalid Content-Length"};
            head.content_length = parsed;
        } else if (
            iequals(name, "transfer-encoding")
            && icontains(value, "chunked")) {
            head.chunked = true;
        }
    }

    if (head.status_line.empty())
        throw protocol_error{"missing response status line"};

    return head;
}

std::string_view as_text(std::span<const std::byte> bytes)
{
    return std::string_view{
        reinterpret_cast<const char *>(bytes.data()), bytes.size_bytes()};
}

char as_char(std::byte byte)
{
    return static_cast<char>(byte);
}

slurper::slurper(std::span<const std::byte> needle)
    : needle_(needle)
{
}

std::span<const std::byte>
slurper::operator()(std::span<const std::byte> bytes) const
{
    auto match = std::ranges::search(bytes, needle_);
    auto size = static_cast<std::size_t>(
        std::distance(bytes.begin(), match.begin()));

    return bytes.first(size);
}

std::size_t slurper::kerf() const
{
    return needle_.size_bytes();
}

slurper slurp(std::string_view needle)
{
    return slurper{std::as_bytes(std::span(needle))};
}

std::size_t parse_chunk_size(std::span<const std::byte> line)
{
    auto text = as_text(line);
    auto end = text.find(';');
    auto size_text = text.substr(0, end);
    if (size_text.empty())
        throw protocol_error{"empty chunk size"};

    auto size = std::size_t{0};
    auto * first = size_text.data();
    auto * last = first + size_text.size();
    auto [ptr, ec] = std::from_chars(first, last, size, 16);
    if (ec != std::errc{} || ptr != last)
        throw protocol_error{"invalid chunk size"};

    return size;
}

} // namespace nxt::io::http
