#include "NixLogWatcher.hpp"
#include "NixLogPlayer.hpp"
#include "UiStateBuilder.hpp"

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

NixLogWatcher::NixLogWatcher (bool quiet, UiMode ui_mode,
                              std::optional<std::string> record_path,
                              std::atomic<bool> *stop_flag,
                              double emit_delay_ms)
    : quiet_ (quiet), ui_ (UiSession::create (ui_mode == UiMode::On)),
      emit_delay_ms_ (emit_delay_ms), stop_flag_ (stop_flag)
{
  nix::initLibStore ();
  nix::initGC ();

  store_ = nix::openStore ();
  state_ = std::make_unique<NixBuildState> (store_);

  if (record_path)
    recorder_ = std::make_unique<NixLogRecorder> (*record_path);

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

nix::ref<nix::EvalState>
NixLogWatcher::get_eval_state ()
{
  if (!eval_state_)
    {
      // Convert shared_ptr to Nix's ref type
      nix::ref<nix::Store> storeRef (store_);

      bool readOnlyMode = true;
      auto evalSettings = nix::EvalSettings{ readOnlyMode };
      auto fetchSettings = nix::fetchers::Settings{};
      auto lookupPath = nix::LookupPath{};

      eval_state_ = std::make_shared<nix::EvalState> (
          lookupPath, storeRef, fetchSettings, evalSettings);
    }

  return nix::ref<nix::EvalState> (eval_state_);
}

std::vector<std::string>
NixLogWatcher::show_derivation (const std::string &installable)
{
  // Convert shared_ptr to Nix's ref type
  nix::ref<nix::Store> storeRef (store_);

  auto evalState = get_eval_state ();

  auto fetchSettings = nix::fetchers::Settings{};
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
  auto drvpaths
      = nix::Installable::toDerivations (storeRef, installables, true);

  std::vector<std::string> drv_json;
  for (auto &drvpath : drvpaths)
    {
      auto drv = store_->readDerivation (drvpath);
      drv_json.push_back (drv.toJSON ().dump (2));
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
        break;
    }
}

void
NixLogWatcher::finish ()
{
}

void
NixLogWatcher::process_line (const std::string &line)
{
  if (recorder_)
    recorder_->record (line);

  auto event_opt = parser_.parse_line (line);

  if (!event_opt)
    {
      if (!quiet_)
        emit_log (line);
      else
        return;
    }

  auto &event = event_opt.value ();

  if (std::holds_alternative<StartEvent> (event))
    handle_start_event (std::get<StartEvent> (event));
  else if (std::holds_alternative<ResultEvent> (event))
    handle_result_event (std::get<ResultEvent> (event));
  else if (std::holds_alternative<StopEvent> (event))
    handle_stop_event (std::get<StopEvent> (event));
  else if (std::holds_alternative<MsgEvent> (event))
    handle_msg_event (std::get<MsgEvent> (event));
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

  fmt::memory_buffer buf;
  for (const auto &field : e.fields)
    {
      fmt::format_to (
          std::back_inserter (buf), "{:<20} ",
          fmt::styled (field, fmt::fg (fmt::terminal_color::yellow)));
    }

  if (e.store_ref.has_value ())
    {
      fmt::format_to (std::back_inserter (buf), " {}",
                      fmt::styled (e.store_ref->path,
                                   fmt::fg (fmt::terminal_color::blue)));
    }

  // emit_log (fmt::to_string (buf));

  if (!e.text.empty ())
    {
      emit_log (fmt::format (
          "{}", fmt::styled (e.text, fmt::fg (fmt::terminal_color::cyan))));
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
  std::string phase_timing_log;

  {
    std::lock_guard<std::mutex> lock (state_mutex_);
    if (const auto *info = state_->get_activity (e.id))
      {
        activity_text = info->text;

        // For build activities, log phase timing information
        if ((info->type == nix::actBuild
             || info->type == nix::actPostBuildHook)
            && !info->phase_timings.empty ())
          {
            std::string label = state_->format_activity_label (*info);
            std::string phase_timing = info->get_phase_timing_string ();
            if (!phase_timing.empty ())
              {
                phase_timing_log
                    = fmt::format ("{}    {}", label, phase_timing);
              }
          }

        if (info->store_path)
          emit_log (fmt::format (
              "{}", fmt::styled (std::string{ info->store_path->name () },
                                 fmt::fg (fmt::terminal_color::yellow))));
      }
    state_->stop_activity (e.id);
  }

  // Log phase timing information if available
  if (!phase_timing_log.empty ())
    {
      emit_log (fmt::format (
          "{}", fmt::styled (phase_timing_log,
                             fmt::fg (fmt::terminal_color::green))));
    }

  // // Simple: just print the activity text if we have it
  // if (!activity_text.empty ())
  //   {
  //     emit_log (activity_text);
  //   }
}

void
NixLogWatcher::handle_msg_event (const MsgEvent &e)
{
  emit_log (e.format ());

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

  // ui_.hud ().present (ui_state_);
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
  const auto fps = 30.0;
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
  NixLogPlayer player ([this] (const std::string &line)
                         { process_line (line); }, stop_flag_);
  player.play (path, speedup);

  // Print final state
  finish ();
}

} // namespace nixb
