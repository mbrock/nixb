#include "NixLogWatcher.hpp"
#include "nix/cmd/installable-flake.hh"
#include "nix/cmd/installables.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/search-path.hh"
#include "nix/flake/flakeref.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/util/ref.hh"
#include "nix/util/types.hh"
#include "src/IdColor.hpp"
#include "src/NixLogPlayer.hpp"

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

#include <algorithm>
#include <cctype>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

  return fmt::format ("{} [{} :: {}] {} {}{}",
                      fmt::styled (prefix, fmt::fg (color)),
                      styled_id (static_cast<uint64_t> (id)),
                      styled_parent (parent), *action, label, suffix);
}

NixLogWatcher::NixLogWatcher (bool quiet, UiMode ui_mode,
                              std::optional<std::string> record_path,
                              std::atomic<bool> *stop_flag,
                              double emit_delay_ms)
    : quiet_ (quiet), emit_delay_ms_ (emit_delay_ms), stop_flag_ (stop_flag)
{
  nix::initLibStore ();
  store_ = nix::openStore ();

  state_ = std::make_unique<NixBuildState> (store_);

  if (ui_mode != UiMode::Off)
    {
      bool force = ui_mode == UiMode::On;
      auto ui = std::make_unique<TerminalUi> (0, force);
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
  if (ui_ && ui_->enabled ())
    {
      ui_->finish ();
    }
}

void
NixLogWatcher::finish ()
{
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

  std::string line = e.format ();
  if (!line.empty () && line.back () == '\n')
    line.pop_back ();

  nlohmann::json json;
  json["id"] = e.id;
  json["level"] = e.level;
  json["type"] = e.type;
  json["text"] = e.text;
  if (e.parent)
    json["parent"] = *e.parent;
  if (!e.fields.empty ())
    json["fields"] = e.fields;
  if (e.store_ref)
    {
      nlohmann::json ref;
      ref["path"] = e.store_ref->path;
      if (e.store_ref->base_url)
        ref["base_url"] = *e.store_ref->base_url;
      json["store_ref"] = std::move (ref);
    }

  line += " json=" + json.dump ();
  // line += "\n";
  emit_log (line);
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
  else
    {
      // If we never saw the start (e.g. interrupted early), try to infer a
      // copy/hashing stop from the last parsed text.
      if (activity_text.empty () && stopped_type == nix::actUnknown)
        {
          type_name = "Unknown";
        }
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
  auto is_blank = [] (const std::string &text)
    {
      return text.empty ()
             || std::all_of (text.begin (), text.end (),
                             [] (unsigned char c) { return std::isspace (c); });
    };

  std::string text = is_blank (block) ? "[empty log line]" : block;

  if (emit_delay_ms_ > 0.0)
    {
      auto delay = std::chrono::duration<double, std::milli> (emit_delay_ms_);
      std::this_thread::sleep_for (delay);
    }

  if (ui_ && ui_->enabled ())
    {
      ui_->redraw (ui_state_);
      ui_->print_log_block (text);
      std::fflush (stdout);
      return;
    }

  fmt::print ("{}\n", text);
  std::fflush (stdout);
}

void
NixLogWatcher::rebuild_ui_state ()
{
  ui_state_.activity_lines.clear ();

  const auto &activities = state_->activities ();
  if (activities.empty ())
    {
      return;
    }

  std::unordered_map<int64_t, std::vector<int64_t>> children;
  std::vector<int64_t> roots;
  std::unordered_set<int64_t> skipped;

  for (const auto &kv : activities)
    {
      const auto &info = kv.second;
      std::optional<int64_t> parent = info.parent;
      if (parent && activities.count (*parent))
        {
          children[*parent].push_back (kv.first);
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

  auto tag_for_type = [&] (ActivityType type)
    {
      switch (type)
        {
        case nix::actSubstitute:
          return std::string{ "[substitute]" };
        case nix::actCopyPath:
          return std::string{ "[copy]" };
        case nix::actFileTransfer:
          return std::string{ "[dl]" };
        case nix::actQueryPathInfo:
          return std::string{ "[query]" };
        case nix::actBuilds:
          return std::string{ "[builds]" };
        case nix::actBuild:
          return std::string{ "[build]" };
        case nix::actBuildWaiting:
          return std::string{ "[queued]" };
        case nix::actFetchTree:
          return std::string{ "[fetch]" };
        default:
          return fmt::format ("[{}]", NixLogParser::activity_type_name (type));
        }
    };

  auto strip_store_hash = [] (std::string_view name)
    {
      auto dash = name.find ('-');
      if (dash != std::string_view::npos && dash + 1 < name.size ())
        {
          return std::string{ name.substr (dash + 1) };
        }
      return std::string{ name };
    };

  auto is_collapse_type = [] (ActivityType type)
    {
      switch (type)
        {
        case nix::actSubstitute:
        case nix::actCopyPath:
        case nix::actQueryPathInfo:
        case nix::actFileTransfer:
          return true;
        default:
          return false;
        }
    };

  std::function<void (int64_t, int)> emit_node = [&] (int64_t id, int depth)
    {
      if (skipped.count (id))
        {
          return;
        }

      const auto *info = state_->get_activity (id);
      if (!info)
        {
          return;
        }

      std::string tag = tag_for_type (info->type);
      bool allow_collapse = is_collapse_type (info->type);
      std::optional<std::string> base_url = info->store_base_url;
      std::string label = state_->format_activity_label (*info);
      if (label.empty ())
        {
          label = "activity";
        }
      const ActivityInfo *display_info = info;

      int64_t cursor = id;
      while (allow_collapse)
        {
          auto it = children.find (cursor);
          if (it == children.end () || it->second.size () != 1)
            break;
          int64_t child_id = it->second.front ();
          const auto *child_info = state_->get_activity (child_id);
          if (!child_info || !is_collapse_type (child_info->type))
            {
              break;
            }
          skipped.insert (child_id);
          cursor = child_id;
          tag = tag_for_type (child_info->type);
          if (label.empty ())
            {
              label = state_->format_activity_label (*child_info);
            }
          if (!base_url && child_info->store_base_url)
            {
              base_url = child_info->store_base_url;
            }
          display_info = child_info;
        }

      std::string indent (static_cast<std::size_t> (depth) * 2, ' ');
      if (depth > 0)
        {
          indent.append ("- ");
        }

      std::string display_label = label;
      if (display_info
          && (display_info->type == nix::actBuild
              || display_info->type == nix::actBuildWaiting))
        {
          if (display_info->store_path)
            {
              display_label
                  = strip_store_hash (display_info->store_path->name ());
            }
          if (!display_info->current_phase.empty ())
            {
              std::string phase = display_info->current_phase;
              constexpr std::string_view suffix = "Phase";
              if (phase.size () > suffix.size ()
                  && phase.compare (phase.size () - suffix.size (),
                                    suffix.size (), suffix)
                         == 0)
                {
                  phase.erase (phase.size () - suffix.size (), suffix.size ());
                }
              std::transform (
                  phase.begin (), phase.end (), phase.begin (),
                  [] (unsigned char c)
                    { return static_cast<char> (std::tolower (c)); });
              tag = fmt::format ("[{}]", phase);
            }
        }

      std::string display
          = fmt::format ("{}{} {}", indent, tag, display_label);
      if (base_url && !base_url->empty ())
        {
          display = fmt::format ("{} {}", display, *base_url);
        }

      auto progress = compute_progress (cursor);
      ui_state_.activity_lines.push_back (
          UiActivityLine{ id, std::move (display), progress });

      auto it = children.find (cursor);
      if (it != children.end ())
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
}

void
NixLogWatcher::refresh_ui ()
{
  if (!ui_ || !ui_->enabled ())
    {
      return;
    }

  rebuild_ui_state ();
  ui_->redraw (ui_state_);
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
