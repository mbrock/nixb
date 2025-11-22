# The Coroutine Vision

## The Problem

Current architecture (src/NixBuildState.{hpp,cpp}):

```cpp
// Activities stored in a map
std::unordered_map<int64_t, ActivityInfo> activities_;

// ActivityInfo has manual state tracking
struct ActivityInfo {
  bool is_finished = false;
  bool has_progress = false;
  std::string current_phase;
  std::chrono::time_point start_time;
  std::chrono::time_point end_time;
  // ... lots of state flags
};

// Processing logic scattered across multiple functions
void start_activity(const StartEvent& e);
void update_progress(const ResultEvent& e);
void stop_activity(int64_t id);
void cleanup_finished_activities();  // Manual cleanup!
```

Issues:
- **State explosion**: Every lifecycle concern becomes a flag or timestamp
- **Scattered logic**: Lifecycle spread across multiple event handlers
- **Manual cleanup**: Have to track "finished 2 seconds ago" manually
- **Implicit phases**: "Linger before cleanup" is not expressed in code
- **Hard to test**: No way to replay just one activity's lifecycle

## The Solution: Activities as Coroutines

```cpp
// Each activity IS a coroutine with explicit lifecycle
coro::task<void> download_activity(DownloadInfo info) {
  // Phase 1: Appear
  co_await fade_in(200ms);

  // Phase 2: Active
  while (auto event = co_await events.next()) {
    if (event.is_progress()) {
      update_progress(event);
    }
    if (event.is_complete()) break;
  }

  // Phase 3: Linger (the feature you love!)
  co_await sleep(2s);

  // Phase 4: Disappear
  co_await fade_out(300ms);

  // Coroutine completes → automatic cleanup
}
```

Benefits:
- **Linear code**: Read top-to-bottom to understand lifecycle
- **Explicit phases**: Every transition is visible
- **Automatic cleanup**: When coroutine completes, it's gone
- **Local state**: Variables in coroutine frame, not maps
- **Testable**: Inject mock event streams
- **Composable**: Nest coroutines for sub-activities

## Architecture Shift

### Before: Event-Driven State Machine

```
Event arrives
  ↓
Find ActivityInfo in map
  ↓
Update state flags
  ↓
Check if should cleanup (manual)
  ↓
Maybe cleanup (if time > threshold)
```

### After: Coroutine Actor Model

```
Event arrives
  ↓
Find activity's event channel
  ↓
Push event to channel
  ↓
Coroutine resumes from co_await
  ↓
Processes event inline
  ↓
Suspends again or completes
```

## Implementation Plan

### Phase 1: Proof of Concept ✅ (YOU ARE HERE)
- [x] Basic coroutine structure
- [x] Mock activity lifecycle
- [x] Nix logger integration skeleton
- [x] Event channel concept

### Phase 2: Real Integration
- [ ] libcoro scheduler (thread_pool or io_scheduler)
- [ ] Real timing (actual sleep/delays)
- [ ] Channel per activity (libcoro::channel<T>)
- [ ] Hook to real Nix builds

### Phase 3: UI Integration
- [ ] Render loop as coroutine
- [ ] Opacity/animation helpers
- [ ] Terminal rendering from coroutine state
- [ ] Preserve record/playback workflow

### Phase 4: Advanced Features
- [ ] Distributed builds (remote builder = async I/O)
- [ ] Build queuing/prioritization
- [ ] Cancellation (co_await cancelled())
- [ ] Structured concurrency (parent/child activities)

## Code Comparison

### Download Linger: Before

```cpp
// In ActivityInfo
std::optional<time_point> finish_time;

// In event handler
void stop_activity(int64_t id) {
  auto& activity = activities_[id];
  activity.is_finished = true;
  activity.finish_time = now();
}

// In cleanup loop (called periodically)
void cleanup_finished_activities() {
  for (auto it = activities_.begin(); it != activities_.end();) {
    if (it->second.is_finished
        && now() - it->second.finish_time > 2s) {
      it = activities_.erase(it);  // Finally cleanup!
    } else {
      ++it;
    }
  }
}
```

### Download Linger: After

```cpp
coro::task<void> activity_lifecycle() {
  // ... do work ...

  co_await sleep(2s);  // That's it!

  // Coroutine completes, cleanup automatic
}
```

**37 lines → 1 line**. And it's self-documenting!

## Inspiration

- **Erlang/OTP**: Gen servers are actors with lifecycle
- **Swift Concurrency**: Each task is independent execution context
- **Rust async**: Futures compose naturally
- **Buck2**: Uses Rust async for build scheduling

The pattern: **Make concurrency units (activities) first-class entities** with their own execution timeline, not data in a map.

## The "Aha!" Moment

You said:
> "downloads stick around for a couple of seconds before they vanish"

That's **temporal logic** - behavior over time. State machines are terrible at expressing time. Coroutines are perfect:

```cpp
// This READS like the behavior:
show_download();
co_await sleep(2s);
hide_download();
```

vs state machine:
```cpp
if (download.state == FINISHED
    && now() - download.finish_time > 2s
    && download.cleanup_scheduled == false) {
  download.cleanup_scheduled = true;
  schedule_cleanup(download.id);
}
```

Coroutines let you **write time** explicitly in code.
