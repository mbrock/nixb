#include "NixLogWatcher.hpp"

#include <fmt/color.h>
#include <fmt/core.h>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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

  return fmt::format (
      "{} [{} :: {}] {} {}{}\n", fmt::styled (prefix, fmt::fg (color)),
      styled_id (static_cast<uint64_t> (id)), styled_parent (parent),
      *action, label, suffix);
}

NixLogWatcher::NixLogWatcher (bool quiet, UiMode ui_mode,
                              std::optional<std::string> record_path)
    : quiet_ (quiet)
{
  nix::initLibStore ();
  store_ = nix::openStore ();
  fmt::print (stderr, "Yay! Nix store opened successfully: {}\n",
              store_->config.getHumanReadableURI ());

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
      record_stream_.open (*record_path, std::ios::out | std::ios::trunc);
      if (record_stream_)
        {
          recording_enabled_ = true;
        }
      else
        {
          fmt::print (stderr, "Failed to open record file: {}\n",
                      *record_path);
        }
    }
}

void
NixLogWatcher::process_input ()
{
  std::string line;
  while (std::getline (std::cin, line))
    {
      process_line (line);
    }
  if (ui_ && ui_->enabled ())
    {
      ui_->finish ();
    }
}

void
NixLogWatcher::process_line (const std::string &line)
{
  record_line (line);

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
  ActivityInfo info{ e.type, e.text };
  info.parent = e.parent;

  if (e.store_ref)
    {
      if (store_)
        {
          if (auto path = store_->maybeParseStorePath (e.store_ref->path))
            {
              info.store_path = path;
              info.label = std::string{ path->name () };
            }
        }
      if (info.label.empty ())
        {
          info.label = e.store_ref->path;
        }
      if (e.store_ref->base_url)
        {
          info.store_base_url = *e.store_ref->base_url;
        }
    }
  activities_[e.id] = std::move (info);
  if (e.type == nix::actBuilds)
    {
      builds_activity_ = e.id;
    }
  if (e.type == nix::actFileTransfer || e.type == nix::actCopyPath)
    {
      note_transfer_start (e.id);
    }
  if (ui_ && ui_->enabled ())
    {
      refresh_ui ();
    }
  auto it = activities_.find (e.id);
  if (it != activities_.end ())
    {
      auto log_line = format_activity_log_line (
          ">>>", fmt::terminal_color::blue, e.id, e.parent, it->second,
          [this] (const ActivityInfo &info)
            {
              std::string label = format_activity_label (info);
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

  if (e.type == nix::resSetExpected)
    {
      update_progress (e);
      if (ui_on)
        {
          refresh_ui ();
        }
      return;
    }

  if (ui_on && e.type == nix::resProgress)
    {
      update_success_tokens (e);
      update_progress (e);
      refresh_ui ();
      return;
    }

  emit_log (e.format ());
  update_success_tokens (e);
  update_progress (e);
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

  if (auto it = activities_.find (e.id); it != activities_.end ())
    {
      type_name = NixLogParser::activity_type_name (it->second.type);
      stopped_type = it->second.type;
      if (it->second.type == nix::actBuild && success_tokens_ > 0)
        {
          build_success = true;
          --success_tokens_;
        }
      if (it->second.type == nix::actBuilds)
        {
          ui_state_.builds_aggregate.reset ();
          builds_activity_.reset ();
        }
      if (it->second.type == nix::actFileTransfer
          || it->second.type == nix::actCopyPath)
        {
          note_transfer_stop (e.id);
        }
      activity_text = it->second.text;
      parent = it->second.parent;
      stopped_info = it->second;
      activities_.erase (it);
    }

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
        std::string label = format_activity_label (info);
        if (label.empty ())
          {
            label = url_basename (info.text);
          }
        return label.empty () ? std::string{ activity_text } : label;
      };

      if (stopped_info)
        {
          if (auto log_line
              = format_activity_log_line ("<<<", fmt::terminal_color::red,
                                          e.id, parent, *stopped_info,
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
          if (auto log_line
              = format_activity_log_line ("<<<", fmt::terminal_color::red,
                                          e.id, parent, fallback_info,
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
NixLogWatcher::update_success_tokens (const ResultEvent &e)
{
  if (e.type != nix::resProgress || !builds_activity_
      || *builds_activity_ != e.id)
    {
      return;
    }
  if (auto done = e.get_int (0))
    {
      if (*done > last_progress_done_)
        {
          success_tokens_ += (*done - last_progress_done_);
          last_progress_done_ = *done;
        }
    }
}

void
NixLogWatcher::update_progress (const ResultEvent &e)
{
  if (!ui_ || !ui_->enabled ())
    {
      return;
    }

  if (e.type == nix::resSetExpected)
    {
      auto type_val = e.get_int (0);
      auto expected_val = e.get_int (1);
      if (!expected_val)
        {
          return;
        }

      auto has_activity = [&] (ActivityType t)
        {
          return std::any_of (activities_.begin (), activities_.end (),
                              [&] (const auto &kv)
                                { return kv.second.type == t; });
        };

      bool touched = false;

      auto set_expected = [&] (std::optional<ActivityProgress> &slot)
        {
          if (*expected_val <= 0)
            {
              slot.reset ();
              touched = true;
              return;
            }
          if (!slot)
            {
              slot = ActivityProgress{};
            }
          slot->expected = *expected_val;
          if (slot->done > slot->expected)
            {
              slot->done = slot->expected;
            }
          slot->running = 0;
          slot->failed = 0;
          touched = true;
        };

      if (type_val)
        {
          ActivityType atype = static_cast<ActivityType> (*type_val);
          switch (atype)
            {
            case nix::actBuilds:
              if (has_activity (nix::actBuilds))
                {
                  set_expected (ui_state_.builds_aggregate);
                }
              break;
            default:
              break;
            }
        }
      if (touched)
        {
          refresh_ui ();
        }
      return;
    }

  if (e.type == nix::resSetPhase)
    {
      if (auto phase = e.get_string (0))
        {
          ui_state_.current_phase = fmt::format ("[phase] {}", *phase);
        }
      return;
    }

  auto it = activities_.find (e.id);

  if (e.type != nix::resProgress || it == activities_.end ())
    {
      return;
    }

  ActivityProgress progress;
  if (auto v = e.get_int (0))
    progress.done = *v;
  if (auto v = e.get_int (1))
    progress.expected = *v;
  if (auto v = e.get_int (2))
    progress.running = *v;
  if (auto v = e.get_int (3))
    progress.failed = *v;

  switch (it->second.type)
    {
    case nix::actBuilds:
      ui_state_.builds_aggregate = progress;
      break;
    case nix::actFileTransfer:
    case nix::actCopyPath:
      if (active_transfers_.count (e.id))
        {
          transfer_progress_[e.id] = progress;
        }
      break;
    default:
      break;
    }
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
NixLogWatcher::record_line (const std::string &line)
{
  if (!recording_enabled_ || !record_stream_)
    {
      return;
    }
  if (!start_time_set_)
    {
      start_time_ = std::chrono::steady_clock::now ();
      start_time_set_ = true;
    }

  auto now = std::chrono::steady_clock::now ();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                        now - start_time_)
                        .count ();
  record_stream_ << fmt::format ("{:013d} {}\n", elapsed_ms, line);
}

void
NixLogWatcher::note_transfer_start (int64_t id)
{
  if (!active_transfers_.insert (id).second)
    {
      return;
    }
  transfer_progress_.emplace (id, ActivityProgress{});
}

void
NixLogWatcher::note_transfer_stop (int64_t id)
{
  auto it = active_transfers_.find (id);
  if (it == active_transfers_.end ())
    {
      return;
    }
  active_transfers_.erase (it);
  transfer_progress_.erase (id);
}

std::string
NixLogWatcher::format_activity_label (const ActivityInfo &info) const
{
  if (info.store_path)
    {
      return std::string{ info.store_path->name () };
    }
  if (!info.label.empty ())
    {
      return info.label;
    }
  if (info.type == nix::actFileTransfer)
    {
      if (!info.text.empty ())
        {
          return url_basename (info.text);
        }
    }
  if (!info.text.empty ())
    {
      return info.text;
    }
  return "activity";
}

void
NixLogWatcher::rebuild_active_builds ()
{
  ui_state_.active_builds.clear ();

  for (const auto &kv : activities_)
    {
      const auto &info = kv.second;
      if (info.type == nix::actBuild || info.type == nix::actBuildWaiting)
        {
          std::string label = format_activity_label (info);
          std::string status
              = info.type == nix::actBuildWaiting ? "queued" : "running";
          ui_state_.active_builds.push_back (SingleBuildState{
              kv.first, std::move (label), std::move (status) });
        }
    }

  std::sort (ui_state_.active_builds.begin (), ui_state_.active_builds.end (),
             [] (const SingleBuildState &a, const SingleBuildState &b)
               { return a.id < b.id; });
}

void
NixLogWatcher::rebuild_active_transfers ()
{
  ui_state_.active_transfers.clear ();

  for (int64_t id : active_transfers_)
    {
      auto info_it = activities_.find (id);
      if (info_it == activities_.end ())
        {
          continue;
        }
      const auto &info = info_it->second;
      auto prog_it = transfer_progress_.find (id);
      ActivityProgress progress = prog_it != transfer_progress_.end ()
                                      ? prog_it->second
                                      : ActivityProgress{};
      std::string label = format_activity_label (info);
      ui_state_.active_transfers.push_back (
          SingleTransferState{ id, std::move (label), progress });
    }

  std::sort (ui_state_.active_transfers.begin (),
             ui_state_.active_transfers.end (),
             [] (const SingleTransferState &a, const SingleTransferState &b)
               { return a.id < b.id; });
}

void
NixLogWatcher::refresh_ui ()
{
  if (!ui_ || !ui_->enabled ())
    {
      return;
    }

  rebuild_active_builds ();
  rebuild_active_transfers ();

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
  std::ifstream in (path);
  if (!in)
    {
      fmt::print (stderr, "Failed to open playback file: {}\n", path);
      return;
    }
  if (speedup < 0.0)
    {
      speedup = 1.0;
    }

  int64_t last_ms = 0;
  bool first = true;
  std::string line;
  while (std::getline (in, line))
    {
      size_t pos = 0;
      while (pos < line.size ()
             && std::isdigit (static_cast<unsigned char> (line[pos])))
        {
          ++pos;
        }

      int64_t timestamp_ms = 0;
      std::string payload = line;

      if (pos > 0)
        {
          try
            {
              timestamp_ms = std::stoll (line.substr (0, pos));
              if (pos < line.size () && line[pos] == ' ')
                {
                  payload = line.substr (pos + 1);
                }
              else
                {
                  payload = line.substr (pos);
                }
            }
          catch (const std::exception &)
            {
              // fall back to treating the whole line as payload
              payload = line;
            }
        }

      int64_t delta = first ? timestamp_ms : (timestamp_ms - last_ms);
      if (delta > 0)
        {
          if (speedup <= 0.0)
            {
              // no delay
            }
          else
            {
              double adjusted = static_cast<double> (delta) / speedup;
              if (adjusted > 0.0)
                {
                  std::this_thread::sleep_for (std::chrono::milliseconds (
                      static_cast<int64_t> (adjusted)));
                }
            }
        }
      last_ms = timestamp_ms;
      first = false;

      process_line (payload);
    }

  if (ui_ && ui_->enabled ())
    {
      ui_->finish ();
    }
}

} // namespace nixb
