#include <CLI/CLI.hpp>

#include <atomic>
#include <csignal>
#include <optional>

#include "NixLogWatcher.hpp"

namespace
{
std::atomic<bool> *g_stop_flag = nullptr;

void
handle_signal (int)
{
  if (g_stop_flag)
    {
      g_stop_flag->store (true, std::memory_order_relaxed);
    }
}

void
install_signal_handlers (std::atomic<bool> &stop_flag)
{
  g_stop_flag = &stop_flag;
  struct sigaction sa;
  sa.sa_handler = handle_signal;
  sigemptyset (&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction (SIGINT, &sa, nullptr);
}
} // namespace

int
main (int argc, char **argv)
{
  CLI::App app{ "nixb - minimal nix internal-json watcher" };
  bool quiet = false;
  app.add_flag ("-q,--quiet", quiet,
                "suppress pass-through lines that are not @nix JSON");
  bool no_ui = false;
  bool force_ui = false;
  app.add_flag ("--no-ui", no_ui, "Disable interactive progress UI");
  app.add_flag ("--ui", force_ui,
                "Force interactive progress UI even if stdout is not a TTY");
  std::string record_file;
  std::string play_file;
  auto record_opt = app.add_option (
      "--record", record_file,
      "Record input with millisecond timestamp prefixes to a file");
  auto play_opt = app.add_option (
      "--play", play_file, "Replay a recorded file (honors recorded timing)");
  record_opt->excludes (play_opt);
  double play_speed = 1.0;
  app.add_option ("--play-speed", play_speed,
                  "Playback speed multiplier (0 = no delays, 2 = 2x faster)");
  CLI11_PARSE (app, argc, argv);

  nixb::NixLogWatcher::UiMode ui_mode = nixb::NixLogWatcher::UiMode::Auto;
  if (no_ui)
    ui_mode = nixb::NixLogWatcher::UiMode::Off;
  else if (force_ui)
    ui_mode = nixb::NixLogWatcher::UiMode::On;

  std::optional<std::string> record_path;
  if (!record_file.empty ())
    {
      record_path = record_file;
    }

  std::atomic<bool> stop_flag{ false };
  install_signal_handlers (stop_flag);

  nixb::NixLogWatcher watcher (quiet, ui_mode, record_path, &stop_flag);

  if (!play_file.empty ())
    {
      watcher.process_playback_file (play_file, play_speed);
    }
  else
    {
      watcher.process_input ();
    }

  return 0;
}
