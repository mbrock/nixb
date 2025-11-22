#include "NixLogWatcher.hpp"
#include "nix/cmd/installable-flake.hh"
#include "nix/cmd/installables.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/search-path.hh"
#include "nix/flake/flakeref.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/util/logging.hh"
#include "nix/util/ref.hh"
#include "nix/util/types.hh"
#include "src/NixLogPlayer.hpp"
#include "src/UiStateBuilder.hpp"

#include <chrono>
#include <filesystem>
#include <fmt/color.h>
#include <fmt/core.h>
#include <nix/expr/eval-settings.hh>
#include <nix/fetchers/fetch-settings.hh>
#include <nix/flake/flake.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nlohmann/json.hpp>
#include <thread>

#include <cctype>
#include <iostream>
#include <string>
#include <vector>

namespace nixb
{

namespace
{
// Removed unused formatting helpers - keeping it simple for now
} // namespace

NixLogWatcher::NixLogWatcher (bool quiet, UiMode ui_mode,
                              std::optional<std::string> record_path,
                              std::atomic<bool> *stop_flag,
                              double emit_delay_ms)
    : quiet_ (quiet),
      // Always create a UI session; uses DumbBackend when disabled
      // Off/Auto: force=false, will check TTY
      // On: force=true, will use TerminalBackend even if not TTY
      ui_ (UiSession::create (ui_mode == UiMode::On)),
      emit_delay_ms_ (emit_delay_ms), stop_flag_ (stop_flag)
{
  nix::initLibStore ();
  store_ = nix::openStore ();

  state_ = std::make_unique<NixBuildState> (store_);

  if (record_path)
    {
      recorder_ = std::make_unique<NixLogRecorder> (*record_path);
    }

  // Start render thread for continuous UI updates
  if (ui_.enabled ())
    {
      render_thread_ = std::make_unique<std::thread> (
          [this] () { this->render_loop (); });
    }
}

NixLogWatcher::~NixLogWatcher ()
{
  if (stop_flag_)
    stop_flag_->store (true, std::memory_order_relaxed);

  if (render_thread_ && render_thread_->joinable ())
    {
      render_thread_->join ();
    }
}

std::vector<std::string>
NixLogWatcher::show_derivation (const std::string &installable)
{
  nix::initGC ();
  nix::initLibStore ();
  auto store = nix::openStore ();
  auto evalStore = store;

  bool readOnlyMode = true;
  auto evalSettings = nix::EvalSettings{ readOnlyMode };

  auto fetchSettings = nix::fetchers::Settings{};
  auto lookupPath = nix::LookupPath{};
  auto evalState = nix::make_ref<nix::EvalState> (lookupPath, store,
                                                  fetchSettings, evalSettings);

  auto [flakeRef, fragment] = nix::parseFlakeRefWithFragment (
      fetchSettings, installable, std::filesystem::current_path ().string ());
  nix::flake::LockFlags lockFlags; // set options if needed
  nix::ExtendedOutputsSpec eos
      = nix::ExtendedOutputsSpec::Default (); // default output selection
  nix::Strings defaults
      = { "packages." + nix::settings.thisSystem.get () + ".default",
          "defaultPackage." + nix::settings.thisSystem.get () };
  nix::Strings prefixes
      = { "packages." + nix::settings.thisSystem.get () + ".",
          "legacyPackages." + nix::settings.thisSystem.get () + "." };
  auto inst = nix::make_ref<nix::InstallableFlake> (
      nullptr, evalState, std::move (flakeRef), fragment, eos, defaults,
      prefixes, lockFlags);

  nix::Installables installables = { inst };
  auto drvpaths = nix::Installable::toDerivations (store, installables, true);

  std::vector<std::string> drv_json;
  for (auto &drvpath : drvpaths)
    {
      auto drv = store->readDerivation (drvpath);
      drv_json.push_back (drv.toJSON (store->config).dump (2));
    }

  return drv_json;
}

void
NixLogWatcher::process_input ()
{
  std::string line;
  while (!stop_requested () && std::getline (std::cin, line))
    {
      process_line (line);
      if (stop_requested ())
        {
          break;
        }
    }
  // UiSession handles cleanup in destructor, no explicit finish needed
}

void
NixLogWatcher::finish ()
{
  // In no-UI mode, print final tree snapshot
  if (!ui_.enabled ())
    {
      std::lock_guard<std::mutex> lock (state_mutex_);

      // Debug: show what roots we have
      const auto &roots = state_->activity_roots ();
      std::cerr << "[DEBUG] " << roots.size () << " roots:\n";
      for (int64_t root_id : roots)
        {
          const auto *info = state_->get_activity (root_id);
          if (info)
            {
              std::cerr << "  Root " << root_id << ": "
                        << state_->format_activity_label (*info)
                        << " (type=" << info->type << ")\n";
            }
        }

      rebuild_ui_state ();

      if (!ui_state_.activity_lines.empty ())
        {
          fmt::print (stderr, "\n─── Final Build Status ───\n");
          for (const auto &line : ui_state_.activity_lines)
            {
              fmt::print (stderr, "{}\n", line.label);
            }
          fmt::print (stderr, "\n");
          std::fflush (stderr);
        }
    }

  // UiSession handles cleanup in destructor, no explicit finish needed
}

void
NixLogWatcher::process_line (const std::string &line)
{
  if (recorder_)
    {
      recorder_->record (line);
    }

  auto event_opt = parser_.parse_line (line);

  if (!event_opt)
    {
      if (!quiet_)
        {
          emit_log (line);
        }
      return;
    }

  auto &event = event_opt.value ();

  if (std::holds_alternative<StartEvent> (event))
    {
      handle_start_event (std::get<StartEvent> (event));
    }
  else if (std::holds_alternative<ResultEvent> (event))
    {
      handle_result_event (std::get<ResultEvent> (event));
    }
  else if (std::holds_alternative<StopEvent> (event))
    {
      handle_stop_event (std::get<StopEvent> (event));
    }
  else if (std::holds_alternative<MsgEvent> (event))
    {
      handle_msg_event (std::get<MsgEvent> (event));
    }
}

void
NixLogWatcher::handle_start_event (const StartEvent &e)
{
  {
    std::lock_guard<std::mutex> lock (state_mutex_);
    state_->start_activity (e);

    // Cache input derivations for dependency tracking
    try
      {
        auto activity
            = const_cast<ActivityInfo *> (state_->get_activity (e.id));
        if (activity && activity->derivation)
          {
            for (const auto &[inputDrv, inputNode] :
                 activity->derivation->inputDrvs.map)
              {
                activity->input_drv_paths.push_back (inputDrv);
              }
          }
      }
    catch (...)
      {
        // Ignore errors when accessing input derivations
        // (may happen when paths are not valid in current store context)
      }
  }

  // Simple: just print the event text
  if (!e.text.empty ())
    {
      emit_log (e.text);
    }
}

void
NixLogWatcher::handle_result_event (const ResultEvent &e)
{
  {
    std::lock_guard<std::mutex> lock (state_mutex_);
    state_->update_progress (e);
  }

  if (e.type == nix::resSetExpected)
    {
      return;
    }

  if (e.type == nix::resProgress)
    {
      return;
    }

  emit_log (e.format ());
}

void
NixLogWatcher::handle_stop_event (const StopEvent &e)
{
  std::string activity_text;

  {
    std::lock_guard<std::mutex> lock (state_mutex_);
    if (const auto *info = state_->get_activity (e.id))
      {
        activity_text = info->text;
      }
    state_->stop_activity (e.id);
  }

  // Simple: just print the activity text if we have it
  if (!activity_text.empty ())
    {
      emit_log (activity_text);
    }
}

void
NixLogWatcher::handle_msg_event (const MsgEvent &e)
{
  emit_log (e.format ());

  // 0000000000063 @nix {"action":"msg","level":3,"msg":"these 129 derivations
  // will be built:"} 0000000000099 @nix {"action":"msg","level":3,"msg":"
  // /nix/store/3yj9z7jbsp0q1n5y7d17iqdipqlh12w2-filc0-git.drv"} 0000000000099
  // @nix {"action":"msg","level":3,"msg":"
  // /nix/store/ngr5y3gx77cmd3si4vlja97b0s0vqzzw-filc0-resource-dir.drv"}
  // 0000000000099 @nix {"action":"msg","level":3,"msg":"
  // /nix/store/iqd1zzyb0r8ci63ir3hbdy3wjg220yi3-filc.drv"} 0000000000099 @nix
  // {"action":"msg","level":3,"msg":"
  // /nix/store/h9y612lvx773jqvf6kyx0jsh67wyywzh-libpizlo-git.drv"}
  // 0000000000099 @nix {"action":"msg","level":3,"msg":"
  // /nix/store/i2zf9rbxj6p31nf0igns2r86sn8vfd12-filc-crt-lib.drv"}
  // 0000000000099 @nix {"action":"msg","level":3,"msg":"
  // /nix/store/4jk36rrvar942fni15mkqb039yczxnrv-filc.drv"} 0000000000099 @nix
  // {"action":"msg","level":3,"msg":"
  // /nix/store/wd594wvph2y9v7jvcryay945hxs10d6p-filc-glibc-2.40.drv"}
  // 0000000000099 @nix {"action":"msg","level":3,"msg":"
  // /nix/store/a8vgan1mlhp7wyp1yjsmkk0jky7j44hz-filc.drv"}

  // These are actually valuable messages.
  // At the start of a build, they list the derivations that will be built,
  // in (I believe) topological dependency order.
  // And probably they are all valid present derivations.

  // We should immediately read these derivations.

  // let's use std::regex to match "these \d+ derivations will be built:"
  std::regex re ("these (\\d+) derivations will be built:");
  std::smatch match;
  if (std::regex_search (e.msg, match, re))
    {
      int count = std::stoi (match[1].str ());
      pending_derivation_count_ = count;
    }

  if (pending_derivation_count_.has_value ())
    {
      std::regex re ("^  (/nix/store/.*\\.drv)$");
      std::smatch match;
      if (std::regex_search (e.msg, match, re))
        {
          auto path = store_->parseStorePath (match[1].str ());
          state_->yearn_for_derivation (path);

          pending_derivation_count_ = pending_derivation_count_.value () - 1;
        }
      if (pending_derivation_count_.value () == 0)
        {
          pending_derivation_count_ = std::nullopt;
          state_->yearn ();
        }
    }
}

void
NixLogWatcher::emit_log (const std::string &block)
{
  if (emit_delay_ms_ > 0.0)
    {
      auto delay = std::chrono::duration<double, std::milli> (emit_delay_ms_);
      std::this_thread::sleep_for (delay);
    }

  ui_.hud ().present (ui_state_);
  ui_.log ().println (block);
}

void
NixLogWatcher::rebuild_ui_state ()
{
  UiStateBuilder builder (*state_);
  ui_state_ = builder.build ();
}

void
NixLogWatcher::refresh_ui ()
{
  std::lock_guard<std::mutex> lock (state_mutex_);
  state_->cleanup_finished_activities ();

  rebuild_ui_state ();

  ui_.hud ().present (ui_state_);
}

void
NixLogWatcher::render_loop ()
{
  const auto fps = 60.0;
  const auto frame_interval
      = std::chrono::milliseconds (static_cast<int> (1000.0 / fps));

  while (!stop_requested ())
    {
      const auto frame_start = std::chrono::steady_clock::now ();

      refresh_ui ();

      const auto frame_end = std::chrono::steady_clock::now ();
      const auto elapsed
          = std::chrono::duration_cast<std::chrono::milliseconds> (
              frame_end - frame_start);

      if (elapsed < frame_interval)
        {
          std::this_thread::sleep_for (frame_interval - elapsed);
        }
    }

  ui_.log ().println (fmt::format (
      "exiting at {} UNIX time",
      std::chrono::system_clock::now ().time_since_epoch ().count () / 1000));
}

void
NixLogWatcher::process_playback_file (const std::string &path)
{
  process_playback_file (path, 1.0);
}

void
NixLogWatcher::process_playback_file (const std::string &path, double speedup)
{
  NixLogPlayer player (
      [this] (const std::string &line) { process_line (line); }, stop_flag_);
  player.play (path, speedup);

  // Print final state
  finish ();
}

} // namespace nixb
