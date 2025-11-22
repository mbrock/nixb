#pragma once

#include <chrono>
#include <coro/coro.hpp>
#include <fmt/core.h>

namespace nixb::coro_prototype
{

using namespace std::chrono_literals;

// Example: Activity with full lifecycle including animations
class DownloadActivity
{
public:
  DownloadActivity (int64_t id, std::string url)
      : id_ (id), url_ (std::move (url))
  {
  }

  // The beautiful part: lifecycle as linear code!
  coro::task<void>
  run ()
  {
    // Phase 1: Fade in
    co_await fade_in ();

    // Phase 2: Download with progress
    co_await download_with_progress ();

    // Phase 3: Linger (this is what you mentioned!)
    co_await linger ();

    // Phase 4: Fade out
    co_await fade_out ();

    // Done! Coroutine completes, can be cleaned up
  }

private:
  coro::task<void>
  fade_in ()
  {
    fmt::print ("[Download {}] Fading in...\n", id_);

    // Animate opacity 0.0 → 1.0 over 200ms
    constexpr int steps = 10;
    constexpr auto step_duration = 20ms;

    for (int i = 0; i <= steps; ++i)
      {
        opacity_ = static_cast<float> (i) / steps;
        co_await sleep (step_duration);
      }

    fmt::print ("[Download {}] Fade in complete (opacity={})\n", id_,
                opacity_);
  }

  coro::task<void>
  download_with_progress ()
  {
    fmt::print ("[Download {}] Downloading {}\n", id_, url_);

    // Simulate download progress
    for (int progress = 0; progress <= 100; progress += 10)
      {
        progress_ = progress;
        fmt::print ("[Download {}] {}%\n", id_, progress_);
        co_await sleep (100ms);
      }

    fmt::print ("[Download {}] Download complete!\n", id_);
  }

  coro::task<void>
  linger ()
  {
    fmt::print ("[Download {}] Lingering for 2 seconds...\n", id_);
    co_await sleep (2s);
    fmt::print ("[Download {}] Linger complete\n", id_);
  }

  coro::task<void>
  fade_out ()
  {
    fmt::print ("[Download {}] Fading out...\n", id_);

    constexpr int steps = 10;
    constexpr auto step_duration = 30ms;

    for (int i = steps; i >= 0; --i)
      {
        opacity_ = static_cast<float> (i) / steps;
        co_await sleep (step_duration);
      }

    fmt::print ("[Download {}] Fade out complete (opacity={})\n", id_,
                opacity_);
  }

  // Helper: sleep for a duration
  coro::task<void>
  sleep (std::chrono::milliseconds duration)
  {
    // In real implementation, would use a timer event
    // For prototype, just yield to show the structure
    co_await std::suspend_always{};
    // TODO: integrate with libcoro's io_scheduler or thread_pool
  }

  int64_t id_;
  std::string url_;
  float opacity_ = 0.0f;
  int progress_ = 0;
};

// Example: Build activity with phase tracking
class BuildActivity
{
public:
  BuildActivity (int64_t id, std::string derivation)
      : id_ (id), derivation_ (std::move (derivation))
  {
  }

  coro::task<void>
  run ()
  {
    fmt::print ("[Build {}] Starting: {}\n", id_, derivation_);

    // Each phase is explicit
    co_await phase ("unpack", 5s);
    co_await phase ("patch", 2s);
    co_await phase ("configure", 10s);
    co_await phase ("build", 30s);
    co_await phase ("install", 3s);

    fmt::print ("[Build {}] Build complete!\n", id_);

    // Keep showing in UI for a bit
    co_await linger (3s);

    fmt::print ("[Build {}] Cleanup\n", id_);
  }

private:
  coro::task<void>
  phase (std::string name, std::chrono::seconds duration)
  {
    current_phase_ = name;
    fmt::print ("[Build {}] Phase: {}\n", id_, name);

    // Simulate phase with progress updates
    int steps = duration.count ();
    for (int i = 0; i < steps; ++i)
      {
        co_await std::suspend_always{};
      }

    fmt::print ("[Build {}] Phase {} complete\n", id_, name);
  }

  coro::task<void>
  linger (std::chrono::seconds duration)
  {
    fmt::print ("[Build {}] Lingering for {}s...\n", id_, duration.count ());
    co_await std::suspend_always{};
    fmt::print ("[Build {}] Linger done\n", id_);
  }

  int64_t id_;
  std::string derivation_;
  std::string current_phase_;
};

// This is the vision: no state machines, no manual phase tracking
// Each activity is a self-contained coroutine expressing its lifecycle
// Compare this to your current ActivityInfo with state flags!

} // namespace nixb::coro_prototype
