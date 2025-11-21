#include <CLI/CLI.hpp>

#include <optional>

#include "NixLogWatcher.hpp"

int main(int argc, char **argv) {
  CLI::App app{"nixb - minimal nix internal-json watcher"};
  bool quiet = false;
  app.add_flag("-q,--quiet", quiet,
               "suppress pass-through lines that are not @nix JSON");
  bool no_ui = false;
  bool force_ui = false;
  app.add_flag("--no-ui", no_ui, "Disable interactive progress UI");
  app.add_flag("--ui", force_ui,
               "Force interactive progress UI even if stdout is not a TTY");
  std::string record_file;
  std::string play_file;
  auto record_opt = app.add_option(
      "--record", record_file,
      "Record input with millisecond timestamp prefixes to a file");
  auto play_opt =
      app.add_option("--play", play_file,
                     "Replay a recorded file (honors recorded timing)");
  record_opt->excludes(play_opt);
  CLI11_PARSE(app, argc, argv);

  nixb::NixLogWatcher::UiMode ui_mode = nixb::NixLogWatcher::UiMode::Auto;
  if (no_ui)
    ui_mode = nixb::NixLogWatcher::UiMode::Off;
  else if (force_ui)
    ui_mode = nixb::NixLogWatcher::UiMode::On;

  std::optional<std::string> record_path;
  if (!record_file.empty()) {
    record_path = record_file;
  }

  nixb::NixLogWatcher watcher(quiet, ui_mode, record_path);

  if (!play_file.empty()) {
    watcher.process_playback_file(play_file);
  } else {
    watcher.process_input();
  }

  return 0;
}
