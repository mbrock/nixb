#include <fmt/base.h>
#include <fmt/color.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <variant>
#include <vector>

#include "drv-graph.hpp"
#include "nix-api.hpp"
#include "nix-log-adapter.hpp"
#include "nxtio/app.hpp"
#include "nxt/tui.hpp"
#include "sim-gen.hpp"

using namespace std::chrono_literals;
using namespace nixb::nix_event;

namespace {

std::string event_label(const NixLogEvent& ev)
{
    return std::visit([](auto&& e) -> std::string {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, ActivityStarted>) {
            return std::visit([](auto&& kind) -> std::string {
                using K = std::decay_t<decltype(kind)>;

                if constexpr (std::is_same_v<K, activity::Build>)
                    return fmt::format("build {}", kind.drv_path);
                else if constexpr (std::is_same_v<K, activity::Substitute>)
                    return fmt::format("substitute {}", kind.path);
                else if constexpr (std::is_same_v<K, activity::Download>)
                    return fmt::format("download {}", kind.url);
                else if constexpr (std::is_same_v<K, activity::Copy>)
                    return fmt::format("copy {}", kind.path);
                else
                    return "activity";
            }, e.kind);
        } else if constexpr (std::is_same_v<T, LogLine>) {
            return e.text;
        } else {
            return {};
        }
    }, ev);
}

bool is_build_start(const NixLogEvent& ev)
{
    if (auto* started = std::get_if<ActivityStarted>(&ev))
        return std::holds_alternative<activity::Build>(started->kind);
    return false;
}

bool is_substitute_start(const NixLogEvent& ev)
{
    if (auto* started = std::get_if<ActivityStarted>(&ev))
        return std::holds_alternative<activity::Substitute>(started->kind);
    return false;
}

struct TuiSimState
{
    struct ActiveItem
    {
        int64_t id = 0;
        std::string label;
        bool substitution = false;
        nxt::percent_t progress{0.0 * nxt::percent};
    };

    std::size_t total = 0;
    std::size_t completed = 0;
    std::size_t active_builds = 0;
    std::size_t active_subs = 0;
    std::size_t max_jobs = 1;
    std::size_t max_substitutions = 1;
    nxb::sim::SimTime virtual_time{0};
    std::vector<std::string> recent;
    std::vector<ActiveItem> active;
};

std::string fit_label(std::string text, std::size_t width)
{
    if (text.size() <= width)
        return text;
    if (width <= 1)
        return text.substr(0, width);
    return text.substr(0, width - 1) + "…";
}

nxt::percent_t percent_of(int64_t done, int64_t total)
{
    if (total <= 0)
        return 0.0 * nxt::percent;
    return (100.0 * static_cast<double>(done)
            / static_cast<double>(total)) * nxt::percent;
}

std::vector<TuiSimState::ActiveItem>::iterator
find_active(TuiSimState& state, int64_t id)
{
    return std::ranges::find_if(state.active, [id](const auto& item) {
        return item.id == id;
    });
}

nxt::task<> run_tui_simulation(
    nxt::ui::UIRuntime& runtime,
    nxb::drv::Graph& graph,
    nxb::sim::Config config,
    float speed,
    TuiSimState& state)
{
    using Clock = std::chrono::steady_clock;

    auto start = Clock::now();
    nxb::sim::SimTime last_sim_time{0};
    config.emit_progress = true;

    for (auto& timed : nxb::sim::generate_timed_events(graph, config)) {
        if (runtime.shutdown_requested())
            co_return;

        if (timed.time > last_sim_time) {
            auto target = start + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<float, std::milli>(
                    timed.time.count() / speed));
            auto now = Clock::now();
            if (target > now)
                co_await runtime.scheduler().yield_for(target - now);
            last_sim_time = timed.time;
        }

        state.virtual_time = timed.time;

        if (auto* started = std::get_if<ActivityStarted>(&timed.event)) {
            const auto is_sub = is_substitute_start(timed.event);
            state.active.push_back({
                started->id.value,
                event_label(timed.event),
                is_sub,
                0.0 * nxt::percent});
            if (is_build_start(timed.event))
                ++state.active_builds;
            else if (is_sub)
                ++state.active_subs;
        } else if (auto* finished =
                       std::get_if<ActivityFinished>(&timed.event)) {
            if (auto it = find_active(state, finished->id.value);
                it != state.active.end()) {
                if (it->substitution)
                    --state.active_subs;
                else
                    --state.active_builds;
                state.active.erase(it);
            }
            ++state.completed;
        } else if (auto* progress =
                       std::get_if<ActivityProgress>(&timed.event)) {
            if (auto it = find_active(state, progress->id.value);
                it != state.active.end())
                it->progress = percent_of(progress->done, progress->expected);
        }

        if (auto label = event_label(timed.event); !label.empty()) {
            runtime.println(label);
            state.recent.insert(state.recent.begin(), std::move(label));
            while (state.recent.size() > 3)
                state.recent.pop_back();
        }

        runtime.signal_damage();
    }

    runtime.println(fmt::format("Simulation complete: {}/{} derivations",
                                state.completed,
                                state.total));
    co_await runtime.sleep(700ms);
    runtime.request_shutdown();
}

int run_tui_simulation_app(
    nxb::drv::Graph& graph,
    nxb::sim::Config config,
    float speed)
{
    using namespace nxt::tui;

    TuiSimState initial{
        .total = graph.nodes.size(),
        .max_jobs = config.max_jobs,
        .max_substitutions = config.max_substitutions,
    };

    return nxt::ui::run(
        std::move(initial),
        [](const TuiSimState& state) {
            return column(
                list(state.active, [](const auto& item) {
                    const auto color = item.substitution
                                           ? nxt::Rgba8::cyan()
                                           : nxt::Rgba8::yellow();
                    return row(
                        text(fmt::format("{:<28}", fit_label(item.label, 28)),
                             fg(color)),
                        progress_bar(item.progress, color),
                        text(fmt::format(" {:>3.0f}%",
                                         item.progress
                                             .force_numerical_value_in(
                                                 nxt::percent)),
                             fg(color) | bold));
                }));
        },
        [&graph, config, speed](
            nxt::ui::UIRuntime& runtime,
            TuiSimState& state) -> nxt::task<> {
            co_await run_tui_simulation(
                runtime, graph, config, speed, state);
        });
}

} // namespace

void print_event(const NixLogEvent& ev, std::optional<nxb::sim::SimTime> time = std::nullopt)
{
    std::visit([&](auto&& e) {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, ActivityStarted>) {
            std::visit([&](auto&& kind) {
                using K = std::decay_t<decltype(kind)>;

                std::string time_str;
                if (time) {
                    time_str = fmt::format("[{:>6}ms] ", time->count());
                }

                if constexpr (std::is_same_v<K, activity::Build>) {
                    fmt::print(fmt::fg(fmt::color::yellow), "{}⚙ {}\n", time_str, kind.drv_path);
                } else if constexpr (std::is_same_v<K, activity::Substitute>) {
                    fmt::print(fmt::fg(fmt::color::cyan), "{}↓ {}\n", time_str, kind.path);
                } else if constexpr (std::is_same_v<K, activity::Download>) {
                    fmt::print(fmt::fg(fmt::color::blue), "{}⬇ {}\n", time_str, kind.url);
                } else if constexpr (std::is_same_v<K, activity::Copy>) {
                    fmt::print(fmt::fg(fmt::color::magenta), "{}⇄ {}\n", time_str, kind.path);
                }
            }, e.kind);
        } else if constexpr (std::is_same_v<T, ActivityFinished>) {
            // Skip finish events for cleaner output (started events are enough)
        } else if constexpr (std::is_same_v<T, ActivityProgress>) {
            fmt::print(fmt::fg(fmt::color::gray), "  [{}/{}]\n", e.done, e.expected);
        } else if constexpr (std::is_same_v<T, LogLine>) {
            fmt::print(fmt::fg(fmt::color::gray), "    {}\n", e.text);
        }
    }, ev);
}

int cmd_simulate(
    const std::string& installable,
    nxb::sim::Config config,
    float speed,
    bool show_time,
    bool tui)
{
    try {
        nxb::NixContext ctx;
        fmt::print("Resolving {}...\n", installable);

        auto roots = nxb::resolve_installable(ctx, installable);
        if (roots.empty()) {
            fmt::print(stderr, "No derivations found\n");
            return 1;
        }

        fmt::print("Building dependency graph...\n");
        auto graph = nxb::drv::build_graph(ctx, roots);
        fmt::print("Found {} derivations\n\n", graph.nodes.size());

        if (tui)
            return run_tui_simulation_app(graph, config, speed);

        std::size_t completed = 0;
        auto start = std::chrono::steady_clock::now();

        nxb::sim::play_realtime(graph, config, speed, [&](const nxb::sim::TimedEvent& timed) {
            if (show_time) {
                print_event(timed.event, timed.time);
            } else {
                print_event(timed.event);
            }

            if (std::holds_alternative<ActivityFinished>(timed.event)) {
                ++completed;
            }
        });

        auto elapsed = std::chrono::steady_clock::now() - start;
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        fmt::print("\n");
        fmt::print(fmt::fg(fmt::color::green) | fmt::emphasis::bold,
            "Simulation complete! {} derivations in {:.1f}s ({}x speed)\n",
            completed, elapsed_ms / 1000.0, speed);
        return 0;

    } catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return 1;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fmt::print("Usage: nxb <installable> [options]\n");
        fmt::print("       nxb nixpkgs#hello\n");
        fmt::print("       nxb .#default -j 8 -s 100 -v\n\n");
        fmt::print("Options:\n");
        fmt::print("  -j, --jobs N    Max concurrent builds (default: 4)\n");
        fmt::print("  -S, --subs N    Max concurrent substitutions (default: 16)\n");
        fmt::print("  -s, --speed N   Playback speed multiplier (default: 10)\n");
        fmt::print("  -t, --time      Show virtual timestamps\n");
        fmt::print("  -T, --tui       Show a live progress HUD\n");
        fmt::print("  -v, --verbose   Show fake build output spam\n");
        return 1;
    }

    std::string installable = argv[1];
    nxb::sim::Config config;
    float speed = 10.0f;  // 10x speed by default (100ms virtual = 10ms real)
    bool show_time = false;
    bool tui = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-j" || arg == "--jobs") && i + 1 < argc) {
            config.max_jobs = std::stoul(argv[++i]);
        } else if ((arg == "-s" || arg == "--speed") && i + 1 < argc) {
            speed = std::stof(argv[++i]);
        } else if ((arg == "-S" || arg == "--subs") && i + 1 < argc) {
            config.max_substitutions = std::stoul(argv[++i]);
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "-t" || arg == "--time") {
            show_time = true;
        } else if (arg == "-T" || arg == "--tui") {
            tui = true;
        }
    }

    return cmd_simulate(installable, config, speed, show_time, tui);
}
