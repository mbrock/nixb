#include <nxtio/async.hpp>
#include <nxtio/http.hpp>
#include <nxtio/net.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace {

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
        auto transport = co_await nxt::io::net::connect_tls(
            sched,
            nxt::io::net::endpoint{
                .host = url.host,
                .port = nxt::io::http::parse_port(url.port),
            });
        co_await fetch_over(transport, url);
        co_await transport.shutdown();
        co_return;
    }

    auto transport = co_await nxt::io::net::connect_tcp(
        sched,
        nxt::io::net::endpoint{
            .host = url.host,
            .port = nxt::io::http::parse_port(url.port),
        });
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
