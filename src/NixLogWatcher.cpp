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
      drv_json.push_back (drv.toJSON (store->config).dump ());
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
  while (!stop_requested ())
    {
      refresh_ui ();
      const auto fps = 60.0;
      const auto frame_interval
          = std::chrono::milliseconds (static_cast<int> (1000.0 / fps));
      std::this_thread::sleep_for (frame_interval);
    }
}

void
NixLogWatcher::process_playback_file (const std::string &path)
{
  process_playback_file (path, 1.0);
}

void
NixLogWatcher::process_playback_file (const std::string &path, double speedup)
{
  NixLogPlayer player ([this] (const std::string &line)
                         { process_line (line); }, stop_flag_);
  player.play (path, speedup);

  // UiSession handles cleanup in destructor, no explicit finish needed
}

} // namespace nixb
