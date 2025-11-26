#include "log-replay.hpp"
#include "nix-api.hpp"
#include "nix-log-adapter.hpp"

#include <exception>
#include <fmt/base.h>
#include <fmt/color.h>
#include <future>
#include <nix/util/error.hh>
#include <nxt/ansi.hpp>
#include <nxt/app.hpp>
#include <nxt/async.hpp>
#include <nxt/tui.hpp>

#include <CLI/CLI.hpp>
#include <fmt/core.h>

#include <boost/stacktrace.hpp>
#include <sys/stat.h>

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
  void operator()(const nix_event::LogLine &ev) {
    auto x = (1.0 - static_cast<float>(ev.level) /
                        static_cast<float>(nix::Verbosity::lvlVomit) * 200.0);
    fmt::println(
        "{} {}",
        fmt::styled(static_cast<int>(ev.level), fmt::fg(fmt::color::gray)),
        fmt::styled(ev.text, fmt::fg(fmt::rgb(x, x, x))));
  }

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

struct DeriveState {
  std::string installable;
  bool done = false;
  std::string result_path;
  std::string error_msg;
};

auto build_derive_ui(const DeriveState &state) {
  return column(
      hrule(),
      text(fmt::format("Deriving: {}", state.installable),
           fg(nxb::Rgba8::cyan())),
      text(state.done ? (state.error_msg.empty() ? "Done" : "Error")
                      : "Evaluating...",
           fg(state.done && state.error_msg.empty() ? nxb::Rgba8::green()
                                                    : nxb::Rgba8::yellow())));
}

nxb::task<> run_derive(nxb::ui::UIRuntime &runtime, DeriveState &state,
                       nxb::queue<NixLogEvent> &events) {
  // Set up our logger to capture Nix logs
  auto adapter = std::make_unique<nixb::coro_adapter::NixLogAdapter>(events);
  nix::logger = std::move(adapter);

  // // Enable verbose logging to get evaluation output
  nix::verbosity = nix::lvlDebug;
  //  nix::loggerSettings.showTrace = true;

  runtime.println("Starting evaluation...");

  try {
    nxb::NixContext ctx;
    runtime.println("Context created, resolving installable...");
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
  } catch (std::exception &e) {
    state.error_msg = e.what();
    runtime.println(fmt::format("error: {}", e.what()));
  }

  state.done = true;
  runtime.signal_damage();
  co_await events.shutdown();
}

nxb::task<> update_derive(nxb::ui::UIRuntime &runtime, DeriveState &state) {
  nxb::queue<NixLogEvent> events;

  co_await runtime.run(consume_events(runtime, events),
                       run_derive(runtime, state, events));
}

int cmd_derive(const std::string &installable) {
  nxb::ansi::init();
  return nxb::ui::run(DeriveState{.installable = installable}, build_derive_ui,
                      update_derive);
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
