#pragma once

// Build simulator using coroutines - mirrors Nix's goal-based architecture
//
// Each derivation becomes a Goal coroutine that:
// 1. co_awaits its input dependencies
// 2. co_awaits a build/substitution slot
// 3. Simulates the work
// 4. Completes, waking up dependent goals
//
// Usage:
//   auto graph = nxb::drv::build_graph(ctx, roots);
//   nxb::queue<nxb::sim::Event> events;
//   nxb::sim::Simulator sim(runtime, graph, events);
//   co_await runtime.run(
//       sim.build(root_indices),
//       consume_events(events)
//   );
//   co_await events.shutdown();

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <nxt/async.hpp>
#include <nxt/app.hpp>

#include "drv-graph.hpp"

namespace nxb::sim {

using namespace std::chrono_literals;
using SimTime = std::chrono::milliseconds;
using NodeIndex = std::size_t;

// ============================================================================
// Events emitted by the simulation (for UI)
// ============================================================================

struct GoalStarted
{
    NodeIndex node;
    std::string name;
    std::size_t num_deps;
};

struct WaitingForDeps
{
    NodeIndex node;
    std::size_t remaining;
};

struct DepsComplete
{
    NodeIndex node;
};

struct SlotAcquired
{
    NodeIndex node;
    bool is_substitution;
};

struct WorkStarted
{
    NodeIndex node;
    SimTime duration;
    bool is_substitution;
};

struct WorkComplete
{
    NodeIndex node;
    bool is_substitution;
};

struct GoalComplete
{
    NodeIndex node;
    bool success;
};

// Fake build output line (for verbose mode)
struct BuildOutput
{
    NodeIndex node;
    std::string line;
};

using Event = std::variant<
    GoalStarted,
    WaitingForDeps,
    DepsComplete,
    SlotAcquired,
    WorkStarted,
    WorkComplete,
    GoalComplete,
    BuildOutput>;

// ============================================================================
// Configuration
// ============================================================================

struct Config
{
    std::size_t max_jobs = 4;
    std::size_t max_substitutions = 16;

    SimTime min_build_time = 200ms;
    SimTime max_build_time = 3000ms;
    SimTime min_substitution_time = 50ms;
    SimTime max_substitution_time = 300ms;

    float substitution_rate = 0.75f;

    // Time acceleration: 1ms sim time = this much real time
    float time_scale = 0.1f;

    // Emit fake build output lines
    bool verbose = false;

    std::uint32_t seed = 0;
};

// ============================================================================
// Semaphore for limiting concurrent jobs (like Nix's build slots)
// ============================================================================

class Semaphore
{
public:
    explicit Semaphore(std::size_t count)
        : count_(count)
    {
    }

    nxb::task<> acquire()
    {
        while (count_ == 0) {
            co_await event_;
            event_.reset();
        }
        --count_;
    }

    void release()
    {
        ++count_;
        event_.set();
    }

    std::size_t available() const { return count_; }

    void reset(std::size_t count)
    {
        count_ = count;
        event_.reset();
    }

private:
    std::size_t count_;
    nxb::event event_;
};

// ============================================================================
// The Simulator - manages coroutine goals
// ============================================================================

class Simulator
{
public:
    Config config;

    Simulator(nxb::ui::UIRuntime& runtime, drv::Graph& graph, nxb::queue<Event>& events)
        : runtime_(runtime)
        , graph_(graph)
        , events_(events)
        , rng_(config.seed ? config.seed : std::random_device{}())
        , build_slots_(config.max_jobs)
        , subst_slots_(config.max_substitutions)
    {
    }

    // Build the given root nodes - returns when all complete
    nxb::task<> build(const std::vector<NodeIndex>& /* roots */)
    {
        // Reset state
        build_slots_.reset(config.max_jobs);
        subst_slots_.reset(config.max_substitutions);
        goal_complete_.clear();
        completed_count_ = 0;

        // Pre-decide which nodes will substitute
        will_substitute_.resize(graph_.nodes.size());
        durations_.resize(graph_.nodes.size());

        std::uniform_real_distribution<float> sub_dist(0.0f, 1.0f);
        for (std::size_t i = 0; i < graph_.nodes.size(); ++i) {
            will_substitute_[i] = sub_dist(rng_) < config.substitution_rate;
            durations_[i] = will_substitute_[i]
                ? random_duration(config.min_substitution_time, config.max_substitution_time)
                : random_duration(config.min_build_time, config.max_build_time);

            // Scale duration by complexity
            auto* node = graph_.get(i);
            if (!will_substitute_[i]) {
                float complexity = 1.0f + 0.3f * std::log2(1.0f + node->inputs.size());
                durations_[i] = SimTime(static_cast<int>(durations_[i].count() * complexity));
            }
        }

        // Create completion events for all nodes
        for (std::size_t i = 0; i < graph_.nodes.size(); ++i) {
            goal_complete_.emplace_back(std::make_unique<nxb::event>());
        }

        // Build a vector of all goal tasks
        std::vector<nxb::task<>> goal_tasks;
        goal_tasks.reserve(graph_.nodes.size());
        for (std::size_t i = 0; i < graph_.nodes.size(); ++i) {
            goal_tasks.push_back(run_goal(i));
        }

        // Run all goals concurrently using when_all
        // This actually drives the coroutines
        co_await nxb::when_all(std::move(goal_tasks));
    }

    // Stats
    std::size_t completed_count() const { return completed_count_; }
    std::size_t total_count() const { return graph_.nodes.size(); }
    std::size_t active_builds() const { return config.max_jobs - build_slots_.available(); }
    std::size_t active_subs() const { return config.max_substitutions - subst_slots_.available(); }

private:
    void emit(Event ev)
    {
        nxb::sync_wait(events_.push(std::move(ev)));
    }

    SimTime random_duration(SimTime min, SimTime max)
    {
        std::uniform_int_distribution<int> dist(min.count(), max.count());
        return SimTime{dist(rng_)};
    }

    // Generate fake build output line
    std::string random_build_line(const std::string& name)
    {
        static const std::vector<std::string> prefixes = {
            "compiling", "linking", "checking", "configuring",
            "installing", "generating", "building", "patching"
        };
        static const std::vector<std::string> suffixes = {
            ".c", ".cpp", ".h", ".o", ".so", ".a", ".py", ".sh"
        };
        static const std::vector<std::string> actions = {
            "gcc -c -O2 -fPIC",
            "g++ -std=c++20 -O2",
            "ar rcs lib{}.a",
            "ld -shared -o lib{}.so",
            "install -m 755",
            "checking for {}... yes",
            "checking for {}... no",
            "make[2]: Entering directory",
            "make[2]: Leaving directory",
            "CC       src/{}.o",
            "CCLD     {}",
            "GEN      {}",
            "INSTALL  {}",
            "patching file {}",
            "applying patch {}",
        };

        std::uniform_int_distribution<std::size_t> action_dist(0, actions.size() - 1);
        std::uniform_int_distribution<std::size_t> suffix_dist(0, suffixes.size() - 1);
        std::uniform_int_distribution<int> num_dist(1, 999);

        auto action = actions[action_dist(rng_)];
        auto suffix = suffixes[suffix_dist(rng_)];

        // Replace {} with something based on the derivation name
        std::string result = action;
        auto pos = result.find("{}");
        if (pos != std::string::npos) {
            // Use part of name or generate something
            std::string replacement = name.substr(0, std::min(name.size(), std::size_t(8)));
            replacement += std::to_string(num_dist(rng_));
            replacement += suffix;
            result.replace(pos, 2, replacement);
        }

        return result;
    }

    // Generate fake download progress line
    std::string random_download_line(const std::string& name)
    {
        static const std::vector<std::string> templates = {
            "copying path '/nix/store/...-{}'",
            "  % Total    % Received",
            "downloading 'https://cache.nixos.org/...'",
            "  {} KiB / {} KiB",
            "fetching {} from binary cache",
            "copying {} from 'https://cache.nixos.org'",
        };

        std::uniform_int_distribution<std::size_t> templ_dist(0, templates.size() - 1);
        std::uniform_int_distribution<int> size_dist(100, 50000);

        auto templ = templates[templ_dist(rng_)];
        std::string result = templ;

        // Replace first {} with name
        auto pos = result.find("{}");
        if (pos != std::string::npos) {
            result.replace(pos, 2, name.substr(0, std::min(name.size(), std::size_t(20))));
        }
        // Replace second {} with size
        pos = result.find("{}");
        if (pos != std::string::npos) {
            result.replace(pos, 2, std::to_string(size_dist(rng_)));
        }

        return result;
    }

    // A single goal coroutine - mirrors Nix's DerivationGoal
    nxb::task<> run_goal(NodeIndex idx)
    {
        auto* node = graph_.get(idx);

        // Check for shutdown before starting
        if (runtime_.shutdown_requested()) co_return;

        emit(GoalStarted{idx, node->name, node->inputs.size()});

        // Phase 1: Wait for all input dependencies
        if (!node->inputs.empty()) {
            emit(WaitingForDeps{idx, node->inputs.size()});

            for (auto dep_idx : node->inputs) {
                if (runtime_.shutdown_requested()) co_return;
                co_await *goal_complete_[dep_idx];
            }

            emit(DepsComplete{idx});
        }

        // Check for shutdown before acquiring slot
        if (runtime_.shutdown_requested()) co_return;

        // Phase 2: Acquire a slot (build or substitution)
        bool is_sub = will_substitute_[idx];
        if (is_sub) {
            co_await subst_slots_.acquire();
        } else {
            co_await build_slots_.acquire();
        }

        // Check for shutdown after acquiring slot (release it if shutting down)
        if (runtime_.shutdown_requested()) {
            if (is_sub) subst_slots_.release();
            else build_slots_.release();
            co_return;
        }

        emit(SlotAcquired{idx, is_sub});

        // Phase 3: Do the "work" (simulate with sleep and fake output)
        auto duration = durations_[idx];
        emit(WorkStarted{idx, duration, is_sub});

        // Sleep for scaled duration, optionally emitting build output
        auto real_duration = std::chrono::milliseconds(
            static_cast<int>(duration.count() * config.time_scale));

        if (config.verbose && real_duration.count() > 20) {
            // Emit fake output lines periodically during the work
            auto interval = std::chrono::milliseconds(15 + (rng_() % 35)); // 15-50ms
            auto elapsed = std::chrono::milliseconds(0);

            while (elapsed < real_duration && !runtime_.shutdown_requested()) {
                auto sleep_time = std::min(interval, real_duration - elapsed);
                co_await runtime_.sleep(sleep_time);
                elapsed += sleep_time;

                // Emit a fake output line
                if (elapsed < real_duration && !runtime_.shutdown_requested()) {
                    std::string line = is_sub
                        ? random_download_line(node->name)
                        : random_build_line(node->name);
                    emit(BuildOutput{idx, std::move(line)});
                }
            }
        } else {
            co_await runtime_.sleep(real_duration);
        }

        // Release slot before checking shutdown
        if (is_sub) {
            subst_slots_.release();
        } else {
            build_slots_.release();
        }

        // Check for shutdown before completing
        if (runtime_.shutdown_requested()) co_return;

        emit(WorkComplete{idx, is_sub});

        ++completed_count_;
        goal_complete_[idx]->set();

        emit(GoalComplete{idx, true});
    }

    nxb::ui::UIRuntime& runtime_;
    drv::Graph& graph_;
    nxb::queue<Event>& events_;
    std::mt19937 rng_;

    Semaphore build_slots_;
    Semaphore subst_slots_;

    std::vector<bool> will_substitute_;
    std::vector<SimTime> durations_;
    std::vector<std::unique_ptr<nxb::event>> goal_complete_;

    std::atomic<std::size_t> completed_count_{0};
};

} // namespace nxb::sim
