#pragma once

#include <coro/coro.hpp>
#include <memory>
#include <string>
#include <unordered_map>

#include <nix/util/logging.hh>

namespace nixb::coro_prototype
{

// Simplified event types for the prototype
struct ActivityStarted
{
  int64_t id;
  nix::ActivityType type;
  std::string text;
  int64_t parent;
};

struct ActivityProgress
{
  int64_t id;
  nix::ResultType type;
  std::string data;
};

struct ActivityStopped
{
  int64_t id;
};

using ActivityEvent
    = std::variant<ActivityStarted, ActivityProgress, ActivityStopped>;

// Event channel that coroutines can await on
// Each activity gets its own channel
class ActivityEventChannel
{
public:
  ActivityEventChannel () = default;

  // Push an event (called by logger)
  void
  push (ActivityEvent event)
  {
    events_.push_back (std::move (event));
  }

  // Await the next event (called by coroutine)
  coro::task<ActivityEvent>
  next ()
  {
    // Simple polling for now - could be made more efficient
    while (events_.empty ())
      {
        co_await coro::sync_wait (
            coro::event{}.reset ()); // Yield to scheduler
      }

    auto event = std::move (events_.front ());
    events_.erase (events_.begin ());
    co_return event;
  }

  bool
  has_events () const
  {
    return !events_.empty ();
  }

private:
  std::vector<ActivityEvent> events_;
};

// The coroutine that manages an activity's lifecycle
class ActivityActor
{
public:
  explicit ActivityActor (ActivityStarted start)
      : id_ (start.id), text_ (start.text), type_ (start.type)
  {
  }

  // The main lifecycle coroutine
  coro::task<void>
  run (std::shared_ptr<ActivityEventChannel> channel)
  {
    fmt::print ("[Activity {}] Started: {}\n", id_, text_);

    // Process events until stopped
    while (true)
      {
        auto event = co_await channel->next ();

        if (std::holds_alternative<ActivityProgress> (event))
          {
            auto &progress = std::get<ActivityProgress> (event);
            fmt::print ("[Activity {}] Progress: {}\n", id_, progress.data);
          }
        else if (std::holds_alternative<ActivityStopped> (event))
          {
            fmt::print ("[Activity {}] Stopped\n", id_);
            break;
          }
      }

    // Linger phase (this is where the magic happens!)
    fmt::print ("[Activity {}] Lingering for 2 seconds...\n", id_);
    co_await coro::sync_wait (coro::event{}.reset ()); // Simulate delay

    fmt::print ("[Activity {}] Cleanup complete\n", id_);
  }

private:
  int64_t id_;
  std::string text_;
  nix::ActivityType type_;
};

// Event dispatcher that manages coroutines
class CoroEventDispatcher
{
public:
  CoroEventDispatcher () = default;

  void
  on_activity_start (ActivityStarted start)
  {
    auto channel = std::make_shared<ActivityEventChannel> ();
    channels_[start.id] = channel;

    // Spawn the activity coroutine
    auto actor = std::make_unique<ActivityActor> (start);
    auto task = actor->run (channel);

    // Store it (in real version, would schedule on thread pool)
    actors_[start.id] = std::move (actor);

    fmt::print ("[Dispatcher] Spawned coroutine for activity {}\n", start.id);
  }

  void
  on_activity_progress (ActivityProgress progress)
  {
    if (auto it = channels_.find (progress.id); it != channels_.end ())
      {
        it->second->push (progress);
      }
  }

  void
  on_activity_stop (ActivityStopped stop)
  {
    if (auto it = channels_.find (stop.id); it != channels_.end ())
      {
        it->second->push (stop);
      }
  }

private:
  std::unordered_map<int64_t, std::shared_ptr<ActivityEventChannel>> channels_;
  std::unordered_map<int64_t, std::unique_ptr<ActivityActor>> actors_;
};

// Logger that feeds events to the coroutine dispatcher
class CoroNixLogger : public nix::Logger
{
public:
  explicit CoroNixLogger (std::shared_ptr<CoroEventDispatcher> dispatcher)
      : dispatcher_ (std::move (dispatcher))
  {
  }

  void
  log (nix::Verbosity lvl, std::string_view s) override
  {
    // Pass through to stderr
    fmt::print (stderr, "{}\n", s);
  }

  void
  logEI (const nix::ErrorInfo &ei) override
  {
    // Simplified for prototype
    fmt::print (stderr, "Error: {}\n", ei.msg.str ());
  }

  void
  startActivity (nix::ActivityId act, nix::Verbosity lvl,
                 nix::ActivityType type, const std::string &text,
                 const nix::Logger::Fields &fields,
                 nix::ActivityId parent) override
  {
    ActivityStarted start{ .id = act,
                           .type = type,
                           .text = text,
                           .parent = parent };
    dispatcher_->on_activity_start (start);
  }

  void
  stopActivity (nix::ActivityId act) override
  {
    ActivityStopped stop{ .id = act };
    dispatcher_->on_activity_stop (stop);
  }

  void
  result (nix::ActivityId act, nix::ResultType type,
          const nix::Logger::Fields &fields) override
  {
    // Extract data from fields
    std::string data;
    if (!fields.empty () && fields[0].type == nix::Logger::Field::tString)
      {
        data = fields[0].s;
      }

    ActivityProgress progress{ .id = act, .type = type, .data = data };
    dispatcher_->on_activity_progress (progress);
  }

private:
  std::shared_ptr<CoroEventDispatcher> dispatcher_;
};

} // namespace nixb::coro_prototype
