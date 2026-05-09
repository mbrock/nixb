#include <nxt/tui.hpp>
#include <nxtio/app.hpp>
#include <nxtio/async.hpp>
#include <nxtio/llm.hpp>
#include <nxtio/net.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct llm_hud_state
{
    nxt::io::llm::openai_responses_request request;
    std::string status = "ready";
    std::string last_event;
    std::string error;
    std::size_t event_count = 0;
    std::size_t output_bytes = 0;
    std::size_t output_chunks = 0;
    bool saw_output = false;
    bool output_ended_with_newline = true;
    bool done = false;
};

std::string fit_label(std::string text, std::size_t width)
{
    if (text.size() <= width)
        return text;
    if (width <= 3)
        return text.substr(0, width);
    return text.substr(0, width - 3) + "...";
}

std::string spinner_for(const llm_hud_state & state)
{
    if (state.done)
        return state.error.empty() ? "ok" : "!!";

    constexpr auto frames = std::array{"-", "\\", "|", "/"};
    return frames[state.event_count % frames.size()];
}

void update_hud(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    auto fn)
{
    fn(state);
    runtime.signal_damage();
}

auto build_hud(const llm_hud_state & state)
{
    using namespace nxt::tui;

    auto accent = fg(nxt::Rgba8{90, 190, 210}) | bold;
    auto muted = fg(nxt::Rgba8{150, 156, 162});
    auto normal = fg(nxt::Rgba8{220, 224, 228});
    auto good = fg(nxt::Rgba8{125, 200, 145}) | bold;
    auto bad = fg(nxt::Rgba8{235, 120, 120}) | bold;
    auto status_style = state.error.empty()
        ? (state.done ? good : normal)
        : bad;

    auto status = spinner_for(state) + " " + state.status;
    auto events = "events " + std::to_string(state.event_count);
    auto chunks = "chunks " + std::to_string(state.output_chunks);
    auto bytes = "bytes " + std::to_string(state.output_bytes);
    auto last = state.last_event.empty() ? "none" : state.last_event;
    auto detail = state.error.empty()
        ? std::string{"assistant text is streaming in scrollback"}
        : "error " + state.error;

    return column(
        row(
            text("nxtllm ", accent),
            text(status, status_style),
            text("  " + events, muted),
            text("  " + chunks, muted),
            text("  " + bytes, muted)),
        hrule(),
        row(
            text("model ", muted),
            text(fit_label(state.request.model, 24), normal),
            text("  last ", muted),
            text(fit_label(last, 44), muted)),
        row(
            text("input ", muted),
            text(fit_label(state.request.input, 72), normal)),
        row(text(fit_label(detail, 96), state.error.empty() ? muted : bad)));
}

nxt::task<> run_stream(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state)
{
    using namespace std::chrono_literals;

    auto exit_delay = 1500ms;
    try {
        runtime.println("user: " + state.request.input);
        runtime.println("assistant:");

        update_hud(runtime, state, [](llm_hud_state & hud) {
            hud.status = "connecting";
        });

        auto transport = co_await nxt::io::net::connect_tls(
            runtime.scheduler_handle(),
            nxt::io::net::endpoint{
                .host = "api.openai.com",
                .port = 443,
            });

        update_hud(runtime, state, [](llm_hud_state & hud) {
            hud.status = "streaming";
        });

        auto on_event =
            [&](nxt::io::llm::stream_event event) -> nxt::task<> {
            auto delta = event.type == "response.output_text.delta"
                ? event.payload.value("delta", std::string{})
                : std::string{};

            if (!delta.empty())
                runtime.print(delta);

            update_hud(runtime, state, [&](llm_hud_state & hud) {
                hud.last_event = event.type;
                ++hud.event_count;
                if (!delta.empty()) {
                    hud.saw_output = true;
                    hud.output_bytes += delta.size();
                    ++hud.output_chunks;
                    hud.output_ended_with_newline = delta.back() == '\n';
                }
                if (event.type == "response.completed") {
                    hud.status = "completed";
                    hud.done = true;
                } else if (
                    event.type == "response.failed"
                    || event.type == "response.incomplete") {
                    hud.status = event.type;
                    hud.done = true;
                }
            });

            co_return;
        };

        co_await nxt::io::llm::stream_openai_responses_over(
            transport,
            state.request,
            on_event);
        co_await transport.shutdown();

        if (state.saw_output && !state.output_ended_with_newline)
            runtime.print("\n");

        update_hud(runtime, state, [](llm_hud_state & hud) {
            if (!hud.done) {
                hud.status = "complete";
                hud.done = true;
            }
        });
    } catch (const std::exception & e) {
        runtime.println(std::string{"nxtllm error: "} + e.what());
        update_hud(runtime, state, [&](llm_hud_state & hud) {
            hud.status = "error";
            hud.error = e.what();
            hud.done = true;
        });
        exit_delay = 3000ms;
    }

    co_await runtime.sleep(exit_delay);
    runtime.request_shutdown();
}

} // namespace

int main(int argc, char ** argv)
{
    auto * api_key = std::getenv("OPENAI_API_KEY");
    if (api_key == nullptr || std::string_view{api_key}.empty()) {
        std::cerr << "nxtllm-hud: OPENAI_API_KEY is not set\n";
        return EXIT_FAILURE;
    }

    auto input = argc > 1 ? std::string{argv[1]}
                          : "explain lambda calculus fixpoints";
    auto model = argc > 2 ? std::string{argv[2]} : "gpt-5-mini";

    auto state = llm_hud_state{};
    state.request = nxt::io::llm::openai_responses_request{
        .api_key = api_key,
        .model = std::move(model),
        .input = std::move(input),
        .max_output_tokens = 3000,
        .reasoning_effort = "low",
        .store = false,
    };

    return nxt::ui::run(std::move(state), build_hud, run_stream);
}
