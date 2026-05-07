#include <CLI/CLI.hpp>

#include <atomic>
#include <csignal>
#include <iostream>
#include <optional>
#include <vector>

#include "NixLogForwardingLogger.hpp"
#include "NixLogWatcher.hpp"
#include <nix/util/logging.hh>
#include <nix/util/signals.hh>

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

  double emit_delay_ms = 0.0;
  app.add_option (
      "--emit-delay-ms", emit_delay_ms,
      "Artificial delay (ms) between handling log lines (helper for demos)");

  std::string installable;
  auto installable_opt
      = app.add_option ("--show", installable,
                        "Show derivation for an installable flake reference");
  installable_opt->excludes (play_opt);

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

  nixb::NixLogWatcher watcher (quiet, ui_mode, record_path, &stop_flag,
                               emit_delay_ms);

  if (!installable.empty ())
    {
      auto bridge = std::make_unique<nixb::NixLogForwardingLogger> (
          [&watcher] (const std::string &line)
            { watcher.process_log_line (line); },
          /*include_prefix=*/true, &stop_flag);

      std::vector<std::unique_ptr<nix::Logger>> extras;
      if (nix::logger)
        extras.push_back (std::move (nix::logger));
      nix::logger
          = nix::makeTeeLogger (std::move (bridge), std::move (extras));

      try
        {
          // construct a fake log line like "@nix { ... }" with type unknown
          // and id=666
          nlohmann::json json;
          json["action"] = "start";
          json["type"] = nix::actUnknown;
          json["id"] = 666;
          json["parent"] = 0;
          json["text"] = "fake start event";
          std::string log_line
              = "@nix "
                + json.dump (-1, ' ', false,
                             nlohmann::json::error_handler_t::replace);
          watcher.process_log_line (log_line);

          auto drv_json = watcher.show_derivation (installable);
          watcher.finish ();
          for (const auto &doc : drv_json)
            {
              // construct a fake log line like "@nix { ... }" with type
              // unknown and text being the JSON string doc
              nlohmann::json json;
              json["action"] = "result";
              json["type"] = nix::resBuildLogLine;
              json["id"] = 667;
              json["parent"] = 666;
              json["fields"] = { doc };
              std::string log_line
                  = "@nix "
                    + json.dump (-1, ' ', false,
                                 nlohmann::json::error_handler_t::replace);
              watcher.process_log_line (log_line);
            }
          return 0;
        }
      catch (const nix::Interrupted &)
        {
          return 130; // conventional SIGINT exit
        }
    }

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
