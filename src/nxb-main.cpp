#include <exception>

// #include <boost/stacktrace.hpp>
#include <boost/stacktrace/frame.hpp>
#include <boost/stacktrace/stacktrace.hpp>

#include <sys/stat.h>

#include <fmt/base.h>
#include <fmt/color.h>
#include <fmt/core.h>

#include <CLI/CLI.hpp>

#include <nix/util/error.hh>

#include <nxt/ansi.hpp>
#include <nxt/app.hpp>
#include <nxt/async.hpp>
#include <nxt/tui.hpp>

#include "log-replay.hpp"
#include "nix-api.hpp"
#include "nix-log-adapter.hpp"

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

  runtime.println("Starting evaluation with TrivialStore...");

  try {
    // Create TrivialStore wired to the UI runtime
    nxb::NixContext ctx(runtime);
    runtime.println(
        fmt::format("Store: {}", ctx.store()->config.getHumanReadableURI()));
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

struct BuildState {
  std::string installable;
  bool done = false;
  std::string error_msg;
  std::vector<std::string> build_results;
};

auto build_build_ui(const BuildState &state) {
  auto results_text =
      state.build_results.empty()
          ? text("")
          : text(fmt::format("{} results", state.build_results.size()));

  return column(
      hrule(),
      text(fmt::format("Building: {}", state.installable),
           fg(nxb::Rgba8::cyan())),
      text(state.done ? (state.error_msg.empty() ? "Done" : "Error")
                      : "Building...",
           fg(state.done && state.error_msg.empty() ? nxb::Rgba8::green()
                                                    : nxb::Rgba8::yellow())),
      results_text);
}

nxb::task<> run_build(nxb::ui::UIRuntime &runtime, BuildState &state,
                      nxb::queue<NixLogEvent> &events) {
  auto adapter = std::make_unique<nixb::coro_adapter::NixLogAdapter>(events);
  nix::logger = std::move(adapter);
  nix::verbosity = nix::lvlChatty;

  runtime.println(
      fmt::format("Building {} with TrivialStore...", state.installable));

  try {
    // Create TrivialStore wired to the UI runtime for output
    nxb::NixContext ctx(runtime);
    runtime.println(
        fmt::format("Store: {}", ctx.store()->config.getHumanReadableURI()));

    auto drv_paths = nxb::resolve_installable(ctx, state.installable);
    runtime.println(fmt::format("Resolved {} derivation(s)", drv_paths.size()));

    if (drv_paths.empty()) {
      state.error_msg = "No derivations found";
    } else {
      std::vector<nix::DerivedPath> to_build;
      for (const auto &drv_path : drv_paths) {
        runtime.println(
            fmt::format("  drv: {}", ctx.store()->printStorePath(drv_path)));
        to_build.push_back(nix::DerivedPath::Built{
            .drvPath = nix::makeConstantStorePathRef(drv_path),
            .outputs = nix::OutputsSpec::All{},
        });
      }

      runtime.println("\nCalling buildPathsWithResults...");
      auto results =
          ctx.store()->buildPathsWithResults(to_build, nix::bmNormal);

      runtime.println(fmt::format("\nBuild results: {}", results.size()));
      for (const auto &result : results) {
        auto status = result.tryGetSuccess() ? "success" : "failed";
        auto path_str = result.path.to_string(*ctx.store());
        runtime.println(fmt::format("  path: {} status: {}", path_str, status));
        state.build_results.push_back(fmt::format("{}: {}", path_str, status));

        if (auto *failure = result.tryGetFailure()) {
          runtime.println(fmt::format("    error: {}", failure->errorMsg));
        }
      }
    }
  } catch (std::exception &e) {
    state.error_msg = e.what();
    runtime.println(fmt::format("Error: {}", e.what()));
  }

  state.done = true;
  runtime.signal_damage();
  runtime.request_shutdown();
  co_await events.shutdown();
}

nxb::task<> update_build(nxb::ui::UIRuntime &runtime, BuildState &state) {
  nxb::queue<NixLogEvent> events;
  co_await runtime.run(consume_events(runtime, events),
                       run_build(runtime, state, events));
}

int cmd_build(const std::string &installable) {
  nxb::ansi::init();
  return nxb::ui::run(BuildState{.installable = installable}, build_build_ui,
                      update_build);
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

  std::string build_installable;
  auto *build_cmd = app.add_subcommand(
      "build", "Build a flake installable (using TrivialStore)");
  build_cmd
      ->add_option("installable", build_installable,
                   "Flake installable (e.g. .#default)")
      ->required();

  CLI11_PARSE(app, argc, argv);

  if (play_cmd->parsed())
    return cmd_play(play_file, speed);

  if (derive_cmd->parsed())
    return cmd_derive(installable);

  if (build_cmd->parsed())
    return cmd_build(build_installable);

  // No subcommand - show help
  fmt::print("{}", app.help());
  return 0;
}
