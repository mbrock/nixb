#pragma once

#include "NixLogParser.hpp"
#include "TerminalUi.hpp"

#include <cstdint>
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

struct ActivityInfo
{
  ActivityType type;
  std::string text;
  std::optional<nix::StorePath> store_path;
  std::optional<std::string> store_base_url;
  std::string label;
  std::optional<int64_t> parent;
};

class NixBuildState
{
public:
  explicit NixBuildState (std::shared_ptr<nix::Store> store);

  void start_activity (const StartEvent &e);
  void stop_activity (int64_t id);
  void update_progress (const ResultEvent &e);

  const std::unordered_map<int64_t, ActivityInfo> &
  activities () const
  {
    return activities_;
  }

  const ActivityInfo *get_activity (int64_t id) const;

  std::optional<int64_t>
  builds_activity () const
  {
    return builds_activity_;
  }

  int64_t
  success_tokens () const
  {
    return success_tokens_;
  }
  void decrement_success_tokens ();

  const std::unordered_set<int64_t> &
  active_transfers () const
  {
    return active_transfers_;
  }

  const std::unordered_map<int64_t, ActivityProgress> &
  transfer_progress () const
  {
    return transfer_progress_;
  }

  std::string format_activity_label (const ActivityInfo &info) const;

  struct BuildsProgress
  {
    std::optional<ActivityProgress> aggregate;
    std::string current_phase;
  };

  const BuildsProgress &
  builds_progress () const
  {
    return builds_progress_;
  }
  void set_builds_expected (int64_t expected);
  void set_builds_progress (const ActivityProgress &progress);
  void set_current_phase (const std::string &phase);
  void clear_builds_aggregate ();

private:
  void note_transfer_start (int64_t id);
  void note_transfer_stop (int64_t id);
  void update_success_tokens (const ResultEvent &e);
  void update_transfer_progress (int64_t id, const ActivityProgress &progress);

  std::shared_ptr<nix::Store> store_;
  std::unordered_map<int64_t, ActivityInfo> activities_;
  std::optional<int64_t> builds_activity_;
  int64_t success_tokens_ = 0;
  int64_t last_progress_done_ = 0;
  std::unordered_set<int64_t> active_transfers_;
  std::unordered_map<int64_t, ActivityProgress> transfer_progress_;
  BuildsProgress builds_progress_;
};

} // namespace nixb
