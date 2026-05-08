#define LIBCORO_FEATURE_NETWORKING
#define LIBCORO_FEATURE_TLS
#include <coro/net/dns/resolver.hpp>
#include <coro/net/tcp/client.hpp>
#include <coro/net/tls/client.hpp>
#undef LIBCORO_FEATURE_NETWORKING
#undef LIBCORO_FEATURE_TLS

#include <nxtio/async.hpp>
#include <nxtio/http.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class tcp_transport
{
public:
    explicit tcp_transport(coro::net::tcp::client client)
        : client_(std::move(client))
    {
    }

    nxt::task<std::size_t> read_some(std::span<char> dst)
    {
        if (dst.empty())
            co_return 0;

        auto [status, bytes] = co_await client_.read_some(dst);
        if (status.is_ok())
            co_return bytes.size();
        if (status.is_closed())
            co_return 0;

        throw std::runtime_error{"read: " + status.message()};
    }

    nxt::task<> write_all(std::string_view bytes)
    {
        auto buffer = std::span<const char>{bytes.data(), bytes.size()};
        auto [status, rest] = co_await client_.write_all(buffer);
        if (!status.is_ok()) {
            (void) rest;
            throw std::runtime_error{"write: " + status.message()};
        }
    }

private:
    coro::net::tcp::client client_;
};

class tls_transport
{
public:
    tls_transport(
        std::shared_ptr<coro::net::tls::context> ctx,
        coro::net::tls::client client)
        : ctx_(std::move(ctx))
        , client_(std::move(client))
    {
    }

    nxt::task<std::size_t> read_some(std::span<char> dst)
    {
        if (dst.empty())
            co_return 0;

        auto [status, bytes] = co_await client_.recv(dst);
        if (status == coro::net::tls::recv_status::ok)
            co_return bytes.size();
        if (status == coro::net::tls::recv_status::closed)
            co_return 0;

        throw std::runtime_error{
            "TLS read: " + coro::net::tls::to_string(status)};
    }

    nxt::task<> write_all(std::string_view bytes)
    {
        auto remaining = std::span<const char>{bytes.data(), bytes.size()};
        while (!remaining.empty()) {
            auto [status, rest] = co_await client_.send(remaining);
            if (status == coro::net::tls::send_status::ok) {
                if (rest.size() == remaining.size())
                    throw std::runtime_error{"TLS write made no progress"};
                remaining = rest;
                continue;
            }

            throw std::runtime_error{
                "TLS write: " + coro::net::tls::to_string(status)};
        }
    }

    nxt::task<> shutdown()
    {
        co_await client_.shutdown(std::chrono::seconds{5});
    }

private:
    std::shared_ptr<coro::net::tls::context> ctx_;
    coro::net::tls::client client_;
};

struct resolved_target
{
    std::vector<coro::net::ip_address> addresses;
    std::uint16_t port = 0;
};

nxt::task<resolved_target> resolve_target(
    std::unique_ptr<nxt::io_scheduler> & sched,
    std::string_view host,
    std::string_view port)
{
    auto port_number = nxt::io::http::parse_port(port);
    auto resolver = coro::net::dns::resolver<nxt::io_scheduler>{
        sched,
        std::chrono::milliseconds{5000},
    };
    auto dns = co_await resolver.host_by_name(
        coro::net::hostname{std::string(host)});

    if (dns->status() != coro::net::dns::status::complete
        || dns->ip_addresses().empty()) {
        throw std::runtime_error{"DNS lookup failed"};
    }

    co_return resolved_target{
        .addresses = dns->ip_addresses(),
        .port = port_number,
    };
}

nxt::task<tcp_transport> connect_tcp(
    std::unique_ptr<nxt::io_scheduler> & sched,
    std::string_view host,
    std::string_view port)
{
    auto target = co_await resolve_target(sched, host, port);
    auto last_status = coro::net::connect_status::error;

    for (auto const & address : target.addresses) {
        auto endpoint = coro::net::socket_address{address, target.port};
        auto client = coro::net::tcp::client{
            sched,
            endpoint,
        };
        last_status = co_await client.connect();
        if (last_status == coro::net::connect_status::connected)
            co_return tcp_transport{std::move(client)};
    }

    throw std::runtime_error{
        "connect: " + coro::net::to_string(last_status)};
}

nxt::task<tls_transport> connect_tls(
    std::unique_ptr<nxt::io_scheduler> & sched,
    std::string_view host,
    std::string_view port)
{
    auto target = co_await resolve_target(sched, host, port);
    auto last_status = coro::net::tls::connection_status::error;
    auto ctx = std::make_shared<coro::net::tls::context>(
        coro::net::tls::verify_peer_t::yes);

    for (auto const & address : target.addresses) {
        auto endpoint = coro::net::socket_address{address, target.port};
        auto client = coro::net::tls::client{
            sched,
            ctx,
            endpoint,
            std::string{host},
        };
        last_status = co_await client.connect();
        if (last_status == coro::net::tls::connection_status::connected)
            co_return tls_transport{ctx, std::move(client)};
    }

    throw std::runtime_error{
        "TLS connect: " + coro::net::tls::to_string(last_status)};
}

template<typename Transport>
nxt::task<>
fetch_over(Transport & transport, nxt::io::http::url const & url)
{
    auto host_header = nxt::io::http::is_default_port(url)
                           ? url.host
                           : url.host + ":" + url.port;
    auto request = std::string{
        "GET " + url.target + " HTTP/1.1\r\n"
        "Host: " + host_header + "\r\n"
        "User-Agent: nxthttp/0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n"};

    co_await transport.write_all(request);

    auto head_buffer = std::array<char, 16 * 1024>{};
    auto head = co_await nxt::io::http::async_grab(
        transport, head_buffer, nxt::io::http::slurp("\r\n\r\n"));
    auto response = nxt::io::http::parse_response_head(head.bytes);

    std::cerr << response.status_line << '\n';

    auto body_buffer = std::array<char, 16 * 1024>{};
    auto line_buffer = std::array<char, 1024>{};
    auto on_chunk = [](std::span<const std::byte> chunk) -> nxt::task<> {
        auto text = nxt::io::http::as_text(chunk);
        std::cout.write(
            text.data(), static_cast<std::streamsize>(text.size()));
        std::cout.flush();
        co_return;
    };

    if (response.chunked) {
        co_await nxt::io::http::read_chunked(
            transport, line_buffer, body_buffer, head.leftover, on_chunk);
    } else if (response.content_length) {
        co_await nxt::io::http::read_content_length(
            transport,
            body_buffer,
            head.leftover,
            *response.content_length,
            on_chunk);
    } else {
        co_await nxt::io::http::read_until_eof(
            transport, body_buffer, head.leftover, on_chunk);
    }
}

nxt::task<>
fetch(std::unique_ptr<nxt::io_scheduler> & sched, nxt::io::http::url url)
{
    if (url.tls) {
        auto transport = co_await connect_tls(sched, url.host, url.port);
        co_await fetch_over(transport, url);
        co_await transport.shutdown();
        co_return;
    }

    auto transport = co_await connect_tcp(sched, url.host, url.port);
    co_await fetch_over(transport, url);
}

} // namespace

int main(int argc, char ** argv)
{
    if (argc != 2) {
        std::cerr << "usage: nxthttp http[s]://host[:port]/path\n";
        return EXIT_FAILURE;
    }

    try {
        auto url = nxt::io::http::parse_url(argv[1]);
        auto sched = nxt::io_scheduler::make_unique(
            nxt::io_scheduler::options{
                .execution_strategy = nxt::io_scheduler::
                    execution_strategy_t::process_tasks_inline,
            });
        nxt::sync_wait(sched->schedule(fetch(sched, std::move(url))));
    } catch (const std::exception & e) {
        std::cerr << "nxthttp: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
