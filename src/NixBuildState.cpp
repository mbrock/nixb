#include "NixBuildState.hpp"
#include "nix/store/derivations.hh"
#include "nix/util/logging.hh"

#include <nix/store/store-api.hh>

namespace nixb
{

NixBuildState::NixBuildState (std::shared_ptr<nix::Store> store)
    : store_ (std::move (store))
{
}

const ActivityInfo *
NixBuildState::get_activity (int64_t id) const
{
  auto it = activities_.find (id);
  return it != activities_.end () ? &it->second : nullptr;
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

  activities_[e.id] = std::move (info);
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
      // For other activities, erase immediately
      activities_.erase (it);
    }
}

void
NixBuildState::cleanup_finished_activities ()
{
  auto now = std::chrono::steady_clock::now ();
  constexpr auto timeout = std::chrono::milliseconds (2000); // 1 second

  for (auto it = activities_.begin (); it != activities_.end ();)
    {
      if (it->second.is_finished && it->second.finish_time)
        {
          auto elapsed = now - *it->second.finish_time;
          if (elapsed >= timeout)
            {
              it = activities_.erase (it);
              continue;
            }
        }
      ++it;
    }
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
      store_path.transform ([] (const nix::StorePath &path) -> std::string
                              { return std::string{ path.to_string () }; }) },
    { "derivation_path", derivation_path.transform (
                             [] (const nix::StorePath &path) -> std::string
                               { return std::string{ path.to_string () }; }) },
    { "derivation",
      derivation.transform ([] (const nix::Derivation &drv) -> std::string
                              { return std::string{ drv.name }; }) },
    { "store_base_url", store_base_url },
    { "label", label }
  }.dump ();
}

} // namespace nixb
