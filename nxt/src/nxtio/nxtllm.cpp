#include <nxtio/async.hpp>
#include <nxtio/llm.hpp>
#include <nxtio/net.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

nxt::task<> run(
    std::unique_ptr<nxt::io_scheduler> & sched,
    nxt::io::llm::openai_responses_request request)
{
    auto transport = co_await nxt::io::net::connect_tls(
        sched,
        nxt::io::net::endpoint{
            .host = "api.openai.com",
            .port = 443,
        });
    auto on_event =
        [](nxt::io::llm::stream_event event) -> nxt::task<> {
        std::cout << "event: " << event.type << '\n';
        if (event.type == "response.output_text.delta") {
            auto delta = event.payload.value("delta", std::string{});
            if (!delta.empty())
                std::cout << "text: " << delta << '\n';
        }
        std::cout << "data: " << event.payload.dump() << "\n\n";
        co_return;
    };

    co_await nxt::io::llm::stream_openai_responses_over(
        transport, request, on_event);
    co_await transport.shutdown();
}

} // namespace

int main(int argc, char ** argv)
{
    auto * api_key = std::getenv("OPENAI_API_KEY");
    if (api_key == nullptr || std::string_view{api_key}.empty()) {
        std::cerr << "nxtllm: OPENAI_API_KEY is not set\n";
        return EXIT_FAILURE;
    }

    auto input = argc > 1 ? std::string{argv[1]} : "Say ok in one word.";
    auto model = argc > 2 ? std::string{argv[2]} : "gpt-5-mini";

    try {
        auto sched = nxt::io_scheduler::make_unique(
            nxt::io_scheduler::options{
                .execution_strategy = nxt::io_scheduler::
                    execution_strategy_t::process_tasks_inline,
            });
        auto request = nxt::io::llm::openai_responses_request{
            .api_key = api_key,
            .model = std::move(model),
            .input = std::move(input),
            .max_output_tokens = 128,
            .reasoning_effort = "minimal",
            .store = false,
        };
        nxt::sync_wait(sched->schedule(run(sched, std::move(request))));
    } catch (const std::exception & e) {
        std::cerr << "nxtllm: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
