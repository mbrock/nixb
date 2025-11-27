
#include <sys/stat.h>
#include <exception>

// #include <boost/stacktrace.hpp>
// #include <boost/stacktrace/frame.hpp>
// #include <boost/stacktrace/stacktrace.hpp>
#include <fmt/base.h>
#include <fmt/color.h>
#include <fmt/core.h>

#include <CLI/CLI.hpp>

#include <nxt/ansi.hpp>
#include <nxt/app.hpp>
#include <nxt/async.hpp>
#include <nxt/tui.hpp>

#include <nix/util/error.hh>

#include "log-replay.hpp"
#include "nix-api.hpp"
#include "nix-log-adapter.hpp"
#include "drv-graph.hpp"
#include "build-sim.hpp"

// void my_terminate_handler() {
//   try {
//     std::cerr << boost::stacktrace::stacktrace();
//   } catch (...) {
//   }
//   std::abort();
// }

using namespace nixb;
using namespace nxb::tui;

namespace {
using nix_event::NixLogEvent;

struct PlaybackState
{
    std::string file;
    double speed = 1.0;
    bool done = false;
};

auto build_play_ui(const PlaybackState & state)
{
    return column(
        hrule(),
        text(fmt::format("Playing: {}", state.file), fg(nxb::Rgba8::cyan())),
        text(state.done ? "Done" : "Playing...", fg(nxb::Rgba8::green())),
        progress_bar(0.5 * mp_units::one));
}

struct EventHandler
{
    nxb::ui::UIRuntime & runtime;

    // Top-level event handlers
    void operator()(const nix_event::LogLine & ev)
    {
        auto x = (1.0 - static_cast<float>(ev.level) / static_cast<float>(nix::Verbosity::lvlVomit) * 200.0);
        fmt::println(
            "{} {}",
            fmt::styled(static_cast<int>(ev.level), fmt::fg(fmt::color::gray)),
            fmt::styled(ev.text, fmt::fg(fmt::rgb(x, x, x))));
    }

    void operator()(const nix_event::ActivityStarted & ev)
    {
        std::visit(*this, ev.kind);
    }

    void operator()(const nix_event::ActivityProgress &)
    {
        // Ignore progress updates for now
    }

    void operator()(const nix_event::ActivityPhase &)
    {
        // Ignore phase updates for now
    }

    void operator()(const nix_event::ActivityFinished &)
    {
        // Ignore finished events for now
    }

    void operator()(const nix_event::Error & ev)
    {
        runtime.println(fmt::format("error: {}", ev.info.msg.str()));
    }

    void operator()(const nix_event::activity::Build & kind)
    {
        runtime.println(fmt::format("building {}", kind.drv_path));
    }

    void operator()(const nix_event::activity::Download & kind)
    {
        runtime.println(fmt::format("downloading {}", kind.url));
    }

    void operator()(const nix_event::activity::Copy & kind)
    {
        runtime.println(fmt::format("copying {}", kind.path));
    }

    void operator()(const nix_event::activity::Realise & kind)
    {
        runtime.println(fmt::format("realising {}", kind.path));
    }

    void operator()(const nix_event::activity::Substitute & kind)
    {
        runtime.println(fmt::format("substituting {}", kind.path));
    }

    void operator()(const nix_event::activity::QueryPathInfo & kind)
    {
        runtime.println(fmt::format("querying path info for {}", kind.path));
    }

    void operator()(const nix_event::activity::PostBuildHook & kind)
    {
        runtime.println(fmt::format("post-build hook for {}", kind.drv_path));
    }

    void operator()(const nix_event::activity::BuildWaiting &)
    {
        runtime.println("build waiting");
    }

    void operator()(const nix_event::activity::Unknown & kind)
    {
        runtime.println(fmt::format("unknown activity: {}", kind.text));
    }
};

nxb::task<> consume_events(nxb::ui::UIRuntime & runtime, nxb::queue<nix_event::NixLogEvent> & events)
{
    while (!runtime.shutdown_requested()) {
        auto event = co_await events.pop();
        if (!event)
            break;

        std::visit(EventHandler{runtime}, *event);
        runtime.signal_damage();
    }
}

nxb::task<> run_replay(nxb::ui::UIRuntime & runtime, PlaybackState & state, nxb::queue<NixLogEvent> & events)
{
    co_await nixb::replay::replay_file(runtime, state.file, events, true, state.speed);
    state.done = true;
    runtime.signal_damage();
    co_await events.shutdown();
}

nxb::task<> update_play(nxb::ui::UIRuntime & runtime, PlaybackState & state)
{
    nxb::queue<NixLogEvent> events;

    co_await runtime.run(consume_events(runtime, events), run_replay(runtime, state, events));
}

int cmd_play(const std::string & file, double speed)
{
    nxb::ansi::init();
    return nxb::ui::run(PlaybackState{.file = file, .speed = speed}, build_play_ui, update_play);
}

struct DeriveState
{
    std::string installable;
    bool done = false;
    std::string result_path;
    std::string error_msg;
};

auto build_derive_ui(const DeriveState & state)
{
    return column(
        hrule(),
        text(fmt::format("Deriving: {}", state.installable), fg(nxb::Rgba8::cyan())),
        text(
            state.done ? (state.error_msg.empty() ? "Done" : "Error") : "Evaluating...",
            fg(state.done && state.error_msg.empty() ? nxb::Rgba8::green() : nxb::Rgba8::yellow())));
}

nxb::task<> run_derive(nxb::ui::UIRuntime & runtime, DeriveState & state, nxb::queue<NixLogEvent> & events)
{
    // Set up our logger to capture Nix logs
    auto adapter = std::make_unique<nixb::coro_adapter::NixLogAdapter>(events);
    nix::logger = std::move(adapter);

    // // Enable verbose logging to get evaluation output
    nix::verbosity = nix::lvlDebug;
    //  nix::loggerSettings.showTrace = true;

    runtime.println("Starting evaluation with TrivialStore...");

    try {
        // Create TrivialStore wired to the UI runtime
        nxb::NixContext ctx(runtime);
        runtime.println(fmt::format("Store: {}", ctx.store()->config.getHumanReadableURI()));
        auto drv_paths = nxb::resolve_installable(ctx, state.installable);

        if (!drv_paths.empty()) {
            state.result_path = ctx.store()->printStorePath(drv_paths[0]);
            runtime.println(fmt::format("Result: {}", state.result_path));

            auto info = read_derivation_info(ctx, drv_paths[0]);
            if (info) {
                runtime.println(fmt::format("name: {}", info->name));
                runtime.println(fmt::format("system: {}", info->system));
                runtime.println(fmt::format("input_drvs: {}", info->input_drvs.size()));
            }
        }

        co_await runtime.sleep(std::chrono::seconds(1));
    } catch (std::exception & e) {
        state.error_msg = e.what();
        runtime.println(fmt::format("error: {}", e.what()));
    }

    state.done = true;
    runtime.signal_damage();
    co_await events.shutdown();
}

nxb::task<> update_derive(nxb::ui::UIRuntime & runtime, DeriveState & state)
{
    nxb::queue<NixLogEvent> events;

    co_await runtime.run(consume_events(runtime, events), run_derive(runtime, state, events));
}

int cmd_derive(const std::string & installable)
{
    nxb::ansi::init();
    return nxb::ui::run(DeriveState{.installable = installable}, build_derive_ui, update_derive);
}

struct BuildState
{
    std::string installable;
    bool done = false;
    std::string error_msg;
    std::vector<std::string> build_results;
};

auto build_build_ui(const BuildState & state)
{
    auto results_text =
        state.build_results.empty() ? text("") : text(fmt::format("{} results", state.build_results.size()));

    return column(
        hrule(),
        text(fmt::format("Building: {}", state.installable), fg(nxb::Rgba8::cyan())),
        text(
            state.done ? (state.error_msg.empty() ? "Done" : "Error") : "Building...",
            fg(state.done && state.error_msg.empty() ? nxb::Rgba8::green() : nxb::Rgba8::yellow())),
        results_text);
}

nxb::task<> run_build(nxb::ui::UIRuntime & runtime, BuildState & state, nxb::queue<NixLogEvent> & events)
{
    auto adapter = std::make_unique<nixb::coro_adapter::NixLogAdapter>(events);
    nix::logger = std::move(adapter);
    nix::verbosity = nix::lvlChatty;

    runtime.println(fmt::format("Building {} with TrivialStore...", state.installable));

    try {
        // Create TrivialStore wired to the UI runtime for output
        nxb::NixContext ctx(runtime);
        runtime.println(fmt::format("Store: {}", ctx.store()->config.getHumanReadableURI()));

        auto drv_paths = nxb::resolve_installable(ctx, state.installable);
        runtime.println(fmt::format("Resolved {} derivation(s)", drv_paths.size()));

        if (drv_paths.empty()) {
            state.error_msg = "No derivations found";
        } else {
            std::vector<nix::DerivedPath> to_build;
            for (const auto & drv_path : drv_paths) {
                runtime.println(fmt::format("  drv: {}", ctx.store()->printStorePath(drv_path)));
                to_build.push_back(
                    nix::DerivedPath::Built{
                        .drvPath = nix::makeConstantStorePathRef(drv_path),
                        .outputs = nix::OutputsSpec::All{},
                    });
            }

            runtime.println("\nCalling buildPathsWithResults...");
            auto results = ctx.store()->buildPathsWithResults(to_build, nix::bmNormal);

            runtime.println(fmt::format("\nBuild results: {}", results.size()));
            for (const auto & result : results) {
                auto status = result.tryGetSuccess() ? "success" : "failed";
                auto path_str = result.path.to_string(*ctx.store());
                runtime.println(fmt::format("  path: {} status: {}", path_str, status));
                state.build_results.push_back(fmt::format("{}: {}", path_str, status));

                if (auto * failure = result.tryGetFailure()) {
                    runtime.println(fmt::format("    error: {}", failure->errorMsg));
                }
            }
        }
    } catch (std::exception & e) {
        state.error_msg = e.what();
        runtime.println(fmt::format("Error: {}", e.what()));
    }

    state.done = true;
    runtime.signal_damage();
    runtime.request_shutdown();
    co_await events.shutdown();
}

nxb::task<> update_build(nxb::ui::UIRuntime & runtime, BuildState & state)
{
    nxb::queue<NixLogEvent> events;
    co_await runtime.run(consume_events(runtime, events), run_build(runtime, state, events));
}

int cmd_build(const std::string & installable)
{
    nxb::ansi::init();
    return nxb::ui::run(BuildState{.installable = installable}, build_build_ui, update_build);
}

// ============================================================================
// Graph command - show derivation dependency graph
// ============================================================================

int cmd_graph(const std::string & installable, bool show_critical_path, bool show_stats)
{
    try {
        nxb::NixContext ctx;

        fmt::print("Resolving {}...\n", installable);
        auto roots = nxb::resolve_installable(ctx, installable);

        if (roots.empty()) {
            fmt::print(stderr, "No derivations found for {}\n", installable);
            return 1;
        }

        fmt::print("Building dependency graph...\n");
        auto graph = nxb::drv::build_graph(ctx, roots);

        // Print basic info
        fmt::print("\n");
        fmt::print(
            fmt::emphasis::bold,
            "Derivation Graph for {}\n",
            installable);
        fmt::print("─────────────────────────────────────────\n");

        // Show roots
        fmt::print(
            fmt::fg(fmt::color::cyan),
            "Roots ({}):\n",
            graph.roots.size());
        for (auto idx : graph.roots) {
            auto * node = graph.get(idx);
            fmt::print(
                "  {} (height={}, subtree={})\n",
                node->name,
                node->height,
                node->subtree_size);
        }

        // Show statistics
        if (show_stats) {
            auto stats = nxb::drv::compute_stats(graph);
            fmt::print("\n");
            fmt::print(fmt::fg(fmt::color::yellow), "Statistics:\n");
            fmt::print("  Total derivations: {}\n", stats.total_nodes);
            fmt::print("  Leaf nodes:        {}\n", stats.leaf_count);
            fmt::print("  Max depth:         {}\n", stats.max_depth);
            fmt::print("  Max height:        {}\n", stats.max_height);
            fmt::print("  Max fan-in:        {}\n", stats.max_fan_in);
            fmt::print("  Max fan-out:       {}\n", stats.max_fan_out);
            fmt::print("  Avg inputs:        {:.2f}\n", stats.avg_inputs);

            fmt::print("\n");
            fmt::print(fmt::fg(fmt::color::yellow), "Nodes per depth:\n");
            for (std::size_t d = 0; d < stats.nodes_per_depth.size(); ++d) {
                fmt::print("  depth {}: {}\n", d, stats.nodes_per_depth[d]);
            }
        }

        // Show critical path
        if (show_critical_path) {
            auto path = nxb::drv::critical_path(graph);
            fmt::print("\n");
            fmt::print(fmt::fg(fmt::color::magenta), "Critical path ({} nodes):\n", path.size());
            for (std::size_t i = 0; i < path.size(); ++i) {
                auto * node = path[i];
                std::string indent(i * 2, ' ');
                fmt::print(
                    "{}└─ {} (depth={}, inputs={})\n",
                    indent,
                    node->name,
                    node->depth,
                    node->inputs.size());
            }
        }

        // Show build order (first 20)
        fmt::print("\n");
        fmt::print(fmt::fg(fmt::color::green), "Build order (first 20):\n");
        auto order = graph.topological_order();
        for (std::size_t i = 0; i < std::min(order.size(), std::size_t{20}); ++i) {
            fmt::print("  {:3}. {}\n", i + 1, order[i]->name);
        }
        if (order.size() > 20) {
            fmt::print("  ... and {} more\n", order.size() - 20);
        }

        return 0;

    } catch (const std::exception & e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return 1;
    }
}

// ============================================================================
// Simulate command - simulated build execution with TUI
// ============================================================================

// Simulation options from CLI
struct SimOptions
{
    float time_scale = 0.1f;      // Real time per sim time (lower = faster)
    std::size_t max_jobs = 4;
    std::size_t max_subs = 16;
    float sub_rate = 0.75f;       // Probability of substitution vs build
    bool verbose = false;         // Print build output spam
};

struct SimState
{
    std::string installable;
    SimOptions options;
    std::unique_ptr<nxb::drv::Graph> graph;
    std::unique_ptr<nxb::sim::Simulator> sim;

    // Display state
    std::size_t completed = 0;
    std::size_t total = 0;
    double progress = 0.0;
    bool done = false;
    std::string error_msg;

    // Active jobs for HUD display
    struct ActiveJob
    {
        nxb::sim::NodeIndex node;
        std::string name;
        nxb::sim::SimTime duration;
        bool is_substitution;
    };
    std::vector<ActiveJob> active_jobs;
};

auto build_sim_ui(const SimState & state)
{
    auto progress_pct = static_cast<float>(state.progress * 100.0) * mp_units::percent;

    // Count builds vs subs
    std::size_t num_builds = 0, num_subs = 0;
    for (const auto & job : state.active_jobs) {
        if (job.is_substitution)
            ++num_subs;
        else
            ++num_builds;
    }

    auto status_text = state.done
        ? (state.error_msg.empty() ? "Complete!" : "Error")
        : fmt::format(
              "Building... {}/{} ({} builds, {} subs)",
              state.completed,
              state.total,
              num_builds,
              num_subs);

    auto status_color = state.done
        ? (state.error_msg.empty() ? nxb::Rgba8::green() : nxb::Rgba8::red())
        : nxb::Rgba8::yellow();

    // Build list of active job displays using the list() primitive
    auto jobs_list = list(state.active_jobs, [](const auto & job) {
        auto prefix = job.is_substitution ? "↓" : "⚙";
        auto color = job.is_substitution ? nxb::Rgba8::cyan() : nxb::Rgba8::yellow();
        return text(fmt::format(" {} {}", prefix, job.name), fg(color));
    });

    return column(
        hrule(),
        text(fmt::format("Simulating: {}", state.installable), fg(nxb::Rgba8::cyan())),
        text(status_text, fg(status_color)),
        std::move(jobs_list),
        progress_bar(progress_pct),
        hrule());
}

// Consume simulation events and update UI state
nxb::task<> consume_sim_events(
    nxb::ui::UIRuntime & runtime,
    SimState & state,
    nxb::queue<nxb::sim::Event> & events)
{
    while (auto ev = co_await events.pop()) {
        std::visit(
            [&](auto && e) {
                using T = std::decay_t<decltype(e)>;

                if constexpr (std::is_same_v<T, nxb::sim::WorkStarted>) {
                    auto * node = state.graph->get(e.node);
                    // Add to active jobs list for HUD display
                    state.active_jobs.push_back({
                        .node = e.node,
                        .name = node->name,
                        .duration = e.duration,
                        .is_substitution = e.is_substitution});
                } else if constexpr (std::is_same_v<T, nxb::sim::WorkComplete>) {
                    // Remove from active jobs list
                    std::erase_if(state.active_jobs, [&](const auto & job) {
                        return job.node == e.node;
                    });
                    ++state.completed;
                    state.progress =
                        static_cast<double>(state.completed) / state.total;
                } else if constexpr (std::is_same_v<T, nxb::sim::BuildOutput>) {
                    // Print fake build output to scroll region
                    auto * node = state.graph->get(e.node);
                    runtime.println(fmt::format("  {} > {}", node->name, e.line));
                }
            },
            *ev);

        runtime.signal_damage();
    }
}

nxb::task<>
run_simulation(
    nxb::ui::UIRuntime & runtime,
    SimState & state,
    nxb::queue<nxb::sim::Event> & events)
{
    // Set up Nix context and resolve installable
    runtime.println(fmt::format("Resolving {}...", state.installable));

    try {
        nxb::NixContext ctx;
        auto roots = nxb::resolve_installable(ctx, state.installable);

        if (roots.empty()) {
            state.error_msg = "No derivations found";
            state.done = true;
            runtime.signal_damage();
            co_return;
        }

        runtime.println(fmt::format("Found {} root derivation(s)", roots.size()));
        runtime.println("Building dependency graph...");

        state.graph = std::make_unique<nxb::drv::Graph>(
            nxb::drv::build_graph(ctx, roots));

        runtime.println(fmt::format(
            "Graph: {} derivations, max depth {}",
            state.graph->total_derivations(),
            state.graph->max_depth()));

        state.total = state.graph->total_derivations();

        // Create simulator with event queue
        state.sim = std::make_unique<nxb::sim::Simulator>(runtime, *state.graph, events);
        state.sim->config.max_jobs = state.options.max_jobs;
        state.sim->config.max_substitutions = state.options.max_subs;
        state.sim->config.substitution_rate = state.options.sub_rate;
        state.sim->config.time_scale = state.options.time_scale;
        state.sim->config.verbose = state.options.verbose;

        // Get root indices
        std::vector<std::size_t> root_indices;
        for (const auto & root : roots) {
            auto it = state.graph->path_to_index.find(root);
            if (it != state.graph->path_to_index.end()) {
                root_indices.push_back(it->second);
            }
        }

        runtime.println("Starting simulation...\n");

        // Run the coroutine-based simulation
        co_await state.sim->build(root_indices);

        runtime.println("\nSimulation complete!");

    } catch (const std::exception & e) {
        state.error_msg = e.what();
        runtime.println(fmt::format("Error: {}", e.what()));
    }

    state.done = true;
    runtime.signal_damage();

    // Shutdown the event queue so consumer exits
    co_await events.shutdown();

    // Wait a bit before exiting so user can see final state
    co_await runtime.sleep(std::chrono::seconds{2});
    runtime.request_shutdown();
}

nxb::task<> update_simulation(nxb::ui::UIRuntime & runtime, SimState & state)
{
    nxb::queue<nxb::sim::Event> events;

    co_await runtime.run(
        run_simulation(runtime, state, events),
        consume_sim_events(runtime, state, events));
}

int cmd_simulate(
    const std::string & installable,
    const SimOptions & options,
    bool debug_ansi,
    bool force_ansi)
{
    nxb::ansi::init();
    if (debug_ansi) {
        nxb::ansi::mode = nxb::ansi::Mode::debug;
    } else if (force_ansi) {
        nxb::ansi::mode = nxb::ansi::Mode::enabled;
    }
    return nxb::ui::run(
        SimState{.installable = installable, .options = options},
        build_sim_ui,
        update_simulation);
}

} // anonymous namespace

int main(int argc, char ** argv)
{
    // std::set_terminate(&my_terminate_handler);

    CLI::App app{"nxb - Nix build UI"};

    std::string play_file;
    double speed = 1.0;

    auto * play_cmd = app.add_subcommand("play", "Replay a recorded nix build log");
    play_cmd->add_option("file", play_file, "Log file to replay (.tnixlog)")->required();
    play_cmd->add_option("-s,--speed", speed, "Playback speed multiplier")->default_val(1.0);

    std::string installable;
    auto * derive_cmd = app.add_subcommand("derive", "Resolve a flake installable to derivation paths");
    derive_cmd->add_option("installable", installable, "Flake installable (e.g. .#default)")->required();

    std::string build_installable;
    auto * build_cmd = app.add_subcommand("build", "Build a flake installable (using TrivialStore)");
    build_cmd->add_option("installable", build_installable, "Flake installable (e.g. .#default)")->required();

    std::string graph_installable;
    bool graph_critical_path = false;
    bool graph_stats = false;
    auto * graph_cmd = app.add_subcommand("graph", "Show derivation dependency graph");
    graph_cmd->add_option("installable", graph_installable, "Flake installable (e.g. nixpkgs#hello)")->required();
    graph_cmd->add_flag("-c,--critical-path", graph_critical_path, "Show critical (longest) dependency path");
    graph_cmd->add_flag("-s,--stats", graph_stats, "Show graph statistics");

    std::string sim_installable;
    SimOptions sim_opts;
    bool sim_debug_ansi = false;
    bool sim_force_ansi = false;
    auto * sim_cmd = app.add_subcommand("simulate", "Simulate build execution with fake timings");
    sim_cmd->add_option("installable", sim_installable, "Flake installable (e.g. nixpkgs#hello)")->required();
    sim_cmd->add_option("-s,--speed", sim_opts.time_scale, "Time scale (lower = faster, default 0.1)")->default_val(0.1f);
    sim_cmd->add_option("-j,--jobs", sim_opts.max_jobs, "Max concurrent build jobs (default 4)")->default_val(4);
    sim_cmd->add_option("-S,--subs", sim_opts.max_subs, "Max concurrent substitutions (default 16)")->default_val(16);
    sim_cmd->add_option("-r,--sub-rate", sim_opts.sub_rate, "Substitution probability 0-1 (default 0.75)")->default_val(0.75f);
    sim_cmd->add_flag("-v,--verbose", sim_opts.verbose, "Print fake build output spam");
    sim_cmd->add_flag("--debug-ansi", sim_debug_ansi, "Print ANSI escapes in readable debug format");
    sim_cmd->add_flag("--force-ansi", sim_force_ansi, "Force real ANSI output even if not a TTY");

    CLI11_PARSE(app, argc, argv);

    if (play_cmd->parsed())
        return cmd_play(play_file, speed);

    if (derive_cmd->parsed())
        return cmd_derive(installable);

    if (build_cmd->parsed())
        return cmd_build(build_installable);

    if (graph_cmd->parsed())
        return cmd_graph(graph_installable, graph_critical_path, graph_stats);

    if (sim_cmd->parsed())
        return cmd_simulate(sim_installable, sim_opts, sim_debug_ansi, sim_force_ansi);

    // No subcommand - show help
    fmt::print("{}", app.help());
    return 0;
}
