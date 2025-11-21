#include "UiStateBuilder.hpp"
#include "NixLogParser.hpp"
#include "nix/util/logging.hh"

#include <algorithm>
#include <cctype>
#include <fmt/core.h>
#include <limits>

namespace nixb
{

UiStateBuilder::UiStateBuilder (const NixBuildState &state) : state_ (state) {}

UiState
UiStateBuilder::build ()
{
  result_.activity_lines.clear ();

  const auto &activities = state_.activities ();
  if (activities.empty ())
    {
      return result_;
    }

  build_activity_tree ();

  for (int64_t root_id : roots_)
    {
      emit_tree_node (root_id, 0);
    }

  sort_activity_lines ();

  return result_;
}

void
UiStateBuilder::build_activity_tree ()
{
  const auto &activities = state_.activities ();

  // Build parent-child relationships
  for (const auto &kv : activities)
    {
      const auto &info = kv.second;
      std::optional<int64_t> parent = info.parent;
      if (parent && activities.count (*parent))
        {
          children_[*parent].push_back (kv.first);
        }
      else
        {
          roots_.push_back (kv.first);
        }
    }

  // Sort children and roots by start order
  auto order_cmp = [this] (int64_t a, int64_t b)
    {
      size_t order_a = order_for_id (a);
      size_t order_b = order_for_id (b);
      if (order_a == order_b)
        {
          return a < b;
        }
      return order_a < order_b;
    };

  for (auto &kv : children_)
    {
      std::sort (kv.second.begin (), kv.second.end (), order_cmp);
    }
  std::sort (roots_.begin (), roots_.end (), order_cmp);
}

std::optional<ActivityProgress>
UiStateBuilder::compute_progress (int64_t id)
{
  const auto *info = state_.get_activity (id);
  if (!info)
    {
      return std::nullopt;
    }

  if (info->has_progress)
    {
      return info->progress;
    }

  return std::nullopt;
}

bool
UiStateBuilder::should_hide_activity (int64_t id) const
{
  const auto *info = state_.get_activity (id);
  if (!info)
    {
      return true; // Hide activities we can't find
    }

  if (info->type == nix::actRealise || info->type == nix::actBuilds
      || info->type == nix::actSubstitute || info->type == nix::actFileTransfer
      || info->type == nix::actUnknown)
    {
      return true;
    }

  return false;
}

std::string
UiStateBuilder::format_display_label (
    const ActivityInfo &info, int visible_depth, const std::string &tag,
    const std::string &label, const std::optional<std::string> &base_url)
{

  std::string display_label = label;

  if (info.type == nix::actBuild || info.type == nix::actBuildWaiting)
    {
      if (info.derivation_path)
        display_label = info.derivation_path->name ();
      else if (info.store_path)
        display_label = info.store_path->name ();
    }

  std::string name_part;
  if (tag.empty ())
    {
      name_part = display_label;
    }
  else
    {
      name_part = fmt::format ("{} {}", tag, display_label);
    }

  // Return just the name part - URL will be rendered separately
  return name_part;
}

void
UiStateBuilder::emit_tree_node (int64_t id, int visible_depth)
{
  const auto *info = state_.get_activity (id);
  if (!info)
    {
      return;
    }

  bool show_this_line = !should_hide_activity (id);
  int next_visible_depth = visible_depth;

  if (show_this_line)
    {
      std::string tag = tag_for_type (info->type);
      std::string label = state_.format_activity_label (*info);

      if (info->type == nix::actBuild || info->type == nix::actBuildWaiting)
        {
          if (!info->current_phase.empty ())
            {
              std::string phase = info->current_phase;
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

      std::string display = format_display_label (*info, visible_depth, tag,
                                                  label, info->store_base_url);

      // Extract URL for separate styling
      std::optional<std::string> url_part;
      if (info->store_base_url && !info->store_base_url->empty ())
        {
          std::string_view url = *info->store_base_url;
          if (url.starts_with ("https://"))
            {
              url = url.substr (8);
            }
          else if (url.starts_with ("http://"))
            {
              url = url.substr (7);
            }
          url_part = std::string (url);
        }

      auto progress = compute_progress (id);

      // Calculate fade factor for finished activities
      double fade_factor = 0.0;
      if (info->is_finished && info->finish_time)
        {
          auto now = std::chrono::steady_clock::now ();
          auto elapsed = now - *info->finish_time;
          auto elapsed_ms
              = std::chrono::duration_cast<std::chrono::milliseconds> (elapsed)
                    .count ();
          // Fade out over 1000ms (matches cleanup timeout)
          fade_factor = std::min (1.0, elapsed_ms / 2000.0);
        }

      result_.activity_lines.push_back (
          UiActivityLine{ id, std::move (display), url_part, progress,
                          info->is_finished, fade_factor });

      next_visible_depth = visible_depth + 1;
    }

  auto it = children_.find (id);
  if (it != children_.end ())
    {
      for (int64_t child_id : it->second)
        {
          emit_tree_node (child_id, next_visible_depth);
        }
    }
}

void
UiStateBuilder::sort_activity_lines ()
{
  // std::stable_sort (result_.activity_lines.begin (),
  //                   result_.activity_lines.end (),
  //                   [&] (const UiActivityLine &a, const UiActivityLine &b)
  //                     {
  //                       const auto *info_a = state_.get_activity (a.id);
  //                       const auto *info_b = state_.get_activity (b.id);
  //                       if (!info_a || !info_b)
  //                         return false;

  //                       bool is_dl_a = (info_a->type == nix::actCopyPath);
  //                       bool is_dl_b = (info_b->type == nix::actCopyPath);

  //                       if (is_dl_a != is_dl_b)
  //                         {
  //                           return !is_dl_a && is_dl_b;
  //                         }

  //                       // if (is_dl_a && is_dl_b)
  //                       //   {
  //                       //     auto prog_a = compute_progress (a.id);
  //                       //     auto prog_b = compute_progress (b.id);

  //                       //     return prog_a && prog_b && prog_a->expected >
  //                       //     prog_b->expected;
  //                       //   }

  //                       return false;
  //                     });
}

size_t
UiStateBuilder::order_for_id (int64_t id) const
{
  if (const auto *info = state_.get_activity (id))
    {
      return info->start_order;
    }
  return std::numeric_limits<size_t>::max ();
}

std::string
UiStateBuilder::tag_for_type (ActivityType type)
{
  switch (type)
    {
    case nix::actSubstitute:
      return std::string{ "substitute" };
    case nix::actCopyPath:
      return std::string{ "%" };
    case nix::actFileTransfer:
      return std::string{ "transfer" };
    case nix::actQueryPathInfo:
      return std::string{ "query" };
    case nix::actBuilds:
      return std::string{ "builds" };
    case nix::actBuild:
      return std::string{ "$" };
    case nix::actBuildWaiting:
      return std::string{ "queued" };
    case nix::actFetchTree:
      return std::string{ "fetch" };
    case nix::actCopyPaths:
      return std::string{ "❡" };
    default:
      return fmt::format ("{}", NixLogParser::activity_type_name (type));
    }
}

bool
UiStateBuilder::is_collapse_type (ActivityType type)
{
  switch (type)
    {
      // case nix::actSubstitute:
      // case nix::actCopyPath:
    // case nix::actQueryPathInfo:
    case nix::actFileTransfer:
      //  case nix::actCopyPath:
      return true;
    default:
      return false;
    }
}

} // namespace nixb
