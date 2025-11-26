#include "log-replay.hpp"
#include "nix-api.hpp"
#include "nix-log-adapter.hpp"

#include <exception>
#include <nxt/ansi.hpp>
#include <nxt/app.hpp>
#include <nxt/async.hpp>
#include <nxt/tui.hpp>

#include <CLI/CLI.hpp>
#include <fmt/core.h>

#include <boost/stacktrace.hpp>

void my_terminate_handler() {
  try {
    std::cerr << boost::stacktrace::stacktrace();
  } catch (...) {
  }
  std::abort();
}

using namespace nixb;
using namespace nxb::tui;

namespace {
using nix_event::NixLogEvent;

struct PlaybackState {
  std::string file;
  double speed = 1.0;
  bool done = false;
};

auto build_play_ui(const PlaybackState &state) {
  return column(
      hrule(),
      text(fmt::format("Playing: {}", state.file), fg(nxb::Rgba8::cyan())),
      text(state.done ? "Done" : "Playing...", fg(nxb::Rgba8::green())),
      progress_bar(0.5 * mp_units::one));
}

struct EventHandler {
  nxb::ui::UIRuntime &runtime;

  // Top-level event handlers
  void operator()(const nix_event::LogLine &ev) { runtime.println(ev.text); }

  void operator()(const nix_event::ActivityStarted &ev) {
    std::visit(*this, ev.kind);
  }

  void operator()(const nix_event::ActivityProgress &) {
    // Ignore progress updates for now
  }

  void operator()(const nix_event::ActivityPhase &) {
    // Ignore phase updates for now
  }

  void operator()(const nix_event::ActivityFinished &) {
    // Ignore finished events for now
  }

  void operator()(const nix_event::Error &ev) {
    runtime.println(fmt::format("error: {}", ev.info.msg.str()));
  }

  void operator()(const nix_event::activity::Build &kind) {
    runtime.println(fmt::format("building {}", kind.drv_path));
  }

  void operator()(const nix_event::activity::Download &kind) {
    runtime.println(fmt::format("downloading {}", kind.url));
  }

  void operator()(const nix_event::activity::Copy &kind) {
    runtime.println(fmt::format("copying {}", kind.path));
  }

  void operator()(const nix_event::activity::Realise &kind) {
    runtime.println(fmt::format("realising {}", kind.path));
  }

  void operator()(const nix_event::activity::Substitute &kind) {
    runtime.println(fmt::format("substituting {}", kind.path));
  }

  void operator()(const nix_event::activity::QueryPathInfo &kind) {
    runtime.println(fmt::format("querying path info for {}", kind.path));
  }

  void operator()(const nix_event::activity::PostBuildHook &kind) {
    runtime.println(fmt::format("post-build hook for {}", kind.drv_path));
  }

  void operator()(const nix_event::activity::BuildWaiting &) {
    runtime.println("build waiting");
  }

  void operator()(const nix_event::activity::Unknown &kind) {
    runtime.println(fmt::format("unknown activity: {}", kind.text));
  }
};

nxb::task<> consume_events(nxb::ui::UIRuntime &runtime,
                           nxb::queue<nix_event::NixLogEvent> &events) {
  while (!runtime.shutdown_requested()) {
    auto event = co_await events.pop();
    if (!event)
      break;

    std::visit(EventHandler{runtime}, *event);
    runtime.signal_damage();
  }
}

nxb::task<> run_replay(nxb::ui::UIRuntime &runtime, PlaybackState &state,
                       nxb::queue<NixLogEvent> &events) {
  co_await nixb::replay::replay_file(runtime, state.file, events, true,
                                     state.speed);
  state.done = true;
  runtime.signal_damage();
  co_await events.shutdown();
}

nxb::task<> update_play(nxb::ui::UIRuntime &runtime, PlaybackState &state) {
  nxb::queue<NixLogEvent> events;

  co_await runtime.run(consume_events(runtime, events),
                       run_replay(runtime, state, events));
}

int cmd_play(const std::string &file, double speed) {
  nxb::ansi::init();
  return nxb::ui::run(PlaybackState{.file = file, .speed = speed},
                      build_play_ui, update_play);
}

int cmd_derive(const std::string &installable) {
  nxb::NixContext ctx;

  try {
    auto drv_paths = nxb::resolve_installable(ctx, installable);

    for (const auto &path : drv_paths) {
      fmt::print("{}\n", ctx.store()->printStorePath(path));
      auto info = read_derivation_info(ctx, path);
      if (info) {
        fmt::print("name: {}\n", info->name);
        fmt::print("system: {}\n", info->system);
        fmt::print("input_drvs: {}\n", info->input_drvs.size());
        for (const auto &drv : info->input_drvs) {
          fmt::print("  {}\n", ctx.store()->printStorePath(drv));
          auto drv_info = read_derivation_info(ctx, drv);
          if (drv_info) {
            for (const auto &dep : drv_info->input_drvs) {
              fmt::print("    {}\n", dep.name());
            }
          }
        }
        fmt::print("output_paths: {}\n", info->output_paths.size());
        for (const auto &path : info->output_paths)
          fmt::print("  {}\n", ctx.store()->printStorePath(path));
        // fmt::print("env:\n");
        // for (const auto &[key, value] : info->env)
        //   if (!value.empty())
        //     fmt::print("  {}={}\n", key, value);
      }
    }

  } catch (std::exception &e) {
    boost::stacktrace::stacktrace trace =
        boost::stacktrace::stacktrace::from_current_exception(); // <---
    std::cerr << "Caught exception: " << e.what() << ", trace:\n" << trace;
  }

  return 0;
}

} // anonymous namespace

int main(int argc, char **argv) {
  std::set_terminate(&my_terminate_handler);

  CLI::App app{"nxb - Nix build UI"};

  std::string play_file;
  double speed = 1.0;

  auto *play_cmd =
      app.add_subcommand("play", "Replay a recorded nix build log");
  play_cmd->add_option("file", play_file, "Log file to replay (.tnixlog)")
      ->required();
  play_cmd->add_option("-s,--speed", speed, "Playback speed multiplier")
      ->default_val(1.0);

  std::string installable;
  auto *derive_cmd = app.add_subcommand(
      "derive", "Resolve a flake installable to derivation paths");
  derive_cmd
      ->add_option("installable", installable,
                   "Flake installable (e.g. .#default)")
      ->required();

  CLI11_PARSE(app, argc, argv);

  if (play_cmd->parsed())
    return cmd_play(play_file, speed);

  if (derive_cmd->parsed())
    return cmd_derive(installable);

  // No subcommand - show help
  fmt::print("{}", app.help());
  return 0;
}
