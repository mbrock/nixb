#pragma once

#include "NixBuildState.hpp"
#include "UiTypes.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nixb
{

// Builds UiState from NixBuildState by transforming activity data into
// a hierarchical display structure with progress information.
class UiStateBuilder
{
public:
  explicit UiStateBuilder (const NixBuildState &state);

  // Build and return a complete UiState snapshot
  UiState build ();

private:
  // Build the activity tree structure (parent-child relationships)
  void build_activity_tree ();

  // Get the progress for an activity (if it has direct progress)
  std::optional<ActivityProgress> compute_progress (int64_t id);

  // Check if an activity should be hidden (filtered out) from the display.
  // Returns true for wrapper activities like Realise, Builds, or collapsible
  // activities. Each line is evaluated independently - hiding a parent doesn't
  // automatically show or hide its children.
  bool should_hide_activity (int64_t id) const;

  // Format a display label for an activity
  std::string
  format_display_label (const ActivityInfo &info, int visible_depth,
                        const std::string &tag, const std::string &label,
                        const std::optional<std::string> &base_url);

  // Recursively emit a tree node and its children. visible_depth is the
  // indentation level counting only visible ancestors (hidden parents don't
  // increase depth).
  void emit_tree_node (int64_t id, int visible_depth);

  // Sort activity lines (downloads at bottom)
  void sort_activity_lines ();

  // Get the start order for an activity
  size_t order_for_id (int64_t id) const;

  // Helper: get tag string for activity type
  static std::string tag_for_type (ActivityType type);

  // Helper: strip store hash prefix from derivation names
  static std::string strip_store_hash (std::string_view name);

  // Helper: check if activity type should be collapsed
  static bool is_collapse_type (ActivityType type);

  // Reference to the build state we're transforming
  const NixBuildState &state_;

  // Tree structure: parent ID -> child IDs
  std::unordered_map<int64_t, std::vector<int64_t>> children_;

  // Root activity IDs (no parent or parent not in activities)
  std::vector<int64_t> roots_;

  // Dependency tracking: derivation path -> activity ID
  std::map<std::string, int64_t> drv_path_to_activity_;

  // Dependency tracking: activity ID -> activities that depend on it
  std::unordered_map<int64_t, std::vector<int64_t>> dependents_;

  // The UiState being built
  UiState result_;
};

} // namespace nixb
