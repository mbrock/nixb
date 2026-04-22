#pragma once

// Pure event generator for simulating Nix builds
//
// Generates a stream of NixLogEvent (same as nix-log-adapter) so that
// consumers (UI, replay, etc.) can use the same code path for real
// builds and simulations.
//
// Usage:
//   auto graph = nxb::drv::build_graph(ctx, roots);
//   for (auto& event : nxb::sim::generate_events(graph)) {
//       // same handling as real nix events
//   }

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <queue>
#include <random>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <nxt/async.hpp>
#include "drv-graph.hpp"
#include "nix-log-adapter.hpp"

namespace nxb::sim {

// ============================================================================
// Funny build log spam generator
// ============================================================================

class BuildSpam {
public:
    explicit BuildSpam(std::uint32_t seed = 0)
        : rng_(seed ? seed : std::random_device{}())
    {}

    std::string operator()(const std::string& name, bool is_sub) {
        if (is_sub) {
            return download_line(name);
        } else {
            return build_line(name);
        }
    }

private:
    std::mt19937 rng_;

    template<typename Container>
    const auto& pick(const Container& c) {
        std::uniform_int_distribution<std::size_t> dist(0, c.size() - 1);
        return c[dist(rng_)];
    }

    int randint(int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng_);
    }

    std::string build_line(const std::string& name) {
        static const std::vector<std::string> cmake_spam = {
            "-- The C compiler identification is GNU {}",
            "-- The CXX compiler identification is Clang {}",
            "-- Detecting C compiler ABI info - done",
            "-- Check for working C compiler: {} - skipped",
            "-- Detecting CXX compile features - done",
            "-- Found PkgConfig: /nix/store/{}",
            "-- Configuring done ({}s)",
            "-- Generating done ({}s)",
            "-- Build files have been written to: {}",
            "-- Looking for pthread.h",
            "-- Looking for pthread.h - found",
            "-- Performing Test CMAKE_HAVE_LIBC_PTHREAD",
            "-- Performing Test CMAKE_HAVE_LIBC_PTHREAD - Success",
            "-- Found Threads: TRUE",
            "-- Found ZLIB: /nix/store/{}/lib/libz.so (found version \"{}\")",
        };

        static const std::vector<std::string> make_spam = {
            "make[{}]: Entering directory '{}'",
            "make[{}]: Leaving directory '{}'",
            "  CC       {}.o",
            "  CXX      {}.o",
            "  CCLD     {}",
            "  AR       lib{}.a",
            "  LINK     {}",
            "  GEN      {}",
            "  INSTALL  {}",
            "  STRIP    {}",
        };

        static const std::vector<std::string> compiler_spam = {
            "gcc -c -O2 -fPIC -I/nix/store/{}/include -o {}.o {}.c",
            "g++ -std=c++20 -O2 -fPIC -I. -o {}.o {}.cpp",
            "clang++ -stdlib=libc++ -O3 -DNDEBUG -o {} {}.o",
            "ld -shared -o lib{}.so.{} {}.o",
            "ar rcs lib{}.a {}.o {}.o",
            "ranlib lib{}.a",
            "/nix/store/{}-binutils-wrapper-2.43.1/bin/ld: warning: {}",
        };

        static const std::vector<std::string> autoconf_spam = {
            "checking for {}... yes",
            "checking for {}... no",
            "checking for {} in -l{}... yes",
            "checking whether {} works... yes",
            "checking size of {}... {}",
            "checking for {} support... experimental",
            "configure: creating ./config.status",
            "config.status: creating Makefile",
            "config.status: creating config.h",
            "config.status: executing libtool commands",
        };

        static const std::vector<std::string> rust_spam = {
            "   Compiling {} v{} (/build/source)",
            "   Compiling {} v{}",
            "    Finished `release` profile [optimized] target(s) in {}s",
            "     Running `target/release/{}`",
            "    Building [===========>         ] {}/{}",
            "   Documenting {} v{}",
            "     Blocking waiting for file lock on package cache",
            "       Fresh {} v{}",
            "     warning: unused import: `{}`",
        };

        static const std::vector<std::string> python_spam = {
            "running build",
            "running build_ext",
            "building '{}' extension",
            "creating build/temp.linux-x86_64-cpython-312",
            "gcc -fno-strict-overflow -Wsign-compare -DNDEBUG -g -O3 -Wall {}",
            "Successfully built {}",
            "Installing collected packages: {}",
            "Requirement already satisfied: {} in /nix/store/{}",
            "Collecting {} (from {})",
        };

        static const std::vector<std::string> meson_spam = {
            "The Meson build system",
            "Version: {}",
            "Source dir: /build/source",
            "Build dir: /build/source/build",
            "Build type: native build",
            "Project name: {}",
            "Compiler for C supports link arguments -Wl,--as-needed: YES",
            "Dependency {} found: YES {}",
            "Build targets in project: {}",
            "ninja: Entering directory `/build/source/build'",
            "[{}/{}] Compiling C object {}",
            "[{}/{}] Linking target {}",
        };

        static const std::vector<std::string> existential_spam = {
            "warning: {} is deprecated, but aren't we all?",
            "note: consider using `{}` instead... or don't, I'm not your mother",
            "info: reticulating splines for {}",
            "info: aligning cosmic bits in {}",
            "debug: {} has achieved enlightenment",
            "trace: {} ponders the void",
            "hint: have you tried turning {} off and on again?",
            "note: {} compiles, therefore it is",
            "warning: {} contains mass quantities of undefined behavior",
            "info: downloading more RAM for {}",
            "debug: teaching {} about the birds and the bees",
            "trace: {} is now sentient, send help",
        };

        std::string base = name.substr(0, std::min(name.size(), std::size_t(12)));

        auto category = randint(0, 7);
        std::string tmpl;
        switch (category) {
            case 0: tmpl = pick(cmake_spam); break;
            case 1: tmpl = pick(make_spam); break;
            case 2: tmpl = pick(compiler_spam); break;
            case 3: tmpl = pick(autoconf_spam); break;
            case 4: tmpl = pick(rust_spam); break;
            case 5: tmpl = pick(python_spam); break;
            case 6: tmpl = pick(meson_spam); break;
            default: tmpl = pick(existential_spam); break;
        }

        return fmt::format(fmt::runtime(tmpl),
            base,
            fmt::format("{}.{}.{}", randint(1, 20), randint(0, 99), randint(0, 9)),
            randint(1, 4),
            randint(1, 999),
            randint(10, 500),
            base + std::to_string(randint(0, 99)),
            randint(1, 100));
    }

    std::string download_line(const std::string& name) {
        static const std::vector<std::string> templates = {
            "copying path '/nix/store/...-{}'",
            "  % Total    % Received  {}%",
            "downloading 'https://cache.nixos.org/nar/{}.nar.xz'",
            "  {} KiB / {} KiB  [{}%]",
            "fetching {} from binary cache...",
            "copying {} from 'https://cache.nixos.org'",
            "unpacking {} into /nix/store/...",
            "validating {} against NAR hash...",
            "decompressing {}.nar.xz ({} KiB)",
        };

        std::string base = name.substr(0, std::min(name.size(), std::size_t(16)));
        auto tmpl = pick(templates);

        return fmt::format(fmt::runtime(tmpl),
            base,
            randint(100, 50000),
            randint(0, 100),
            base + std::to_string(randint(0, 99)));
    }
};

using namespace std::chrono_literals;
using namespace nixb::nix_event;

using SimTime = std::chrono::milliseconds;
using NodeIndex = std::size_t;

// ============================================================================
// Configuration
// ============================================================================

struct Config {
    std::size_t max_jobs = 4;
    std::size_t max_substitutions = 16;

    SimTime min_build_time = 200ms;
    SimTime max_build_time = 3000ms;
    SimTime min_sub_time = 50ms;
    SimTime max_sub_time = 300ms;

    float substitution_rate = 0.75f;

    // Emit LogLine events with fake build output
    bool verbose = false;
    // How many lines per build (if verbose)
    std::size_t lines_per_build = 5;

    std::uint32_t seed = 0;
};

// ============================================================================
// Timed event with virtual timestamp
// ============================================================================

struct TimedEvent {
    SimTime time;
    NixLogEvent event;
};

// ============================================================================
// Duration estimator based on derivation characteristics
// ============================================================================

inline SimTime estimate_build_duration(
    const drv::Node& node,
    std::mt19937& rng,
    const Config& config)
{
    std::uniform_int_distribution<int> base_dist(
        config.min_build_time.count(), config.max_build_time.count());

    auto base = SimTime{base_dist(rng)};

    // Scale by input count (more deps = more complex build)
    float input_factor = 1.0f + 0.2f * std::log2(1.0f + node.inputs.size());

    // Scale by subtree size (larger dep tree = bigger package)
    float subtree_factor = 1.0f + 0.1f * std::log2(1.0f + node.subtree_size);

    // Scale by height (deeper in tree = more foundational, often bigger)
    float height_factor = 1.0f + 0.05f * node.height;

    // Heuristic: certain names suggest longer builds
    float name_factor = 1.0f;
    auto name = node.name;
    if (name.find("gcc") != std::string::npos ||
        name.find("llvm") != std::string::npos ||
        name.find("clang") != std::string::npos) {
        name_factor = 3.0f;  // compilers are slow
    } else if (name.find("glibc") != std::string::npos ||
               name.find("linux") != std::string::npos) {
        name_factor = 2.5f;  // kernel/libc are big
    } else if (name.find("boost") != std::string::npos ||
               name.find("qt") != std::string::npos ||
               name.find("chromium") != std::string::npos) {
        name_factor = 4.0f;  // notoriously slow
    } else if (name.find(".tar") != std::string::npos ||
               name.find("-source") != std::string::npos) {
        name_factor = 0.3f;  // just unpacking
    }

    auto total = static_cast<int>(base.count() * input_factor * subtree_factor * height_factor * name_factor);

    // Clamp to reasonable range
    return SimTime{std::clamp(total, 50, 30000)};
}

inline SimTime estimate_sub_duration(
    const drv::Node& node,
    std::mt19937& rng,
    const Config& config)
{
    std::uniform_int_distribution<int> base_dist(
        config.min_sub_time.count(), config.max_sub_time.count());

    auto base = SimTime{base_dist(rng)};

    // Bigger subtrees = bigger downloads
    float size_factor = 1.0f + 0.15f * std::log2(1.0f + node.subtree_size);

    return SimTime{static_cast<int>(base.count() * size_factor)};
}

// ============================================================================
// The Generator
// ============================================================================

inline nxb::generator<NixLogEvent> generate_events(drv::Graph& graph, Config config = {})
{
    if (graph.nodes.empty())
        co_return;

    std::mt19937 rng(config.seed ? config.seed : std::random_device{}());

    // Pre-decide timings and substitution status for each node
    std::vector<bool> will_substitute(graph.nodes.size());
    std::vector<SimTime> durations(graph.nodes.size());

    std::uniform_real_distribution<float> sub_dist(0.0f, 1.0f);

    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        auto& node = *graph.nodes[i];
        will_substitute[i] = sub_dist(rng) < config.substitution_rate;

        if (will_substitute[i]) {
            durations[i] = estimate_sub_duration(node, rng, config);
        } else {
            durations[i] = estimate_build_duration(node, rng, config);
        }
    }

    // Simulation state
    enum class State { waiting, ready, running, done };
    std::vector<State> node_state(graph.nodes.size(), State::waiting);
    std::vector<std::size_t> pending_deps(graph.nodes.size());

    // Activity IDs: use node index + 1 (0 is reserved for root)
    auto activity_id = [](NodeIndex idx) { return ActivityId{static_cast<int64_t>(idx + 1)}; };
    const auto root_activity = ActivityId{0};

    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        pending_deps[i] = graph.nodes[i]->inputs.size();
    }

    // Priority queue: (completion_time, node_index)
    using TimeEvent = std::pair<SimTime, NodeIndex>;
    auto cmp = [](const TimeEvent& a, const TimeEvent& b) {
        return a.first > b.first;
    };
    std::priority_queue<TimeEvent, std::vector<TimeEvent>, decltype(cmp)> timeline(cmp);

    std::size_t active_builds = 0;
    std::size_t active_subs = 0;
    SimTime current_time{0};

    std::queue<NodeIndex> ready_queue;

    // Seed ready queue with leaves
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        if (pending_deps[i] == 0) {
            node_state[i] = State::ready;
            ready_queue.push(i);
        }
    }

    // Helper to start work and emit events
    auto start_work = [&]() {
        std::vector<NodeIndex> started;

        while (!ready_queue.empty()) {
            auto idx = ready_queue.front();
            bool is_sub = will_substitute[idx];

            if (is_sub) {
                if (active_subs >= config.max_substitutions)
                    break;
                ++active_subs;
            } else {
                if (active_builds >= config.max_jobs)
                    break;
                ++active_builds;
            }

            ready_queue.pop();
            node_state[idx] = State::running;
            timeline.push({current_time + durations[idx], idx});
            started.push_back(idx);
        }

        return started;
    };

    // Build spam generator
    BuildSpam spam(config.seed);

    // Start initial work
    for (auto idx : start_work()) {
        auto* node = graph.nodes[idx].get();
        bool is_sub = will_substitute[idx];

        activity::Kind kind = is_sub
            ? activity::Kind{activity::Substitute{node->name, "https://cache.nixos.org"}}
            : activity::Kind{activity::Build{node->name, "out"}};

        co_yield ActivityStarted{activity_id(idx), root_activity, std::move(kind)};
    }

    // Main simulation loop
    while (!timeline.empty()) {
        auto [finish_time, idx] = timeline.top();
        timeline.pop();

        current_time = finish_time;

        auto* node = graph.nodes[idx].get();
        bool is_sub = will_substitute[idx];
        node_state[idx] = State::done;

        if (is_sub) {
            --active_subs;
        } else {
            --active_builds;
        }

        // Emit build spam before finishing
        if (config.verbose) {
            for (std::size_t i = 0; i < config.lines_per_build; ++i) {
                co_yield LogLine{nix::lvlInfo, spam(node->name, is_sub)};
            }
        }

        co_yield ActivityFinished{activity_id(idx)};

        // Wake up dependents
        for (auto dep_idx : graph.nodes[idx]->outputs) {
            if (--pending_deps[dep_idx] == 0 && node_state[dep_idx] == State::waiting) {
                node_state[dep_idx] = State::ready;
                ready_queue.push(dep_idx);
            }
        }

        // Start more work
        for (auto started_idx : start_work()) {
            auto* started_node = graph.nodes[started_idx].get();
            bool started_is_sub = will_substitute[started_idx];

            activity::Kind kind = started_is_sub
                ? activity::Kind{activity::Substitute{started_node->name, "https://cache.nixos.org"}}
                : activity::Kind{activity::Build{started_node->name, "out"}};

            co_yield ActivityStarted{activity_id(started_idx), root_activity, std::move(kind)};
        }
    }
}

// ============================================================================
// Timed generator - yields events with virtual timestamps
// ============================================================================

inline nxb::generator<TimedEvent> generate_timed_events(drv::Graph& graph, Config config = {})
{
    if (graph.nodes.empty())
        co_return;

    std::mt19937 rng(config.seed ? config.seed : std::random_device{}());

    std::vector<bool> will_substitute(graph.nodes.size());
    std::vector<SimTime> durations(graph.nodes.size());

    std::uniform_real_distribution<float> sub_dist(0.0f, 1.0f);

    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        auto& node = *graph.nodes[i];
        will_substitute[i] = sub_dist(rng) < config.substitution_rate;
        durations[i] = will_substitute[i]
            ? estimate_sub_duration(node, rng, config)
            : estimate_build_duration(node, rng, config);
    }

    enum class State { waiting, ready, running, done };
    std::vector<State> node_state(graph.nodes.size(), State::waiting);
    std::vector<std::size_t> pending_deps(graph.nodes.size());

    auto activity_id = [](NodeIndex idx) { return ActivityId{static_cast<int64_t>(idx + 1)}; };
    const auto root_activity = ActivityId{0};

    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        pending_deps[i] = graph.nodes[i]->inputs.size();
    }

    using TimeNodePair = std::pair<SimTime, NodeIndex>;
    auto cmp = [](const TimeNodePair& a, const TimeNodePair& b) { return a.first > b.first; };
    std::priority_queue<TimeNodePair, std::vector<TimeNodePair>, decltype(cmp)> timeline(cmp);

    std::size_t active_builds = 0;
    std::size_t active_subs = 0;
    SimTime current_time{0};

    std::queue<NodeIndex> ready_queue;

    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        if (pending_deps[i] == 0) {
            node_state[i] = State::ready;
            ready_queue.push(i);
        }
    }

    BuildSpam spam(config.seed);

    auto start_work = [&]() {
        std::vector<NodeIndex> started;
        while (!ready_queue.empty()) {
            auto idx = ready_queue.front();
            bool is_sub = will_substitute[idx];

            if (is_sub) {
                if (active_subs >= config.max_substitutions) break;
                ++active_subs;
            } else {
                if (active_builds >= config.max_jobs) break;
                ++active_builds;
            }

            ready_queue.pop();
            node_state[idx] = State::running;
            timeline.push({current_time + durations[idx], idx});
            started.push_back(idx);
        }
        return started;
    };

    // Start initial work
    for (auto idx : start_work()) {
        auto* node = graph.nodes[idx].get();
        bool is_sub = will_substitute[idx];

        activity::Kind kind = is_sub
            ? activity::Kind{activity::Substitute{node->name, "https://cache.nixos.org"}}
            : activity::Kind{activity::Build{node->name, "out"}};

        co_yield TimedEvent{current_time, ActivityStarted{activity_id(idx), root_activity, std::move(kind)}};
    }

    while (!timeline.empty()) {
        auto [finish_time, idx] = timeline.top();
        timeline.pop();

        current_time = finish_time;

        auto* node = graph.nodes[idx].get();
        bool is_sub = will_substitute[idx];
        node_state[idx] = State::done;

        if (is_sub) --active_subs;
        else --active_builds;

        if (config.verbose) {
            for (std::size_t i = 0; i < config.lines_per_build; ++i) {
                co_yield TimedEvent{current_time, LogLine{nix::lvlInfo, spam(node->name, is_sub)}};
            }
        }

        co_yield TimedEvent{current_time, ActivityFinished{activity_id(idx)}};

        for (auto dep_idx : node->outputs) {
            if (--pending_deps[dep_idx] == 0 && node_state[dep_idx] == State::waiting) {
                node_state[dep_idx] = State::ready;
                ready_queue.push(dep_idx);
            }
        }

        for (auto started_idx : start_work()) {
            auto* started_node = graph.nodes[started_idx].get();
            bool started_is_sub = will_substitute[started_idx];

            activity::Kind kind = started_is_sub
                ? activity::Kind{activity::Substitute{started_node->name, "https://cache.nixos.org"}}
                : activity::Kind{activity::Build{started_node->name, "out"}};

            co_yield TimedEvent{current_time, ActivityStarted{activity_id(started_idx), root_activity, std::move(kind)}};
        }
    }
}

// ============================================================================
// Real-time playback helper
// ============================================================================

template<typename Callback>
void play_realtime(drv::Graph& graph, Config config, float speed, Callback&& on_event)
{
    using Clock = std::chrono::steady_clock;

    auto start = Clock::now();
    SimTime last_sim_time{0};

    for (auto& timed : generate_timed_events(graph, config)) {
        if (timed.time > last_sim_time) {
            auto delta = timed.time - last_sim_time;
            auto real_delta = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<float, std::milli>(delta.count() / speed));

            auto target = start + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<float, std::milli>(timed.time.count() / speed));

            std::this_thread::sleep_until(target);
            last_sim_time = timed.time;
        }

        on_event(timed);
    }
}

} // namespace nxb::sim
