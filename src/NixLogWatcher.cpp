#include "NixLogWatcher.hpp"
#include "src/IdColor.hpp"
#include "src/NixLogPlayer.hpp"

#include <fmt/color.h>
#include <fmt/core.h>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>

#include <algorithm>
#include <iostream>
#include <string>

namespace nixb
{

namespace
{
std::string
styled_id (uint64_t id)
{
  return fmt::format ("{}",
                      fmt::styled (hashed_id_token (id), style_for_id (id)));
}

std::string
styled_parent (std::optional<int64_t> parent)
{
  if (!parent)
    {
      return fmt::format (
          "{}", fmt::styled ("-", fmt::fg (fmt::terminal_color::white)));
    }
  return styled_id (static_cast<uint64_t> (*parent));
}

std::string
url_basename (std::string_view text)
{
  auto pos = text.rfind ('/');
  if (pos == std::string_view::npos || pos + 1 >= text.size ())
    {
      return std::string{ text };
    }
  return std::string{ text.substr (pos + 1) };
}

std::optional<std::string_view>
activity_action (ActivityType type)
{
  switch (type)
    {
    case nix::actCopyPath:
      return "COPY";
    case nix::actFileTransfer:
      return "WGET";
    case nix::actSubstitute:
      return "SUBS";
    case nix::actQueryPathInfo:
      return "INFO";
    default:
      return std::nullopt;
    }
}

} // namespace

std::optional<std::string>
NixLogWatcher::format_activity_log_line (
    std::string_view prefix, fmt::terminal_color color, int64_t id,
    std::optional<int64_t> parent, const ActivityInfo &info,
    const std::function<std::string (const ActivityInfo &)> &label_fn) const
{
  auto action = activity_action (info.type);
  if (!action)
    {
      return std::nullopt;
    }

  std::string label = label_fn (info);
  std::string suffix;
  if ((info.type == nix::actSubstitute || info.type == nix::actQueryPathInfo)
      && info.store_base_url && !info.store_base_url->empty ())
    {
      suffix = fmt::format (" @ {}", *info.store_base_url);
    }

  return fmt::format ("{} [{} :: {}] {} {}{}\n",
                      fmt::styled (prefix, fmt::fg (color)),
                      styled_id (static_cast<uint64_t> (id)),
                      styled_parent (parent), *action, label, suffix);
}

NixLogWatcher::NixLogWatcher (bool quiet, UiMode ui_mode,
                              std::optional<std::string> record_path,
                              std::atomic<bool> *stop_flag)
    : quiet_ (quiet), stop_flag_ (stop_flag)
{
  nix::initLibStore ();
  store_ = nix::openStore ();
  fmt::print (stderr, "Yay! Nix store opened successfully: {}\n",
              store_->config.getHumanReadableURI ());

  state_ = std::make_unique<NixBuildState> (store_);

  if (ui_mode != UiMode::Off)
    {
      bool force = ui_mode == UiMode::On;
      auto ui = std::make_unique<TerminalUi> (3, force);
      if (ui->enabled ())
        {
          ui_ = std::move (ui);
        }
    }

  if (record_path)
    {
      recorder_ = std::make_unique<NixLogRecorder> (*record_path);
    }
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
  if (ui_ && ui_->enabled ())
    {
      ui_->finish ();
    }
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
          emit_log (fmt::format ("{}\n", line));
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
  state_->start_activity (e);

  if (ui_ && ui_->enabled ())
    {
      refresh_ui ();
    }

  const auto *info = state_->get_activity (e.id);
  if (info)
    {
      auto log_line = format_activity_log_line (
          ">>>", fmt::terminal_color::blue, e.id, e.parent, *info,
          [this] (const ActivityInfo &info)
            {
              std::string label = state_->format_activity_label (info);
              if (label.empty ())
                {
                  label = url_basename (info.text);
                }
              return label;
            });
      if (log_line)
        {
          emit_log (*log_line);
          return;
        }
    }

  emit_log (e.format ());
}

void
NixLogWatcher::handle_result_event (const ResultEvent &e)
{
  bool ui_on = ui_ && ui_->enabled ();

  state_->update_progress (e);

  if (e.type == nix::resSetExpected)
    {
      if (ui_on)
        {
          refresh_ui ();
        }
      return;
    }

  if (ui_on && e.type == nix::resProgress)
    {
      refresh_ui ();
      return;
    }

  emit_log (e.format ());
  if (ui_on)
    {
      refresh_ui ();
    }
}

void
NixLogWatcher::handle_stop_event (const StopEvent &e)
{
  std::string_view type_name = "Unknown";
  std::string activity_text;
  bool build_success = false;
  std::optional<int64_t> parent;
  ActivityType stopped_type = nix::actUnknown;

  std::optional<ActivityInfo> stopped_info;

  if (const auto *info = state_->get_activity (e.id))
    {
      type_name = NixLogParser::activity_type_name (info->type);
      stopped_type = info->type;
      if (info->type == nix::actBuild && state_->success_tokens () > 0)
        {
          build_success = true;
          state_->decrement_success_tokens ();
        }
      activity_text = info->text;
      parent = info->parent;
      stopped_info = *info;
    }

  state_->stop_activity (e.id);

  if (ui_ && ui_->enabled ())
    {
      refresh_ui ();
    }

  if (stopped_type == nix::actCopyPath || stopped_type == nix::actFileTransfer
      || stopped_type == nix::actQueryPathInfo
      || stopped_type == nix::actSubstitute)
    {
      auto label_fn = [this, &activity_text] (const ActivityInfo &info)
        {
          std::string label = state_->format_activity_label (info);
          if (label.empty ())
            {
              label = url_basename (info.text);
            }
          return label.empty () ? std::string{ activity_text } : label;
        };

      if (stopped_info)
        {
          if (auto log_line = format_activity_log_line (
                  "<<<", fmt::terminal_color::red, e.id, parent, *stopped_info,
                  label_fn))
            {
              emit_log (*log_line);
              return;
            }
        }
      else
        {
          ActivityInfo fallback_info{ stopped_type, std::string{} };
          fallback_info.parent = parent;
          if (auto log_line = format_activity_log_line (
                  "<<<", fmt::terminal_color::red, e.id, parent, fallback_info,
                  label_fn))
            {
              emit_log (*log_line);
              return;
            }
        }
    }

  std::optional<uint64_t> parent_id;
  if (parent)
    {
      parent_id = static_cast<uint64_t> (*parent);
    }

  emit_log (e.format (type_name, activity_text, build_success, parent_id));
}

void
NixLogWatcher::handle_msg_event (const MsgEvent &e)
{
  emit_log (e.format ());
}

void
NixLogWatcher::emit_log (const std::string &block)
{
  if (ui_ && ui_->enabled ())
    {
      ui_->print_log_block (block);
      ui_->redraw (ui_state_);
      return;
    }
  fmt::print ("{}", block);
}

void
NixLogWatcher::rebuild_ui_state ()
{
  // Rebuild active builds
  ui_state_.active_builds.clear ();
  for (const auto &kv : state_->activities ())
    {
      const auto &info = kv.second;
      if (info.type == nix::actBuild || info.type == nix::actBuildWaiting)
        {
          std::string label = state_->format_activity_label (info);
          std::string status
              = info.type == nix::actBuildWaiting ? "queued" : "running";
          ui_state_.active_builds.push_back (SingleBuildState{
              kv.first, std::move (label), std::move (status) });
        }
    }
  std::sort (ui_state_.active_builds.begin (), ui_state_.active_builds.end (),
             [] (const SingleBuildState &a, const SingleBuildState &b)
               { return a.id < b.id; });

  // Rebuild active transfers
  ui_state_.active_transfers.clear ();
  for (int64_t id : state_->active_transfers ())
    {
      const auto *info = state_->get_activity (id);
      if (!info)
        {
          continue;
        }
      auto prog_it = state_->transfer_progress ().find (id);
      ActivityProgress progress
          = prog_it != state_->transfer_progress ().end ()
                ? prog_it->second
                : ActivityProgress{};
      std::string label = state_->format_activity_label (*info);
      ui_state_.active_transfers.push_back (
          SingleTransferState{ id, std::move (label), progress });
    }
  std::sort (ui_state_.active_transfers.begin (),
             ui_state_.active_transfers.end (),
             [] (const SingleTransferState &a, const SingleTransferState &b)
               { return a.id < b.id; });

  // Copy builds progress
  const auto &bp = state_->builds_progress ();
  ui_state_.builds_aggregate = bp.aggregate;
  if (!bp.current_phase.empty ())
    {
      ui_state_.current_phase = fmt::format ("[phase] {}", bp.current_phase);
    }
  else
    {
      ui_state_.current_phase.clear ();
    }
}

void
NixLogWatcher::refresh_ui ()
{
  if (!ui_ || !ui_->enabled ())
    {
      return;
    }

  rebuild_ui_state ();

  int max_footer = ui_->max_status_lines ();
  if (max_footer <= 0)
    {
      max_footer = 1;
    }

  int reserve_header = 1;
  int reserve_phase = ui_state_.current_phase.empty () ? 0 : 1;
  int transfers_lines
      = ui_state_.active_transfers.empty ()
            ? 1
            : static_cast<int> (ui_state_.active_transfers.size ());

  int space_for_builds = std::max (
      0, max_footer - (reserve_header + reserve_phase + transfers_lines));
  if (space_for_builds < static_cast<int> (ui_state_.active_builds.size ()))
    {
      ui_state_.active_builds.resize (space_for_builds);
    }

  int desired_lines = reserve_header + reserve_phase + transfers_lines
                      + static_cast<int> (ui_state_.active_builds.size ());
  desired_lines = std::max (2, desired_lines);

  ui_->update_status_height (desired_lines, ui_state_);
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

  if (ui_ && ui_->enabled ())
    {
      ui_->finish ();
    }
}

} // namespace nixb
