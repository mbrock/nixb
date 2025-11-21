#include "NixBuildState.hpp"

#include <nix/store/store-api.hh>

namespace nixb
{

namespace
{
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
} // namespace

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
  ActivityInfo info{ e.type, e.text };
  info.parent = e.parent;
  info.start_order = next_activity_order_++;

  if (e.store_ref)
    {
      if (store_)
        {
          if (auto path = store_->maybeParseStorePath (e.store_ref->path))
            {
              info.store_path = path;
              info.label = std::string{ path->name () };
            }
        }
      if (info.label.empty ())
        {
          info.label = e.store_ref->path;
        }
      if (e.store_ref->base_url)
        {
          info.store_base_url = *e.store_ref->base_url;
        }
    }
  else if (e.type == nix::actFileTransfer && !e.fields.empty ())
    {
      // For file transfers, fields[0] contains the clean URL
      info.label = url_basename (e.fields[0]);
    }

  activities_[e.id] = std::move (info);

  if (e.type == nix::actBuilds)
    {
      builds_activity_ = e.id;
    }

  if (e.type == nix::actFileTransfer || e.type == nix::actCopyPath)
    {
      note_transfer_start (e.id);
    }
}

void
NixBuildState::stop_activity (int64_t id)
{
  auto it = activities_.find (id);
  if (it == activities_.end ())
    {
      return;
    }

  const auto &info = it->second;

  if (info.type == nix::actBuilds)
    {
      builds_progress_.aggregate.reset ();
      builds_activity_.reset ();
    }

  if (info.type == nix::actFileTransfer || info.type == nix::actCopyPath)
    {
      note_transfer_stop (id);
    }

  activities_.erase (it);
}

void
NixBuildState::update_progress (const ResultEvent &e)
{
  update_success_tokens (e);

  if (e.type == nix::resSetExpected)
    {
      auto expected_val = e.get_int (1);
      if (expected_val)
        {
          set_builds_expected (*expected_val);
          if (builds_activity_)
            {
              auto builds_it = activities_.find (*builds_activity_);
              if (builds_it != activities_.end ())
                {
                  builds_it->second.progress.expected = *expected_val;
                  builds_it->second.has_progress = true;
                }
            }
        }
      return;
    }

  if (e.type == nix::resSetPhase)
    {
      if (auto phase = e.get_string (0))
        {
          set_current_phase (std::string{ *phase });
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
  it->second.progress = progress;
  it->second.has_progress = true;

  switch (it->second.type)
    {
    case nix::actBuilds:
      set_builds_progress (progress);
      break;
    case nix::actFileTransfer:
    case nix::actCopyPath:
      update_transfer_progress (e.id, progress);
      break;
    default:
      break;
    }
}

void
NixBuildState::update_success_tokens (const ResultEvent &e)
{
  if (e.type != nix::resProgress || !builds_activity_
      || *builds_activity_ != e.id)
    {
      return;
    }
  if (auto done = e.get_int (0))
    {
      if (*done > last_progress_done_)
        {
          success_tokens_ += (*done - last_progress_done_);
          last_progress_done_ = *done;
        }
    }
}

void
NixBuildState::decrement_success_tokens ()
{
  if (success_tokens_ > 0)
    {
      --success_tokens_;
    }
}

void
NixBuildState::note_transfer_start (int64_t id)
{
  if (!active_transfers_.insert (id).second)
    {
      return;
    }
  transfer_progress_.emplace (id, ActivityProgress{});
}

void
NixBuildState::note_transfer_stop (int64_t id)
{
  active_transfers_.erase (id);
  transfer_progress_.erase (id);
}

void
NixBuildState::update_transfer_progress (int64_t id,
                                         const ActivityProgress &progress)
{
  if (active_transfers_.count (id))
    {
      transfer_progress_[id] = progress;
    }
}

void
NixBuildState::set_builds_expected (int64_t expected)
{
  if (expected <= 0)
    {
      builds_progress_.aggregate.reset ();
      return;
    }
  if (!builds_progress_.aggregate)
    {
      builds_progress_.aggregate = ActivityProgress{};
    }
  builds_progress_.aggregate->expected = expected;
  if (builds_progress_.aggregate->done > expected)
    {
      builds_progress_.aggregate->done = expected;
    }
  builds_progress_.aggregate->running = 0;
  builds_progress_.aggregate->failed = 0;
}

void
NixBuildState::set_builds_progress (const ActivityProgress &progress)
{
  builds_progress_.aggregate = progress;
}

void
NixBuildState::set_current_phase (const std::string &phase)
{
  builds_progress_.current_phase = phase;
}

void
NixBuildState::clear_builds_aggregate ()
{
  builds_progress_.aggregate.reset ();
}

std::string
NixBuildState::format_activity_label (const ActivityInfo &info) const
{
  if (info.store_path)
    {
      return std::string{ info.store_path->name () };
    }
  if (!info.label.empty ())
    {
      return info.label;
    }
  if (info.type == nix::actFileTransfer)
    {
      if (!info.text.empty ())
        {
          return url_basename (info.text);
        }
    }
  if (!info.text.empty ())
    {
      return info.text;
    }
  return "activity";
}

} // namespace nixb
