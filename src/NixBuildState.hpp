#pragma once

#include "NixLogParser.hpp"
#include "UiTypes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <nix/store/derivations.hh>
#include <nix/store/path.hh>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace nix
{
class Store;
}

namespace nixb
{

// Yearning: Build plan from "these N derivations will be built"
// Static metadata loaded upfront, defines dependency tree structure
struct Yearning
{
  nix::StorePath derivation_path;
  nix::Derivation derivation;
  std::vector<int64_t>
      dependency_yearning_ids;             // IDs of yearnings this depends on
  size_t order = 0;                        // Order in build plan
  std::optional<int64_t> live_activity_id; // Linked real activity, if started
};

// ActivityInfo: Live events from Nix's @nix JSON stream
// Dynamic state: phases, progress, current status
struct ActivityInfo
{
  ActivityType type;
  std::string text;
  std::optional<nix::StorePath> store_path;
  std::optional<nix::StorePath> derivation_path;
  std::optional<nix::Derivation> derivation;
  std::optional<std::string> store_base_url;
  std::string label;
  std::optional<int64_t> parent;
  ActivityProgress progress;
  bool has_progress = false;
  size_t start_order = 0;
  std::string current_phase;
  bool is_finished = false;
  std::optional<std::chrono::steady_clock::time_point> finish_time;
  std::vector<nix::StorePath> input_drv_paths; // Build dependencies

  std::string to_json () const;
};

class NixBuildState
{
public:
  explicit NixBuildState (std::shared_ptr<nix::Store> store);

  void start_activity (const StartEvent &e);
  void stop_activity (int64_t id);
  void update_progress (const ResultEvent &e);
  void cleanup_finished_activities ();

  const std::unordered_map<int64_t, ActivityInfo> &
  activities () const
  {
    return activities_;
  }

  const std::unordered_map<int64_t, Yearning> &
  yearnings () const
  {
    return yearnings_;
  }

  const ActivityInfo *get_activity (int64_t id) const;
  const Yearning *get_yearning (int64_t id) const;

  std::string format_activity_label (const ActivityInfo &info) const;

  // Returns a generation counter that increments whenever activities change
  size_t
  generation () const
  {
    return generation_;
  }

  // Retained-mode tree structure accessors
  const std::unordered_map<int64_t, std::vector<int64_t>> &
  activity_children () const
  {
    return activity_children_;
  }
  const std::vector<int64_t> &
  activity_roots () const
  {
    return activity_roots_;
  }
  const std::map<std::string, int64_t> &
  drv_path_to_activity () const
  {
    return drv_path_to_activity_;
  }
  const std::unordered_map<int64_t, std::vector<int64_t>> &
  activity_dependents () const
  {
    return activity_dependents_;
  }

  void yearn_for_derivation (const nix::StorePath &path);
  void yearn ();

private:
  void create_synthetic_activities ();
  void build_activity_tree (); // Build tree from scratch
  void add_activity_to_tree (int64_t id, const ActivityInfo &info);
  void remove_activity_from_tree (int64_t id);
  ActivityType infer_activity_type (const StartEvent &e) const;
  void process_store_ref (const StartEvent &e, ActivityInfo &info);
  void process_text_fallback (const StartEvent &e, ActivityType type,
                              ActivityInfo &info);
  void extract_label_from_quotes (std::string_view text,
                                  ActivityInfo &info) const;

  std::shared_ptr<nix::Store> store_;

  // Yearnings: build plan loaded from "these N derivations" message
  std::unordered_map<int64_t, Yearning> yearnings_; // Negative IDs
  size_t next_yearning_order_ = 0;

  // Activities: live events from Nix's @nix stream
  std::unordered_map<int64_t, ActivityInfo> activities_; // Nix's huge IDs
  size_t next_activity_order_ = 0;

  // Association between yearnings and activities
  std::unordered_map<int64_t, int64_t>
      yearning_to_activity_; // yearning_id -> activity_id
  std::unordered_map<int64_t, int64_t>
      activity_to_yearning_; // activity_id -> yearning_id

  // Temporary: for building yearnings
  std::vector<nix::StorePath> derivations_to_build_;
  std::multimap<nix::StorePath, nix::StorePath> deps_;

  size_t generation_ = 0; // Increments whenever state changes

  // Retained-mode tree (built from yearnings, not activities!)
  std::unordered_map<int64_t, std::vector<int64_t>> activity_children_;
  std::vector<int64_t> activity_roots_;
  std::map<std::string, int64_t> drv_path_to_activity_;
  std::unordered_map<int64_t, std::vector<int64_t>> activity_dependents_;
};

} // namespace nixb
