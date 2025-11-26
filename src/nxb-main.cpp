#include "log-replay.hpp"
#include "nix-log-adapter.hpp"

#include <nxt/ansi.hpp>
#include <nxt/app.hpp>
#include <nxt/tui.hpp>

#include <CLI/CLI.hpp>
#include <coro/coro.hpp>
#include <coro/queue.hpp>
#include <fmt/core.h>
#include <mp-units/framework.h>

using namespace nixb;
using namespace nxb::tui;

namespace
{

  /// Simple consumer that prints events to the scroll region.
  coro::task<>
  event_consumer (nxb::ui::UIRuntime &runtime,
                  coro::queue<nix_event::Event> &queue)
  {
    co_await runtime.scheduler ().schedule ();

    while (!runtime.shutdown_requested ())
      {
        auto event = co_await queue.pop ();
        if (!event)
          break;

        std::visit (
          [&runtime] (auto &&ev)
            {
              using T = std::decay_t<decltype (ev)>;

              if constexpr (std::is_same_v<T, nix_event::LogLine>)
                {
                  runtime.println (ev.text);
                }
              else if constexpr (std::is_same_v<
                                   T, nix_event::ActivityStarted>)
                {
                  std::visit (
                    [&runtime] (auto &&kind)
                      {
                        using K = std::decay_t<decltype (kind)>;
                        if constexpr (std::is_same_v<
                                        K, nix_event::activity::Build>)
                          runtime.println (
                            fmt::format ("building {}", kind.drv_path));
                        else if constexpr (std::is_same_v<
                                             K, nix_event::activity::
                                                  Download>)
                          runtime.println (
                            fmt::format ("downloading {}", kind.url));
                        else if constexpr (std::is_same_v<
                                             K,
                                             nix_event::activity::Copy>)
                          runtime.println (
                            fmt::format ("copying {}", kind.path));
                        else if constexpr (std::is_same_v<
                                             K, nix_event::activity::
                                                  Substitute>)
                          runtime.println (
                            fmt::format ("substituting {}", kind.path));
                      },
                    ev.kind);
                }
              else if constexpr (std::is_same_v<T, nix_event::Error>)
                {
                  runtime.println (
                    fmt::format ("error: {}", ev.info.msg.str ()));
                }
            },
          *event);

        runtime.signal_damage ();
      }
  }

  /// Build a simple HUD showing playback status.
  struct PlaybackState
  {
    std::string file;
    bool done = false;
  };

  auto
  build_ui (const PlaybackState &state)
  {
    return column (hrule (),
                   text (fmt::format ("Playing: {}", state.file),
                         fg (nxb::Rgba8::cyan ())),
                   text (state.done ? "Done" : "Playing...",
                         fg (nxb::Rgba8::green ())),
                   progress_bar (0.5 * mp_units::one));
  }

  int
  cmd_play (const std::string &file, double speed)
  {
    nxb::ui::UIRuntime runtime;
    PlaybackState state{ .file = file };
    coro::queue<nix_event::Event> queue;

    try
      {
        nxb::ui::TerminalGuard guard;
        std::vector<coro::task<>> tasks;

        tasks.push_back (runtime.signal_loop ());

        tasks.push_back (runtime.run_render_loop (
          [&state] { return build_ui (state); }));

        tasks.push_back (event_consumer (runtime, queue));

        auto replay_task
          = [] (nxb::ui::UIRuntime &runtime, PlaybackState &state,
                const std::string &path,
                coro::queue<nix_event::Event> &queue,
                std::stop_token stoken, double spd) -> coro::task<>
          {
            co_await runtime.scheduler ().schedule ();
            co_await nixb::replay::replay_file (
              runtime.scheduler (), path, queue, stoken, true, spd);
            state.done = true;
            runtime.request_shutdown ();
            co_await queue.shutdown ();
          };

        tasks.push_back (replay_task (
          runtime, state, file, queue, runtime.get_stop_token (), speed));
        coro::sync_wait (coro::when_all (std::move (tasks)));

        runtime.cleanup ();
        runtime.request_shutdown ();
      }
    catch (const std::exception &e)
      {
        runtime.cleanup ();
        fmt::print (stderr, "Error: {}\n", e.what ());
        return 1;
      }

    return 0;
  }

} // anonymous namespace

int
main (int argc, char **argv)
{
  CLI::App app{ "nxb - Nix build UI" };

  std::string play_file;
  double speed = 1.0;

  auto *play_cmd
    = app.add_subcommand ("play", "Replay a recorded nix build log");
  play_cmd
    ->add_option ("file", play_file, "Log file to replay (.tnixlog)")
    ->required ();
  play_cmd->add_option ("-s,--speed", speed, "Playback speed multiplier")
    ->default_val (1.0);

  CLI11_PARSE (app, argc, argv);

  nxb::ansi::init ();

  if (play_cmd->parsed ())
    return cmd_play (play_file, speed);

  // No subcommand - show help
  fmt::print ("{}", app.help ());
  return 0;
}
