#include "UiStateBuilder.hpp"
#include "NixLogParser.hpp"
#include "nix/util/logging.hh"

#include <algorithm>
#include <cctype>
#include <fmt/core.h>
#include <limits>
#include <unordered_set>

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

  children_.clear ();
  roots_.clear ();
  drv_path_to_activity_.clear ();
  dependents_.clear ();

  // First pass: build derivation path -> activity ID mapping
  for (const auto &[id, info] : activities)
    {
      if (info.derivation_path)
        {
          try
            {
              std::string drv_str
                  = std::string{ info.derivation_path->to_string () };
              drv_path_to_activity_[drv_str] = id;
            }
          catch (const std::exception &)
            {
              // Ignore errors when converting store paths to strings
            }
        }
    }

  // Second pass: build dependency relationships
  for (const auto &[id, info] : activities)
    {
      // For each input derivation this activity depends on,
      // find the activity building that derivation
      for (const auto &input_drv : info.input_drv_paths)
        {
          try
            {
              std::string input_drv_str
                  = std::string{ input_drv.to_string () };
              auto it = drv_path_to_activity_.find (input_drv_str);
              if (it != drv_path_to_activity_.end ())
                {
                  int64_t dep_activity_id = it->second;
                  // This activity (id) depends on dep_activity_id
                  // So dep_activity_id has id as a dependent
                  dependents_[dep_activity_id].push_back (id);
                }
            }
          catch (const std::exception &)
            {
              // Ignore errors when converting store paths to strings
            }
        }
    }

  // Use dependency relationships as the hierarchy
  children_ = dependents_;

  std::unordered_set<int64_t> seen_children;
  for (const auto &[parent_id, child_ids] : children_)
    {
      seen_children.insert (child_ids.begin (), child_ids.end ());
    }

  for (const auto &[id, info] : activities)
    {
      if (!seen_children.contains (id))
        {
          roots_.push_back (id);
        }
    }

  if (roots_.empty ())
    {
      for (const auto &[id, info] : activities)
        {
          roots_.push_back (id);
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
      kv.second.erase (std::unique (kv.second.begin (), kv.second.end ()),
                       kv.second.end ());
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
  return false;
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
      if (info.derivation)
        {
          display_label = std::string{ info.derivation->name } + " "
                          + info.derivation->platform;
        }
      else if (info.derivation_path)
        display_label = info.derivation_path->name ();
      else if (info.store_path)
        display_label = info.store_path->name ();
    }

  std::string indent_prefix;
  if (visible_depth > 0)
    {
      indent_prefix.assign (static_cast<size_t> (visible_depth) * 2, ' ');
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
  return indent_prefix + name_part;
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

      // Get dependency counts
      size_t num_input_deps = info->input_drv_paths.size ();
      size_t num_dependents = 0;
      auto dep_it = dependents_.find (id);
      if (dep_it != dependents_.end ())
        {
          num_dependents = dep_it->second.size ();
        }

      result_.activity_lines.push_back (UiActivityLine{
          id, std::move (display), url_part, progress, info->is_finished,
          fade_factor, num_input_deps, num_dependents });

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
