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

namespace
{
  using nix_event::NixLogEvent;

  struct PlaybackState
  {
    std::string file;
    double speed = 1.0;
    bool done = false;
  };

  auto
  build_play_ui (
    const PlaybackState &state)
  {
    return column (hrule (),
                   text (fmt::format ("Playing: {}", state.file),
                         fg (nxb::Rgba8::cyan ())),
                   text (state.done ? "Done" : "Playing...",
                         fg (nxb::Rgba8::green ())),
                   progress_bar (0.5 * mp_units::one));
  }

  nxb::task<>
  consume_events (
    nxb::ui::UIRuntime &runtime,
    nxb::queue<nix_event::NixLogEvent> &events)
  {
    while (!runtime.shutdown_requested ())
      {
        auto event = co_await events.pop ();
        if (!event)
          break;

        std::visit (
          [&runtime] (auto &&ev) {
            using T = std::decay_t<decltype (ev)>;

            if constexpr (std::is_same_v<T, nix_event::LogLine>)
              runtime.println (ev.text);
            else if constexpr (std::is_same_v<
                                 T,
                                 nix_event::ActivityStarted>)
              std::visit (
                [&runtime] (auto &&kind) {
                  using K = std::decay_t<decltype (kind)>;
                  if constexpr (std::is_same_v<
                                  K,
                                  nix_event::activity::Build>)
                    runtime.println (
                      fmt::format ("building {}", kind.drv_path));
                  else if constexpr (
                    std::is_same_v<K,
                                   nix_event::activity::Download>)
                    runtime.println (
                      fmt::format ("downloading {}", kind.url));
                  else if constexpr (std::is_same_v<
                                       K,
                                       nix_event::activity::Copy>)
                    runtime.println (
                      fmt::format ("copying {}", kind.path));
                  else if constexpr (
                    std::is_same_v<K,
                                   nix_event::activity::Substitute>)
                    runtime.println (
                      fmt::format ("substituting {}", kind.path));
                },
                ev.kind);
            else if constexpr (std::is_same_v<T, nix_event::Error>)
              runtime.println (
                fmt::format ("error: {}", ev.info.msg.str ()));
          },
          *event);

        runtime.signal_damage ();
      }
  }

  nxb::task<>
  run_replay (
    nxb::ui::UIRuntime &runtime,
    PlaybackState &state,
    nxb::queue<NixLogEvent> &events)
  {
    co_await nixb::replay::replay_file (
      runtime, state.file, events, true, state.speed);
    state.done = true;
    runtime.signal_damage ();
    co_await events.shutdown ();
  }

  nxb::task<>
  update_play (
    nxb::ui::UIRuntime &runtime, PlaybackState &state)
  {
    nxb::queue<NixLogEvent> events;

    co_await runtime.run (consume_events (runtime, events),
                          run_replay (runtime, state, events));
  }

  int
  cmd_play (
    const std::string &file, double speed)
  {
    nxb::ansi::init ();
    return nxb::ui::run (
      PlaybackState{ .file = file, .speed = speed },
      build_play_ui,
      update_play);
  }

} // anonymous namespace

int
main (
  int argc, char **argv)
{
  CLI::App app{ "nxb - Nix build UI" };

  std::string play_file;
  double speed = 1.0;

  auto *play_cmd = app.add_subcommand (
    "play", "Replay a recorded nix build log");
  play_cmd
    ->add_option (
      "file", play_file, "Log file to replay (.tnixlog)")
    ->required ();
  play_cmd
    ->add_option ("-s,--speed", speed, "Playback speed multiplier")
    ->default_val (1.0);

  CLI11_PARSE (app, argc, argv);

  if (play_cmd->parsed ())
    return cmd_play (play_file, speed);

  // No subcommand - show help
  fmt::print ("{}", app.help ());
  return 0;
}
