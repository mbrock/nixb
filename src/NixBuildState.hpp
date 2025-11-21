#pragma once

#include "NixLogParser.hpp"
#include "UiTypes.hpp"

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

  std::string format_activity_label (const ActivityInfo &info) const;

private:
  ActivityType infer_activity_type (const StartEvent &e) const;
  void process_store_ref (const StartEvent &e, ActivityInfo &info);
  void process_text_fallback (const StartEvent &e, ActivityType type,
                              ActivityInfo &info);
  void extract_label_from_quotes (std::string_view text,
                                  ActivityInfo &info) const;

  std::shared_ptr<nix::Store> store_;
  std::unordered_map<int64_t, ActivityInfo> activities_;
  size_t next_activity_order_ = 0;
};

} // namespace nixb
