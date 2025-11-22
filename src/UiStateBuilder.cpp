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

  // Just read the retained tree structure from state
  children_ = state_.activity_children ();
  roots_ = state_.activity_roots ();
  drv_path_to_activity_ = state_.drv_path_to_activity ();
  dependents_ = state_.activity_dependents ();

  // Track which activities we've already emitted to avoid duplicates in DAG
  std::unordered_set<int64_t> emitted_ids;

  // Sort roots: prioritize branches that contain active (non-queued) builds
  auto roots_sorted = roots_;

  std::function<bool (int64_t, std::unordered_set<int64_t> &)>
      has_active_descendant
      = [this, &has_active_descendant] (
            int64_t root_id, std::unordered_set<int64_t> &visited) -> bool {
    if (visited.contains (root_id))
      return false;
    visited.insert (root_id);

    const auto *yearning = state_.get_yearning (root_id);
    if (yearning && yearning->live_activity_id)
      {
        const auto *activity
            = state_.get_activity (*yearning->live_activity_id);
        if (activity && activity->type != nix::actBuildWaiting)
          return true;
      }

    const auto &children_map = state_.activity_children ();
    auto it = children_map.find (root_id);
    if (it != children_map.end ())
      {
        for (int64_t child_id : it->second)
          {
            if (has_active_descendant (child_id, visited))
              return true;
          }
      }
    return false;
  };

  std::stable_sort (roots_sorted.begin (), roots_sorted.end (),
                    [&] (int64_t a, int64_t b) {
                      std::unordered_set<int64_t> visited_a, visited_b;
                      bool a_has_active = has_active_descendant (a, visited_a);
                      bool b_has_active = has_active_descendant (b, visited_b);

                      if (a_has_active != b_has_active)
                        return a_has_active; // Active branches first

                      return false; // Keep original order otherwise
                    });

  // Render the tree - branches with active builds are now at top
  constexpr int max_depth = 8;
  constexpr size_t max_roots = 20; // Show first N roots

  size_t roots_shown = 0;
  for (int64_t root_id : roots_sorted)
    {
      if (roots_shown++ >= max_roots)
        break;
      emit_tree_node (root_id, 0, max_depth, emitted_ids);
    }

  // Summary of what's not shown
  size_t total_not_shown = state_.yearnings ().size () - emitted_ids.size ();

  if (total_not_shown > 0)
    {
      result_.activity_lines.push_back (UiActivityLine{
          -999999, fmt::format ("... and {} more packages", total_not_shown),
          std::nullopt, std::nullopt, false, 0.0, 0, 0 });
    }

  sort_activity_lines ();

  return result_;
}

// NOTE: This method is now obsolete - the tree is built and maintained
// in NixBuildState in a retained-mode fashion. Keeping the signature
// for now in case we need to add any UiStateBuilder-specific processing.
void
UiStateBuilder::build_activity_tree ()
{
  // Tree is now maintained in NixBuildState, just read it from there
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
    return true;

  if (info->type == nix::actRealise || info->type == nix::actBuilds
      || info->type == nix::actSubstitute || info->type == nix::actFileTransfer
      || info->type == nix::actCopyPath || info->type == nix::actUnknown)
    return true;

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
UiStateBuilder::emit_tree_node (int64_t yearning_id, int visible_depth,
                                int max_depth,
                                std::unordered_set<int64_t> &emitted_ids)
{
  // We're iterating YEARNINGS now, not activities!
  // Try to get yearning (negative IDs) or standalone activity (positive IDs)
  const auto *yearning = state_.get_yearning (yearning_id);
  const ActivityInfo *activity = nullptr;

  // If this is a standalone activity (no yearning), get it directly
  if (!yearning)
    {
      activity = state_.get_activity (yearning_id);
      if (!activity)
        {
          return; // Neither yearning nor activity found
        }
    }
  else
    {
      // Get linked activity for this yearning (if any)
      if (yearning->live_activity_id)
        {
          activity = state_.get_activity (*yearning->live_activity_id);
        }
    }

  // Stop recursion if we've hit max depth
  if (max_depth <= 0)
    {
      return;
    }

  // If we've already emitted this node, skip it silently
  if (emitted_ids.contains (yearning_id))
    {
      return;
    }

  // Mark this node as emitted
  if (yearning)
    emitted_ids.insert (yearning_id);

  // Determine if we should show this line
  bool show_this_line = activity->type == nix::actBuild
                        || activity->type == nix::actBuildWaiting
                        || activity->type == nix::actCopyPath;

  int next_visible_depth = visible_depth;

  if (show_this_line)
    {
      // Build display info from yearning or activity
      std::string tag;
      std::string label;
      std::optional<std::string> url_part;
      std::optional<ActivityProgress> progress;
      bool is_finished = false;
      double fade_factor = 0.0;
      size_t num_input_deps = 0;
      size_t num_dependents = 0;

      // Get label from yearning or activity
      if (yearning)
        {
          label = std::string{ yearning->derivation.name } + " "
                  + yearning->derivation.platform;
          num_input_deps = yearning->dependency_yearning_ids.size ();
        }
      else if (activity)
        {
          label = state_.format_activity_label (*activity);
        }

      // Get tag and state from activity
      if (activity)
        {
          tag = tag_for_type (activity->type);

          // Use current phase if available
          if (!activity->current_phase.empty ())
            {
              std::string phase = activity->current_phase;
              constexpr std::string_view suffix = "Phase";
              if (phase.size () > suffix.size ()
                  && phase.compare (phase.size () - suffix.size (),
                                    suffix.size (), suffix)
                         == 0)
                {
                  phase.erase (phase.size () - suffix.size (), suffix.size ());
                }
              std::transform (phase.begin (), phase.end (), phase.begin (),
                              [] (unsigned char c) {
                                return static_cast<char> (std::tolower (c));
                              });
              tag = fmt::format ("[{}]", phase);
            }

          // Get progress from activity
          if (activity->has_progress)
            progress = activity->progress;

          is_finished = activity->is_finished;

          if (activity->is_finished && activity->finish_time)
            {
              auto now = std::chrono::steady_clock::now ();
              auto elapsed = now - *activity->finish_time;
              auto elapsed_ms
                  = std::chrono::duration_cast<std::chrono::milliseconds> (
                        elapsed)
                        .count ();
              fade_factor = std::min (1.0, elapsed_ms / 2000.0);
            }

          if (activity->store_base_url)
            {
              std::string_view url = *activity->store_base_url;
              if (url.starts_with ("https://"))
                url = url.substr (8);
              else if (url.starts_with ("http://"))
                url = url.substr (7);
              url_part = std::string (url);
            }
        }
      else if (yearning)
        {
          // Yearning without live activity - show as queued
          tag = "queued";
        }

      // Get dependency counts
      auto dep_it = dependents_.find (yearning_id);
      if (dep_it != dependents_.end ())
        {
          num_dependents = dep_it->second.size ();
        }

      std::string indent;
      if (visible_depth > 0)
        {
          indent.assign (static_cast<size_t> (visible_depth) * 2, ' ');
        }

      std::string display = indent;
      if (!tag.empty ())
        display += tag + " ";
      display += label;

      result_.activity_lines.push_back (UiActivityLine{
          yearning_id, std::move (display), url_part, progress, is_finished,
          fade_factor, num_input_deps, num_dependents });

      next_visible_depth = visible_depth + 1;
    }

  auto it = children_.find (yearning_id);
  if (it != children_.end ())
    {
      for (int64_t child_yearning_id : it->second)
        {
          emit_tree_node (child_yearning_id, next_visible_depth, max_depth - 1,
                          emitted_ids);
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
      return std::string{ "" };
    case nix::actFileTransfer:
      return std::string{ "transfer" };
    case nix::actQueryPathInfo:
      return std::string{ "query" };
    case nix::actBuilds:
      return std::string{ "builds" };
    case nix::actBuild:
      return std::string{ "" };
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
