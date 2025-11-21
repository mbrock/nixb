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
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

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
  ui_state_.activity_lines.clear ();

  const auto &activities = state_->activities ();
  if (activities.empty ())
    {
      ui_state_.current_phase.clear ();
      return;
    }

  std::unordered_map<int64_t, std::vector<int64_t>> children;
  std::vector<int64_t> roots;

  for (const auto &kv : activities)
    {
      const auto &info = kv.second;
      if (info.parent && activities.count (*info.parent))
        {
          children[*info.parent].push_back (kv.first);
        }
      else
        {
          roots.push_back (kv.first);
        }
    }

  auto order_for_id = [&] (int64_t id) -> size_t
    {
      if (const auto *info = state_->get_activity (id))
        {
          return info->start_order;
        }
      return std::numeric_limits<size_t>::max ();
    };

  auto order_cmp = [&] (int64_t a, int64_t b)
    {
      size_t order_a = order_for_id (a);
      size_t order_b = order_for_id (b);
      if (order_a == order_b)
        {
          return a < b;
        }
      return order_a < order_b;
    };

  for (auto &kv : children)
    {
      std::sort (kv.second.begin (), kv.second.end (), order_cmp);
    }
  std::sort (roots.begin (), roots.end (), order_cmp);

  std::unordered_map<int64_t, std::optional<ActivityProgress>> progress_cache;
  std::function<std::optional<ActivityProgress> (int64_t)> compute_progress;

  compute_progress = [&] (int64_t id) -> std::optional<ActivityProgress>
    {
      auto cache_it = progress_cache.find (id);
      if (cache_it != progress_cache.end ())
        {
          return cache_it->second;
        }

      const auto *info = state_->get_activity (id);
      if (!info)
        {
          progress_cache[id] = std::nullopt;
          return std::nullopt;
        }

      if (info->has_progress)
        {
          progress_cache[id] = info->progress;
          return info->progress;
        }

      ActivityProgress aggregate{};
      bool have_child = false;
      if (auto it = children.find (id); it != children.end ())
        {
          for (int64_t child_id : it->second)
            {
              if (auto child_prog = compute_progress (child_id))
                {
                  aggregate.done += child_prog->done;
                  aggregate.expected += child_prog->expected;
                  aggregate.running += child_prog->running;
                  aggregate.failed += child_prog->failed;
                  have_child = true;
                }
            }
        }

      if (!have_child)
        {
          progress_cache[id] = std::nullopt;
          return std::nullopt;
        }

      progress_cache[id] = aggregate;
      return aggregate;
    };

  auto format_label = [&] (const ActivityInfo &info, int depth)
    {
      std::string tag;
      switch (info.type)
        {
        case nix::actBuilds:
          tag = "[builds]";
          break;
        case nix::actBuild:
          tag = "[build]";
          break;
        case nix::actBuildWaiting:
          tag = "[queued]";
          break;
        case nix::actFileTransfer:
          tag = "[dl]";
          break;
        case nix::actCopyPath:
          tag = "[copy]";
          break;
        case nix::actSubstitute:
          tag = "[substitute]";
          break;
        case nix::actQueryPathInfo:
          tag = "[query]";
          break;
        case nix::actFetchTree:
          tag = "[fetch]";
          break;
        default:
          tag = fmt::format ("[{}]",
                             NixLogParser::activity_type_name (info.type));
          break;
        }

      std::string label = state_->format_activity_label (info);
      if (label.empty ())
        {
          label = "activity";
        }

      std::string indent (static_cast<std::size_t> (depth) * 2, ' ');
      return fmt::format ("{}{} {}", indent, tag, label);
    };

  std::function<void (int64_t, int)> emit_node
      = [&] (int64_t id, int depth)
  {
    const auto *info = state_->get_activity (id);
    if (!info)
      {
        return;
      }

    auto progress = compute_progress (id);
    ui_state_.activity_lines.push_back (UiActivityLine{
        id, format_label (*info, depth), progress });

    if (auto it = children.find (id); it != children.end ())
      {
        for (int64_t child_id : it->second)
          {
            emit_node (child_id, depth + 1);
          }
      }
  };

  for (int64_t root_id : roots)
    {
      emit_node (root_id, 0);
    }

  // Copy builds progress
  const auto &bp = state_->builds_progress ();
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
  int desired_lines = static_cast<int> (ui_state_.activity_lines.size ())
                      + (ui_state_.current_phase.empty () ? 0 : 1);
  desired_lines = std::max (1, desired_lines);
  if (max_footer > 0)
    {
      desired_lines = std::min (desired_lines, max_footer);
    }

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
