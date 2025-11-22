# Coroutine Prototype

Experimental coroutine-based architecture using libcoro.

## Concept

Instead of manually managing activity state in maps with state machines, each activity becomes an independent coroutine:

```cpp
task<void> activity_lifecycle(ActivityStarted start) {
  // Fade in
  co_await animate_opacity(0.0 → 1.0, 200ms);

  // Process events
  while (true) {
    auto event = co_await channel.next();
    if (stopped) break;
  }

  // Linger before cleanup
  co_await sleep(2s);

  // Fade out
  co_await animate_opacity(1.0 → 0.0, 300ms);
}
```

## Architecture

```
Nix API
  ↓
CoroNixLogger (implements nix::Logger)
  ↓
CoroEventDispatcher
  ↓
ActivityEventChannel (one per activity)
  ↓
ActivityActor coroutine (manages lifecycle)
```

## Dependencies

- **libcoro**: Header-only C++20 coroutine library
  - Included as git subtree in `subprojects/libcoro`
  - Already in the repo, no setup needed!
  - Integrated into meson build system

## Building

The prototypes are integrated into the main meson build:

```bash
# Just build normally - libcoro is already in the repo
meson setup build
meson compile -C build

# Run demos
./build/src/new/nixb-lifecycle-demo          # No Nix needed
./build/src/new/nixb-coro-prototype          # With Nix integration
```

If you need to update libcoro later:
```bash
git subtree pull --prefix subprojects/libcoro \
  https://github.com/jbaldwin/libcoro main --squash
```

## Next Steps

1. **Real scheduler**: libcoro has `thread_pool` - use it to actually run coroutines
2. **Timing**: Add real delays with `coro::sync_wait()` or timers
3. **Channels**: Use libcoro's `channel<T>` instead of manual event vectors
4. **UI integration**: Render loop as a coroutine that yields every frame
5. **Nix integration**: Actually call `store->buildPaths()` and see real events flow

## Why This Is Exciting

- Each activity is **self-contained** - no shared state machines
- Lifecycle is **explicit** - you read the code and see: start → update → linger → cleanup
- **Composable** - can have sub-coroutines for animations, HTTP requests, etc.
- **Testable** - inject mock event channels, control timing precisely
- **Future-proof** - naturally extends to distributed builds (remote builder = async I/O)
