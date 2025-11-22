# Building a Responsive Terminal UI Framework: Lessons from Wayland, Pike, and Coroutines

## Introduction

Terminal user interfaces might seem like a solved problem - just print some ANSI
codes and call it a day. But building a *responsive*, *efficient*, and
*maintainable* TUI for complex real-time applications reveals surprising
architectural challenges. This article explores the design of a modern terminal
UI framework inspired by windowing systems like Wayland and Rob Pike's Plan 9
innovations, implemented using C++20 coroutines.

The motivating use case is a live build monitor for Nix package builds - imagine
32 CPU cores simultaneously churning through compilation, each generating
hundreds of log events per second, with dynamically changing dependency
relationships, progress bars, phase transitions, and a user watching it all over
SSH on a terminal that might resize at any moment.

## The Fundamental Problem: Many Worlds Running at Different Speeds

A complex TUI has multiple concurrent concerns operating at vastly different
rates:

- **Event ingestion**: 1000+ events/second (build logs, state changes)
- **State updates**: Hundreds/second (dependency graph mutations, progress
  updates)
- **Rendering**: 60 frames/second (what humans can perceive)
- **Display I/O**: 60fps or less (terminal refresh rate)
- **User input**: Sporadic (resize events, keyboard input)

The naive approach - re-render everything on every event - creates a firehose of
terminal output, flickering displays, and wasted CPU cycles rendering frames
that are immediately overwritten.

## Architecture: Three Distinct Layers

### Layer 1: The State Model (Retained-Mode)

The state model is a plain data structure that absorbs the event firehose. It's
updated at event rate but not rendered at event rate:

```cpp
class NixBuildState {
    // Retained dependency graph
    std::unordered_map<int64_t, ActivityInfo> activities_;
    std::unordered_map<int64_t, std::vector<int64_t>> children_;
    std::unordered_map<int64_t, DerivationYearning> yearnings_;
    
    // Updated at event rate (1000/sec)
    void on_activity_started(ActivityStartedEvent e) {
        activities_[e.id] = ActivityInfo{...};
        // Just update data structures, no rendering
    }
    
    void on_progress_update(ActivityProgressEvent e) {
        activities_[e.id].progress = e.progress;
    }
    
    // Read at render rate (60/sec)
    const ActivityInfo* get_activity(int64_t id) const {
        return &activities_[id];
    }
};
```

The state model is the **impedance matcher** between fast events and slow
rendering. It maintains the ground truth without concerning itself with display.

### Layer 2: View Generators (Stateful Rendering)

View generators are coroutines that sample the state model at display rate and
transform it into visual output. Critically, they maintain *local rendering
state* - smoothing buffers, animation timelines, history - separate from the
domain state:

```cpp
coro::generator<Raster> download_section_view(
    const NixBuildState& state) 
{
    // Generator-local state: preserved across frames
    std::unordered_map<int64_t, float> smoothed_progress;
    std::unordered_map<int64_t, std::deque<float>> speed_history;
    
    while (true) {
        // Sample state at 60fps
        auto downloads = state.get_active_downloads();
        
        Raster section(80, downloads.size());
        
        for (size_t i = 0; i < downloads.size(); ++i) {
            const auto* activity = state.get_activity(downloads[i]);
            
            // Apply EMA smoothing in generator state
            float& smoothed = smoothed_progress[downloads[i]];
            smoothed = 0.9f * smoothed + 0.1f * activity->progress.percent;
            
            // Track speed history for sparklines
            speed_history[downloads[i]].push_back(activity->current_speed);
            
            render_progress_bar(section, i, activity, smoothed, 
                              speed_history[downloads[i]]);
        }
        
        co_yield section;  // Suspend, preserve state
    }
}
```

**Why generators?** Manual state machines for UI rendering quickly become
nightmarish:

```cpp
// Manual state machine - imagine debugging this
class ProgressAnimation {
    enum Phase { Intro, Active, FadeOut, Done };
    Phase phase_ = Intro;
    int frame_count_ = 0;
    float alpha_ = 0.0f;
    std::deque<float> history_;
    
    std::optional<Raster> render(Progress p) {
        switch (phase_) {
            case Intro:
                if (frame_count_++ < 10) {
                    return draw_intro(frame_count_ / 10.0f);
                }
                phase_ = Active;
                frame_count_ = 0;
                [[fallthrough]];
            
            case Active:
                history_.push_back(p.percent);
                if (p.finished) {
                    phase_ = FadeOut;
                    alpha_ = 1.0f;
                }
                return draw_active(history_);
            // ... etc
        }
    }
};
```

Versus the generator version:

```cpp
coro::generator<Raster> progress_animation(coro::queue<Progress>& updates) {
    // Intro
    for (int frame = 0; frame < 10; ++frame) {
        co_yield draw_intro(frame / 10.0f);
    }
    
    // Active
    std::deque<float> history;
    Progress p;
    do {
        p = co_await updates.pop();
        history.push_back(p.percent);
        co_yield draw_active(history);
    } while (!p.finished);
    
    // Fade out
    for (float alpha = 1.0f; alpha > 0.0f; alpha -= 0.1f) {
        co_yield draw_with_alpha(p, alpha);
    }
}
```

Natural control flow - loops, conditionals, sequential steps - with state
preservation implicit in the call stack. No manual phase tracking, no explicit
state variables for loop counters or transition conditions.

### Layer 3: The Compositor (Scheduled I/O)

The compositor runs at display rate, pulling frames from generators and emitting
ANSI codes:

```cpp
class TerminalCompositor {
    Raster screen_;
    Raster prev_screen_;
    
public:
    void present() {
        // Diff current vs previous frame
        for (const auto& run : diff_rasters(prev_screen_, screen_)) {
            // Emit ANSI only for changed regions
            ansi::move_cursor(run.y + 1, run.x + 1);
            
            if (run.fg_change)
                ansi::set_fg_color(run.fg_change->to_rgb());
            
            for (auto glyph : run.glyphs) {
                write_glyph(glyph);
            }
        }
        
        std::swap(prev_screen_, screen_);
    }
};

coro::task<void> compositor_loop(
    coro::io_scheduler& sched,
    TerminalCompositor& comp,
    coro::event& damage_event)
{
    while (true) {
        // Wait for damage OR frame timeout
        co_await when_any(
            damage_event.wait(),
            sched.sleep_for(16ms)  // Max 60fps
        );
        
        comp.present();
        
        if (shutdown_requested())
            break;
    }
}
```

The compositor is event-driven but rate-limited. If nothing changes, it sleeps.
If updates arrive faster than 60fps, it naturally coalesces them.

## The Wayland Connection: Buffers and Composition

The architecture mirrors Wayland's design philosophy:

**In Wayland:**

- Clients render into their own buffers
- Clients send buffer handles + positions to compositor
- Compositor assembles final screen, handles Z-order, alpha blending
- No intermediate compositing in client

**In our TUI:**

- Widgets (generators) render into their own raster buffers
- Widgets don't know global screen positions, just local coordinates
- Compositor flattens the widget tree, blits to screen positions
- Alpha blending happens once, at compositor level

### Why Not Blit Intermediate Buffers?

Early designs had a hierarchical structure where each container composited its
children's buffers:

```
ProgressWidget → render into 80x1 buffer
  ↓ blit
DownloadSection → render into 80x4 buffer  
  ↓ blit
HudCompositor → render into 80x6 buffer
  ↓ blit
Terminal → render into 80x24 buffer
  ↓ diff → ANSI
```

But this is wasteful! Each blit copies memory that will just be copied again.
Instead:

```cpp
// Compositor just tracks positions
struct ChildPlacement {
    Widget* widget;     // Points to widget's buffer
    Point position;     // Screen coordinates
};

// Flatten entire tree to screen positions
auto placed_widgets = flatten_hierarchy(root);

// Copy each buffer directly to final screen position (once!)
for (auto& [widget, pos] : placed_widgets) {
    if (widget->is_dirty()) {
        copy_to_screen(widget->buffer(), screen, pos);
    }
}
```

**Zero intermediate buffers.** Compositors are just layout managers, tracking
positions. Only leaf widgets allocate buffers.

## Pike's Insight: Channels and Multiplexing

Rob Pike's Plan 9 window system treated windows as file system resources with
channels for communication. Each window process communicated via channels -
reading mouse/keyboard events, writing drawing commands.

Our responsive layouts use the same multiplexing concept:

```cpp
// Three layout variants
auto compact_gen = compact_progress_layout();
auto medium_gen = medium_progress_layout();  
auto full_gen = full_progress_layout();

// Multiplexer routes updates based on width
struct ResponsiveLayout {
    coro::generator<Raster>* active_;
    
    Raster render(Progress p, int width) {
        // Select generator based on control signal (width)
        active_ = (width < 60)  ? &compact_gen :
                  (width < 100) ? &medium_gen : &full_gen;
        
        // Drive active generator
        return active_->send(p);
    }
};
```

This is Pike's message routing through a circuit, where the "control signal" (
terminal width) determines which processing path is active.

When width changes, we just switch which generator we're driving. The other
generators remain suspended with their state intact - no cancellation, no
restart, no cleanup.

## Responsive Design for Terminals

"Responsive design" usually refers to web UIs adapting to screen size. Terminals
need this too:

**Terminal viewport ranges:**

- Classic: 80×24
- Laptop: 100×30, 120×35
- Desktop: 140×45, 160×50
- Ultrawide: 200×60
- Tmux pane: 40×20

The same widget definition should work everywhere:

```cpp
struct ResponsiveProgressLayout {
    Raster render(const Progress& p, int width) {
        if (width < 60) {
            // Compact: "[lib] 50%"
            return render_compact(p);
        } else if (width < 100) {
            // Medium: "libx11 [████░░] 50%"
            return render_medium(p);
        } else {
            // Full: "libx11 [████░░] 50% 1M/s cache.nixos.org"
            return render_full(p);
        }
    }
};
```

CSS container queries for terminals - the widget asks "how wide is MY
container?" not "how wide is the terminal?"

### Terminal Scroll Regions: A Hidden Gem

ANSI terminals support scroll regions - a way to partition the screen into
independently scrolling sections:

```cpp
// Set scroll region to lines 1-40
ansi::set_scroll_region(1, 40);

// Printing at line 40 now scrolls lines 1-40 up
// Lines 41+ stay fixed!
```

This creates a natural boundary between flowing content (logs) and fixed UI (
status HUD):

```
┌─────────────────────────────────┐
│ Log output (scrolls)            │  Lines 1-40
│ copying path foo from cache     │  Terminal handles
│ building derivation bar         │  scroll automatically
│ ...                             │
├─────────────────────────────────┤  ← Border
│ libx11  [████░] 50% 1M/s       │  Lines 41-47
│ source  [██░░░] 20% 500KB      │  Fixed HUD
│ Status: 5/50 builds complete    │  Manually drawn
└─────────────────────────────────┘
```

The scroll region and fixed HUD are **completely independent** - different
compositors managing different regions, neither knowing about the other. The
terminal hardware does the work of keeping them separate.

## Memory Layout and Cache Efficiency

### SoA vs AoS for Raster Data

Terminal rasters can use Structure-of-Arrays (SoA):

```cpp
class Raster {
    std::vector<GlyphId> glyphs_;   // [g0, g1, g2, ..., gN]
    std::vector<Rgba8>   fgs_;      // [f0, f1, f2, ..., fN]
    std::vector<Rgba8>   bgs_;      // [b0, b1, b2, ..., bN]
};
```

Or Array-of-Structures (AoS):

```cpp
struct Cell {
    GlyphId glyph;  // 4 bytes
    Rgba8 fg;       // 4 bytes
    Rgba8 bg;       // 4 bytes
    float alpha;    // 4 bytes
};                  // 16 bytes total

class Raster {
    std::vector<Cell> cells_;
};
```

**For diffing (finding changed regions), SoA wins:**

```cpp
// SIMD-friendly scan for glyph changes
auto [front_g, back_g] = std::mismatch(
    front.glyphs_.begin() + start,
    front.glyphs_.begin() + end,
    back.glyphs_.begin() + start
);
// Linear scan, compiler can auto-vectorize
```

**For blitting (copying regions), AoS wins:**

```cpp
// Single cache line brings entire cell
Cell& src = src_cells[idx];
Cell& dst = dst_cells[idx];
dst = src;  // 16-byte copy, all fields together
```

**But there's a subtlety:** Terminal content has **run-length properties**. Most
cells share colors with neighbors:

```
"libx11  [████████░░] 50% 1024M"
// All "█" chars have same color (green)
// All " " chars have same color (white)
```

Finding these color runs is faster with SoA (vectorized search). For most TUIs,
diffing happens once per frame while blitting happens multiple times (at each
compositor level), so **AoS is probably the better default**, with the option to
add SoA caching for hot diff paths if profiling shows it matters.

## Alpha Blending and Composition

Supporting alpha transparency enables smooth fade-out animations and translucent
overlays:

```cpp
struct Cell {
    GlyphId glyph;
    Rgba8 fg, bg;
    float alpha;  // Per-cell alpha
};

class Widget {
    Raster buffer_;
    float global_alpha_ = 1.0f;  // Whole-widget alpha
};

// Effective alpha is product
float effective = cell.alpha * widget.global_alpha_;
```

This hybrid approach supports both:

- **Per-cell effects**: Gradient fades, sparkline dimming
- **Global widget fade**: Entire widget fading out when finished

Blending happens during composition using perceptually-uniform color spaces (
OKLCH) to avoid muddy colors:

```cpp
Rgba8 blend_oklch(Rgba8 fg, Rgba8 bg, float alpha) {
    // Convert RGB → OKLCH
    // Interpolate in OKLCH space
    // Convert back to RGB
    // Much better than naive RGB lerp
}
```

## Async vs Sync: Know Your Coroutine

C++20 conflates two distinct coroutine types:

**Generators (synchronous):**

- `co_yield` = pause, return value, wait for next call
- State preserved in suspended stack frame
- No scheduler, no I/O, pure control flow
- Used for: rendering, layout, stateful transformations

**Async coroutines (asynchronous):**

- `co_await` = suspend, wait for I/O or event
- Scheduled by event loop
- Actually waits for external conditions
- Used for: network I/O, timers, user input

```cpp
// Generator - sync, no waiting
coro::generator<Raster> layout(State s) {
    std::deque<float> history;  // Preserved!
    while (true) {
        history.push_back(s.value);
        co_yield render(history);  // Pause, don't wait
        s = co_await next_state(); // Resume point
    }
}

// Async task - actually waits
coro::task<void> network_fetch() {
    auto response = co_await http.get(url);  // Blocks on I/O
    co_return parse(response);
}
```

The architecture uses both:

- **Generators** for rendering (sync, stateful, no I/O)
- **Async tasks** for events, scheduling, terminal I/O

## Putting It Together: A Complete Example

Here's the full architecture for a Nix build monitor:

```cpp
// ============================================================================
// Layer 1: State Model
// ============================================================================

class NixBuildState {
    std::unordered_map<int64_t, ActivityInfo> activities_;
    std::unordered_map<int64_t, std::vector<int64_t>> children_;
    
public:
    // Updated at event rate (1000/sec)
    void on_event(LogEvent e) {
        std::visit([this](auto&& evt) {
            update_state(evt);
        }, e);
    }
    
    // Queried at render rate (60/sec)
    std::vector<int64_t> get_active_downloads() const;
    const ActivityInfo* get_activity(int64_t id) const;
};

// ============================================================================
// Layer 2: View Generator
// ============================================================================

coro::generator<void> build_hud_view(
    const NixBuildState& state,
    TerminalCompositor& comp,
    coro::channel<TermSize>& resizes)
{
    TermSize size = co_await resizes.recv();
    
    // Generator state: smoothing, history
    std::unordered_map<int64_t, float> smoothed_progress;
    std::unordered_map<int64_t, float> fade_alphas;
    
    while (true) {
        // Check resize
        if (auto new_size = resizes.try_recv()) {
            size = *new_size;
        }
        
        // Sample state
        auto downloads = state.get_active_downloads();
        
        // Render with smoothing
        Raster& buf = comp.back_buffer();
        buf.clear();
        
        for (size_t i = 0; i < downloads.size(); ++i) {
            auto* activity = state.get_activity(downloads[i]);
            
            // Smooth progress
            float& smoothed = smoothed_progress[downloads[i]];
            smoothed = 0.9f * smoothed + 0.1f * activity->progress;
            
            // Fade out finished items
            float& alpha = fade_alphas[downloads[i]];
            if (activity->finished) {
                alpha = std::max(0.0f, alpha - 0.05f);
            } else {
                alpha = 1.0f;
            }
            
            render_progress_bar(buf, i, activity, smoothed, alpha, size.width);
        }
        
        co_yield;  // Compositor will present
    }
}

// ============================================================================
// Layer 3: Compositor + Event Loop
// ============================================================================

coro::task<void> run_build_monitor(NixBuildState& state) {
    coro::io_scheduler sched;
    GlyphTable glyphs;
    TerminalCompositor comp(terminal_width(), terminal_height(), glyphs);
    
    coro::event damage_event;
    coro::channel<TermSize> resize_events;
    coro::queue<LogEvent> log_events;
    
    // Task 1: Ingest log events (fast)
    auto event_ingestion = [&]() -> coro::task<void> {
        while (auto event = co_await log_events.pop()) {
            state.on_event(*event);  // Update state, no rendering
        }
    };
    
    // Task 2: Drive view generator (60fps)
    auto view_driver = [&]() -> coro::task<void> {
        auto view_gen = build_hud_view(state, comp, resize_events);
        
        while (true) {
            view_gen.next();  // Sample state, render
            damage_event.set();  // Wake compositor
            co_await sched.yield_for(16ms);
        }
    };
    
    // Task 3: Compositor (event-driven, rate-limited)
    auto compositor = [&]() -> coro::task<void> {
        while (true) {
            co_await when_any(
                damage_event.wait(),
                sched.sleep_for(16ms)
            );
            
            comp.present();  // Diff and emit ANSI
            
            if (shutdown_requested()) break;
        }
    };
    
    // Run all concurrently
    co_await when_all(
        event_ingestion(),
        view_driver(),
        compositor()
    );
}
```

**Data flow:**

```
Nix stderr (1000 events/sec)
    ↓
NixBuildState (retained model)
    ↓ sampled at 60fps
View Generator (smoothing, animations)
    ↓ yield Raster
Compositor (diff, ANSI emit)
    ↓
Terminal
```

Three layers, three rates, clean separation.

## Lessons Learned

1. **Generators are not async** - they're synchronous, stateful functions. Use
   them for rendering logic where control flow as state is powerful.

2. **Separate model from view** - the state model absorbs fast events, view
   generators sample slowly. This impedance matching is critical for
   performance.

3. **Compositor !== renderer** - the compositor schedules and orchestrates, it
   doesn't need to know about your domain model (DOM, build state, whatever).

4. **Wayland got it right** - buffer handles + positions, compositor assembles.
   No intermediate compositing needed.

5. **Responsive design matters for terminals** - container queries, breakpoints,
   adaptive layouts - all relevant for TUIs.

6. **Manual state machines are tedious** - generators make complex UI state
   transitions tractable.

7. **Scroll regions are underused** - ANSI scroll regions create natural
   boundaries between flowing and fixed content.

## Conclusion

Building a responsive, efficient TUI requires thinking about the same problems
that windowing systems solved decades ago: how do you compose multiple
independent visual elements, each updating at their own rate, into a coherent
display? The answer involves careful layering - a retained-mode state model to
absorb events, stateful generators for rendering logic, and an async compositor
for scheduling and I/O.

The result is a framework where:

- Individual widgets are simple generators with natural control flow
- The state model and view are cleanly separated
- Performance scales gracefully from idle (0% CPU) to firehose (1000+
  events/sec)
- The architecture supports responsive layouts, animations, and complex
  compositions
- Terminal quirks (scroll regions, resize, SSH) are handled systematically

Whether you're building a package build monitor, an LLM agent interface, or any
other complex TUI, these patterns provide a solid foundation. The code may use
modern C++20 coroutines, but the ideas trace back to Pike's elegant
channel-based designs and the hard-won lessons of the Wayland protocol.