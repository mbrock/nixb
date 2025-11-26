#include "log-replay.hpp"
#include "nix-log-adapter.hpp"

#include <nxt/ansi.hpp>
#include <nxt/app.hpp>
#include <nxt/async.hpp>
#include <nxt/tui.hpp>

#include <CLI/CLI.hpp>
#include <fmt/core.h>

using namespace nixb;
using namespace nxb::tui;

namespace {
using nix_event::NixLogEvent;

struct PlaybackState {
    std::string file;
    double speed = 1.0;
    bool done = false;
};

auto build_play_ui(const PlaybackState& state)
{
    return column(
        hrule(),
        text(fmt::format("Playing: {}", state.file), fg(nxb::Rgba8::cyan())),
        text(state.done ? "Done" : "Playing...", fg(nxb::Rgba8::green())),
        progress_bar(0.5 * mp_units::one));
}

struct EventHandler {
    nxb::ui::UIRuntime& runtime;

    // Top-level event handlers
    void operator()(const nix_event::LogLine& ev) { runtime.println(ev.text); }

    void operator()(const nix_event::ActivityStarted& ev)
    {
        std::visit(*this, ev.kind);
    }

    void operator()(const nix_event::ActivityProgress&)
    {
        // Ignore progress updates for now
    }

    void operator()(const nix_event::ActivityPhase&)
    {
        // Ignore phase updates for now
    }

    void operator()(const nix_event::ActivityFinished&)
    {
        // Ignore finished events for now
    }

    void operator()(const nix_event::Error& ev)
    {
        runtime.println(fmt::format("error: {}", ev.info.msg.str()));
    }

    void operator()(const nix_event::activity::Build& kind)
    {
        runtime.println(fmt::format("building {}", kind.drv_path));
    }

    void operator()(const nix_event::activity::Download& kind)
    {
        runtime.println(fmt::format("downloading {}", kind.url));
    }

    void operator()(const nix_event::activity::Copy& kind)
    {
        runtime.println(fmt::format("copying {}", kind.path));
    }

    void operator()(const nix_event::activity::Realise& kind)
    {
        runtime.println(fmt::format("realising {}", kind.path));
    }

    void operator()(const nix_event::activity::Substitute& kind)
    {
        runtime.println(fmt::format("substituting {}", kind.path));
    }

    void operator()(const nix_event::activity::QueryPathInfo& kind)
    {
        runtime.println(fmt::format("querying path info for {}", kind.path));
    }

    void operator()(const nix_event::activity::PostBuildHook& kind)
    {
        runtime.println(fmt::format("post-build hook for {}", kind.drv_path));
    }

    void operator()(const nix_event::activity::BuildWaiting&)
    {
        runtime.println("build waiting");
    }

    void operator()(const nix_event::activity::Unknown& kind)
    {
        runtime.println(fmt::format("unknown activity: {}", kind.text));
    }
};

nxb::task<> consume_events(nxb::ui::UIRuntime& runtime,
    nxb::queue<nix_event::NixLogEvent>& events)
{
    while (!runtime.shutdown_requested()) {
        auto event = co_await events.pop();
        if (!event)
            break;

        std::visit(EventHandler { runtime }, *event);
        runtime.signal_damage();
    }
}

nxb::task<> run_replay(nxb::ui::UIRuntime& runtime, PlaybackState& state,
    nxb::queue<NixLogEvent>& events)
{
    co_await nixb::replay::replay_file(runtime, state.file, events, true,
        state.speed);
    state.done = true;
    runtime.signal_damage();
    co_await events.shutdown();
}

nxb::task<> update_play(nxb::ui::UIRuntime& runtime, PlaybackState& state)
{
    nxb::queue<NixLogEvent> events;

    co_await runtime.run(consume_events(runtime, events),
        run_replay(runtime, state, events));
}

int cmd_play(const std::string& file, double speed)
{
    nxb::ansi::init();
    return nxb::ui::run(PlaybackState { .file = file, .speed = speed },
        build_play_ui, update_play);
}

} // anonymous namespace

int main(int argc, char** argv)
{
    CLI::App app { "nxb - Nix build UI" };

    std::string play_file;
    double speed = 1.0;

    auto* play_cmd = app.add_subcommand("play", "Replay a recorded nix build log");
    play_cmd->add_option("file", play_file, "Log file to replay (.tnixlog)")
        ->required();
    play_cmd->add_option("-s,--speed", speed, "Playback speed multiplier")
        ->default_val(1.0);

    CLI11_PARSE(app, argc, argv);

    if (play_cmd->parsed())
        return cmd_play(play_file, speed);

    // No subcommand - show help
    fmt::print("{}", app.help());
    return 0;
}
