#include "NixBuildState.hpp"
#include "nix/store/derivations.hh"
#include "nix/store/derived-path.hh"
#include "nix/store/path.hh"
#include "nix/util/logging.hh"

#include <map>
#include <nix/store/store-api.hh>
#include <unordered_map>

namespace nixb
{

NixBuildState::NixBuildState (std::shared_ptr<nix::Store> store)
    : store_ (std::move (store)), derivations_to_build_ ()
{
}

void
NixBuildState::yearn_for_derivation (const nix::StorePath &path)
{
  derivations_to_build_.push_back (path);
}

void
NixBuildState::yearn ()
{
  for (const auto &path : derivations_to_build_)
    {
      auto drv = store_->readDerivation (path);
      for (const auto &[inputDrv, inputNode] : drv.inputDrvs.map)
        {
          deps_.insert (std::make_pair (path, inputDrv));
        }
    }

  // Create synthetic activities for all the derivations we know about
  create_synthetic_activities ();

  // Build the activity tree once after all synthetic activities are created
  build_activity_tree ();

  generation_++;
}

void
NixBuildState::create_synthetic_activities ()
{
  // Create yearnings (build plan) with negative IDs
  int64_t yearning_id = -1;
  std::map<std::string, int64_t> drv_to_yearning;

  // Create yearnings for all planned derivations
  for (const auto &drv_path : derivations_to_build_)
    {
      try
        {
          auto drv = store_->readDerivation (drv_path);

          std::string drv_str = std::string{ drv_path.to_string () };
          drv_to_yearning[drv_str] = yearning_id;

          Yearning yearning{
            drv_path, drv, {}, next_yearning_order_++, std::nullopt
          };

          yearnings_.emplace (yearning_id, std::move (yearning));
          yearning_id--;
        }
      catch (...)
        {
          // Skip derivations we can't read
        }
    }

  // Second pass: populate dependency_yearning_ids from deps_
  for (auto &[id, yearning] : yearnings_)
    {
      // Find all dependencies for this derivation
      auto range = deps_.equal_range (yearning.derivation_path);
      for (auto it = range.first; it != range.second; ++it)
        {
          std::string dep_str = std::string{ it->second.to_string () };
          auto dep_it = drv_to_yearning.find (dep_str);
          if (dep_it != drv_to_yearning.end ())
            {
              yearning.dependency_yearning_ids.push_back (dep_it->second);
            }
        }
    }
}

void
NixBuildState::build_activity_tree ()
{
  // Build tree from YEARNINGS, not activities!
  activity_children_.clear ();
  activity_roots_.clear ();
  activity_dependents_.clear ();

  // Build dependency tree from yearnings
  for (const auto &[yearning_id, yearning] : yearnings_)
    {
      // This yearning's children are its dependencies
      for (int64_t dep_yearning_id : yearning.dependency_yearning_ids)
        {
          activity_children_[yearning_id].push_back (dep_yearning_id);
          // So dep has this yearning as a dependent
          activity_dependents_[dep_yearning_id].push_back (yearning_id);
        }
    }

  // Find roots: yearnings that aren't dependencies of anything
  std::unordered_set<int64_t> is_dependency;
  for (const auto &[parent_id, child_ids] : activity_children_)
    {
      is_dependency.insert (child_ids.begin (), child_ids.end ());
    }

  for (const auto &[yearning_id, yearning] : yearnings_)
    {
      if (!is_dependency.contains (yearning_id))
        {
          activity_roots_.push_back (yearning_id);
        }
    }

  // Note: drv_path_to_activity_ is now built incrementally as activities start

  // Sort by importance: works on yearning IDs
  // Checks if they have live activities to determine priority
  auto importance_cmp = [this] (int64_t a, int64_t b) {
    const auto *yearning_a = get_yearning (a);
    const auto *yearning_b = get_yearning (b);
    if (!yearning_a || !yearning_b)
      return a < b;

    // Compute sort order based on linked activity (if any)
    auto sort_order = [this] (const Yearning &yearning) -> int {
      if (!yearning.live_activity_id)
        return 2; // Queued - medium priority

      const auto *activity = get_activity (*yearning.live_activity_id);
      if (!activity)
        return 2;

      // Lower number = higher priority
      if (activity->type == nix::actBuild)
        return 0; // Building NOW - highest priority
      if (activity->type == nix::actCopyPath
          || activity->type == nix::actFileTransfer)
        return 1; // Downloading - high priority
      if (activity->is_finished)
        return 3; // Done - low priority
      return 1;   // Other active states
    };

    int order_a = sort_order (*yearning_a);
    int order_b = sort_order (*yearning_b);

    if (order_a != order_b)
      return order_a < order_b;

    // Within same priority, sort by build plan order
    if (yearning_a->order != yearning_b->order)
      return yearning_a->order < yearning_b->order;

    return a < b;
  };

  for (auto &[parent_id, child_ids] : activity_children_)
    {
      std::sort (child_ids.begin (), child_ids.end (), importance_cmp);
      child_ids.erase (std::unique (child_ids.begin (), child_ids.end ()),
                       child_ids.end ());
    }
  std::sort (activity_roots_.begin (), activity_roots_.end (), importance_cmp);
}

void
NixBuildState::add_activity_to_tree (int64_t id, const ActivityInfo &info)
{
  // Add to drv_path_to_activity mapping
  if (info.derivation_path)
    {
      try
        {
          std::string drv_str
              = std::string{ info.derivation_path->to_string () };
          drv_path_to_activity_[drv_str] = id;
        }
      catch (...)
        {
        }
    }

  // Add dependency relationships
  for (const auto &input_drv : info.input_drv_paths)
    {
      try
        {
          std::string input_drv_str = std::string{ input_drv.to_string () };
          auto it = drv_path_to_activity_.find (input_drv_str);
          if (it != drv_path_to_activity_.end ())
            {
              int64_t dep_activity_id = it->second;
              activity_dependents_[dep_activity_id].push_back (id);
              activity_children_[dep_activity_id].push_back (id);

              // Remove from roots if it now has a parent
              auto root_it = std::find (activity_roots_.begin (),
                                        activity_roots_.end (), id);
              if (root_it != activity_roots_.end ())
                {
                  activity_roots_.erase (root_it);
                }
            }
        }
      catch (...)
        {
        }
    }

  // If no dependencies found, it's a root
  if (activity_children_.find (id) == activity_children_.end ()
      || activity_children_[id].empty ())
    {
      // Check if it's not already a child of something
      bool is_child = false;
      for (const auto &[parent_id, children] : activity_children_)
        {
          if (std::find (children.begin (), children.end (), id)
              != children.end ())
            {
              is_child = true;
              break;
            }
        }
      if (!is_child)
        {
          activity_roots_.push_back (id);
        }
    }
}

void
NixBuildState::remove_activity_from_tree (int64_t id)
{
  const auto *info = get_activity (id);
  if (!info)
    return;

  // Remove from drv_path_to_activity mapping
  if (info->derivation_path)
    {
      try
        {
          std::string drv_str
              = std::string{ info->derivation_path->to_string () };
          drv_path_to_activity_.erase (drv_str);
        }
      catch (...)
        {
        }
    }

  // Remove from dependents and children maps
  activity_dependents_.erase (id);
  activity_children_.erase (id);

  // Remove from any parent's children list
  for (auto &[parent_id, children] : activity_children_)
    {
      auto it = std::find (children.begin (), children.end (), id);
      if (it != children.end ())
        {
          children.erase (it);
        }
    }

  // Remove from roots
  auto root_it
      = std::find (activity_roots_.begin (), activity_roots_.end (), id);
  if (root_it != activity_roots_.end ())
    {
      activity_roots_.erase (root_it);
    }
}

const ActivityInfo *
NixBuildState::get_activity (int64_t id) const
{
  auto it = activities_.find (id);
  return it != activities_.end () ? &it->second : nullptr;
}

const Yearning *
NixBuildState::get_yearning (int64_t id) const
{
  auto it = yearnings_.find (id);
  return it != yearnings_.end () ? &it->second : nullptr;
}

void
NixBuildState::start_activity (const StartEvent &e)
{
  ActivityType inferred_type = infer_activity_type (e);
  ActivityInfo info{ inferred_type, e.text };
  info.parent = e.parent;
  info.start_order = next_activity_order_++;

  process_store_ref (e, info);
  process_text_fallback (e, inferred_type, info);

  if (info.type == nix::actCopyPath)
    {
      info.has_progress = true;
      info.progress.unit = ProgressUnit::Bytes;
    }

  // Link this activity to its yearning (if one exists)
  if (info.derivation_path)
    {
      try
        {
          std::string drv_str
              = std::string{ info.derivation_path->to_string () };

          // Find yearning by derivation path
          for (auto &[yearning_id, yearning] : yearnings_)
            {
              try
                {
                  if (std::string{ yearning.derivation_path.to_string () }
                      == drv_str)
                    {
                      // Link them!
                      yearning.live_activity_id = e.id;
                      activity_to_yearning_[e.id] = yearning_id;
                      yearning_to_activity_[yearning_id] = e.id;

                      drv_path_to_activity_[drv_str] = e.id;
                      break;
                    }
                }
              catch (...)
                {
                }
            }
        }
      catch (...)
        {
        }
    }

  activities_[e.id] = info;

  // Note: We don't add activities to the tree anymore!
  // Tree structure comes from yearnings only.

  generation_++;
}

ActivityType
NixBuildState::infer_activity_type (const StartEvent &e) const
{
  if (e.type != nix::actUnknown)
    return e.type;

  if (e.text.rfind ("hashing '", 0) == 0)
    return nix::actFileTransfer;
  if (e.text.rfind ("copying '", 0) == 0)
    return nix::actCopyPath;

  return e.type;
}

void
NixBuildState::process_store_ref (const StartEvent &e, ActivityInfo &info)
{
  if (!e.store_ref)
    return;

  if (auto path = store_->maybeParseStorePath (e.store_ref->path))
    {
      info.store_path = path;

      if (auto drv_path = store_->getBuildDerivationPath (*path))
        {
          info.derivation_path = drv_path;
          try
            {
              info.derivation = store_->readDerivation (*drv_path);
            }
          catch (...)
            {
              // Ignore errors when reading derivation
              // (may happen when paths are not valid in current store context)
            }
        }
    }

  if (e.store_ref->base_url)
    info.store_base_url = *e.store_ref->base_url;
}

void
NixBuildState::process_text_fallback (const StartEvent &e, ActivityType type,
                                      ActivityInfo &info)
{
  if (type == nix::actFileTransfer || type == nix::actCopyPath)
    {
      extract_label_from_quotes (e.text, info);
    }
}

void
NixBuildState::extract_label_from_quotes (std::string_view text,
                                          ActivityInfo &info) const
{
  auto first = text.find ('\'');
  if (first == std::string::npos)
    return;

  auto last = text.find_last_of ('\'');
  if (last == std::string::npos || last <= first)
    return;

  if (last - first > 1)
    {
      info.label = text.substr (first + 1, last - first - 1);
    }
}

void
NixBuildState::stop_activity (int64_t id)
{
  auto it = activities_.find (id);
  if (it == activities_.end ())
    return;

  // For file transfer/download activities, mark as finished instead of erasing
  if (it->second.type == nix::actFileTransfer
      || it->second.type == nix::actCopyPath)
    {
      it->second.is_finished = true;
      it->second.finish_time = std::chrono::steady_clock::now ();
    }
  else
    {
      // Incrementally remove from tree before erasing
      remove_activity_from_tree (id);
      // For other activities, erase immediately
      activities_.erase (it);
    }
  generation_++;
}

void
NixBuildState::cleanup_finished_activities ()
{
  auto now = std::chrono::steady_clock::now ();
  constexpr auto timeout = std::chrono::milliseconds (2000); // 1 second

  bool erased_any = false;
  for (auto it = activities_.begin (); it != activities_.end ();)
    {
      if (it->second.is_finished && it->second.finish_time)
        {
          auto elapsed = now - *it->second.finish_time;
          if (elapsed >= timeout)
            {
              // Incrementally remove from tree before erasing
              remove_activity_from_tree (it->first);
              it = activities_.erase (it);
              erased_any = true;
              continue;
            }
        }
      ++it;
    }
  if (erased_any)
    generation_++;
}

void
NixBuildState::update_progress (const ResultEvent &e)
{
  if (e.type == nix::resSetPhase)
    {
      if (auto phase = e.get_string (0))
        {
          auto it = activities_.find (e.id);
          if (it != activities_.end ())
            {
              it->second.current_phase = std::string{ *phase };
            }
        }
      return;
    }

  if (e.type != nix::resProgress)
    {
      return;
    }

  auto it = activities_.find (e.id);
  if (it == activities_.end ())
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

  // Set unit type based on activity type
  if (it->second.type == nix::actFileTransfer
      || it->second.type == nix::actCopyPath)
    {
      progress.unit = ProgressUnit::Bytes;
    }
  if (it->second.type == nix::actCopyPaths)
    {
      progress.unit = ProgressUnit::Count;
    }

  it->second.progress = progress;
  it->second.has_progress = true;
}

std::string
NixBuildState::format_activity_label (const ActivityInfo &info) const
{
  if (info.derivation)
    {
      return std::string{ info.derivation->name } + " "
             + info.derivation->platform;
    }
  if (info.store_path)
    {
      return std::string{ info.store_path->name () };
    }
  if (!info.label.empty ())
    {
      return info.label;
    }
  if (info.derivation_path)
    {
      if (info.derivation)
        {
          return std::string{ info.derivation->name } + " "
                 + info.derivation->platform;
        }
      else
        {
          return std::string{ info.derivation_path->name () };
        }
    }
  if (!info.text.empty ())
    {
      return info.text;
    }
  return "";
}

std::string
ActivityInfo::to_json () const
{
  return nlohmann::json{
    { "type", type },
    { "text", text },
    { "store_path",
      store_path.transform ([] (const nix::StorePath &path) -> std::string {
        return std::string{ path.to_string () };
      }) },
    { "derivation_path", derivation_path.transform (
                             [] (const nix::StorePath &path) -> std::string {
                               return std::string{ path.to_string () };
                             }) },
    { "derivation",
      derivation.transform ([] (const nix::Derivation &drv) -> std::string {
        return std::string{ drv.name };
      }) },
    { "store_base_url", store_base_url },
    { "label", label }
  }.dump ();
}

} // namespace nixb
