#include <fmt/base.h>
#include <fmt/color.h>

#include <chrono>
#include <thread>
#include <variant>

#include "drv-graph.hpp"
#include "nix-api.hpp"
#include "nix-log-adapter.hpp"
#include "sim-gen.hpp"

using namespace std::chrono_literals;
using namespace nixb::nix_event;

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

int cmd_simulate(const std::string& installable, nxb::sim::Config config, float speed, bool show_time)
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
        fmt::print("  -v, --verbose   Show fake build output spam\n");
        return 1;
    }

    std::string installable = argv[1];
    nxb::sim::Config config;
    float speed = 10.0f;  // 10x speed by default (100ms virtual = 10ms real)
    bool show_time = false;

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
        }
    }

    return cmd_simulate(installable, config, speed, show_time);
}
