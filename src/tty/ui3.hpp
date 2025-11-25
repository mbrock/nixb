#pragma once

#include "raster.hpp"

#include <boost/hana.hpp>
#include <concepts>
#include <coro/coro.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace nxb::ui3
{

namespace hana = boost::hana;

// ============================================================================
// Fundamental Types
// ============================================================================

/// Simple size: width, height
struct Size
{
  std::size_t w = 0;
  std::size_t h = 0;
};

/// Placement in space: position + size
struct Placement
{
  std::size_t x = 0, y = 0;
  std::size_t w = 0, h = 0;
};

/// Size hint for flex layout (grow factor or fixed size)
struct SizeHint
{
  std::size_t fixed = 0; // Fixed size (0 = use content size)
  std::size_t grow = 0;  // Flex grow factor (0 = don't grow)

  static constexpr SizeHint
  content ()
  {
    return { 0, 0 };
  }

  static constexpr SizeHint
  fixed_size (std::size_t n)
  {
    return { n, 0 };
  }

  static constexpr SizeHint
  flex (std::size_t factor = 1)
  {
    return { 0, factor };
  }
};

// ============================================================================
// Core Concepts
// ============================================================================

/// A Layout describes spatial arrangement and how to render
/// Layouts are copyable and composable into trees (Row, Column)
/// May be pure values (TextLayout, BoxLayout) or copyable handles to
/// shared reactive state (Widget) - like P2300's split sender
template <typename L>
concept Layout = requires (const std::remove_cvref_t<L> &layout, Raster &raster, Size size) {
  { layout.preferred_size () } -> std::same_as<Size>;
  { layout.render (raster, size) } -> std::same_as<void>;
  requires std::copyable<std::remove_cvref_t<L>>;
};

/// A Reactive layout can be subscribed to - it updates over time
/// Calling run() creates a coroutine (operation state) that must be kept alive
/// The coroutine subscribes to state changes and updates the layout
template <typename L>
concept Reactive = Layout<L> && requires (const L &layout,
                                          coro::io_scheduler &sched,
                                          std::function<void ()> on_change) {
  { layout.run (sched, on_change) } -> std::same_as<coro::task<>>;
};

/// A View transforms state into a Layout (state → layout)
template <typename V, typename State>
concept View = requires (V view, State state) {
  { view (state) } -> Layout;
};

/// A Compositor manages rasters and handles rendering output
/// (analogous to P2300's execution resource / scheduler)
template <typename C>
concept Compositor = requires (C &comp) {
  { comp.back_buffer () } -> std::convertible_to<Raster &>;
  { comp.present_frame () } -> std::same_as<void>;
};

// ============================================================================
// Pure Layouts (Structural Descriptions)
// ============================================================================

/// Leaf: Text widget
struct TextLayout
{
  std::string content;
  Rgba8 color = Rgba8 (255, 255, 255);
  Rgba8 bg_color = Rgba8::transparent ();

  SizeHint width_hint = SizeHint::content ();
  SizeHint height_hint = SizeHint::content ();

  /// Count UTF-8 characters (not bytes) for display width
  static std::size_t
  utf8_display_width (std::string_view text)
  {
    std::size_t width = 0;
    std::size_t i = 0;
    while (i < text.size ())
      {
        unsigned char byte = static_cast<unsigned char> (text[i]);
        if ((byte & 0x80) == 0)
          i += 1; // ASCII
        else if ((byte & 0xE0) == 0xC0)
          i += 2; // 2-byte
        else if ((byte & 0xF0) == 0xE0)
          i += 3; // 3-byte
        else if ((byte & 0xF8) == 0xF0)
          i += 4; // 4-byte
        else
          i += 1; // Invalid, skip
        width++;
      }
    return width;
  }

  Size
  preferred_size () const
  {
    return { utf8_display_width (content), 1 };
  }

  void
  render (Raster &raster, Size) const
  {
    // Use write_text for proper UTF-8 handling
    raster.write_text (0, 0, content, color, bg_color);
  }
};

/// Leaf: Box (filled rectangle)
struct BoxLayout
{
  Rgba8 color = Rgba8 (100, 100, 100);

  SizeHint width_hint = SizeHint::flex ();
  SizeHint height_hint = SizeHint::content ();

  Size
  preferred_size () const
  {
    return { 0, 1 };
  }

  void
  render (Raster &raster, Size) const
  {
    for (std::size_t y = 0; y < raster.height (); ++y)
      {
        for (std::size_t x = 0; x < raster.width (); ++x)
          {
            raster.set_bg (x, y, color);
          }
      }
  }
};

// ============================================================================
// Layout Algorithms (Space Allocators)
// ============================================================================

/// Flexbox-style horizontal layout algorithm
struct FlexRowLayout
{
  template <typename... Children>
  static std::array<Placement, sizeof...(Children)>
  compute (Size container, const hana::tuple<Children...> &children)
  {
    constexpr std::size_t N = sizeof...(Children);
    std::array<Placement, N> placements{};
    std::array<std::size_t, N> widths{};

    // First pass: calculate fixed sizes and total grow
    std::size_t total_fixed = 0;
    std::size_t total_grow = 0;
    std::size_t idx = 0;

    hana::for_each (children,
                    [&] (const auto &child)
                      {
                        if (child.width_hint.fixed > 0)
                          {
                            widths[idx] = child.width_hint.fixed;
                            total_fixed += widths[idx];
                          }
                        else if (child.width_hint.grow > 0)
                          {
                            total_grow += child.width_hint.grow;
                          }
                        else
                          {
                            widths[idx] = child.preferred_size ().w;
                            total_fixed += widths[idx];
                          }
                        idx++;
                      });

    // Second pass: distribute remaining space
    std::size_t remaining
        = container.w > total_fixed ? container.w - total_fixed : 0;

    if (total_grow > 0)
      {
        idx = 0;
        hana::for_each (children,
                        [&] (const auto &child)
                          {
                            if (child.width_hint.grow > 0)
                              {
                                widths[idx]
                                    = (remaining * child.width_hint.grow)
                                      / total_grow;
                              }
                            idx++;
                          });
      }

    // Third pass: create placements
    std::size_t x = 0;
    for (idx = 0; idx < N; ++idx)
      {
        placements[idx] = { x, 0, widths[idx], container.h };
        x += widths[idx];
      }

    return placements;
  }
};

// ============================================================================
// Composite Layouts
// ============================================================================

/// Row - horizontal composition using FlexRowLayout
template <Layout... Children> struct RowLayout
{
  hana::tuple<Children...> children;
  Rgba8 bg_color = Rgba8::transparent ();

  SizeHint width_hint = SizeHint::content ();
  SizeHint height_hint = SizeHint::content ();

  static constexpr std::size_t child_count = sizeof...(Children);

  Size
  preferred_size () const
  {
    Size total{ 0, 0 };
    hana::for_each (children,
                    [&] (const auto &child)
                      {
                        auto pref = child.preferred_size ();
                        total.w += pref.w;
                        total.h = std::max (total.h, pref.h);
                      });
    return total;
  }

  void
  render (Raster &raster, Size allocated) const
  {
    // Fill background
    if (bg_color != Rgba8::transparent ())
      {
        for (std::size_t y = 0; y < raster.height (); ++y)
          for (std::size_t x = 0; x < raster.width (); ++x)
            raster.set_bg (x, y, bg_color);
      }

    // Compute placements using layout algorithm
    auto placements = FlexRowLayout::compute (allocated, children);

    // Render children to subrasters
    std::size_t idx = 0;
    hana::for_each (children,
                    [&] (const auto &child)
                      {
                        auto &p = placements[idx];
                        auto child_raster = raster.subraster (p.x, p.y, p.w, p.h);
                        child.render (child_raster, Size{ p.w, p.h });
                        idx++;
                      });
  }
};

/// Column - vertical composition
template <Layout... Children> struct ColumnLayout
{
  hana::tuple<Children...> children;
  Rgba8 bg_color = Rgba8::transparent ();

  SizeHint width_hint = SizeHint::content ();
  SizeHint height_hint = SizeHint::content ();

  static constexpr std::size_t child_count = sizeof...(Children);

  Size
  preferred_size () const
  {
    Size total{ 0, 0 };
    hana::for_each (children,
                    [&] (const auto &child)
                      {
                        auto pref = child.preferred_size ();
                        total.w = std::max (total.w, pref.w);
                        total.h += pref.h;
                      });
    return total;
  }

  void
  render (Raster &raster, Size allocated) const
  {
    // Fill background
    if (bg_color != Rgba8::transparent ())
      {
        for (std::size_t y = 0; y < raster.height (); ++y)
          for (std::size_t x = 0; x < raster.width (); ++x)
            raster.set_bg (x, y, bg_color);
      }

    // Simple vertical stacking - each child gets full width, preferred height
    std::size_t y = 0;
    hana::for_each (children,
                    [&] (const auto &child)
                      {
                        if (y >= allocated.h)
                          return;

                        auto pref = child.preferred_size ();
                        std::size_t h = std::min (pref.h, allocated.h - y);

                        auto child_raster = raster.subraster (0, y, allocated.w, h);
                        child.render (child_raster, Size{ allocated.w, h });

                        y += h;
                      });
  }
};

// ============================================================================
// Convenience Constructors
// ============================================================================

template <Layout... Children>
RowLayout<std::decay_t<Children>...>
row (Children &&...children)
{
  return { hana::make_tuple (std::forward<Children> (children)...) };
}

template <Layout... Children>
ColumnLayout<std::decay_t<Children>...>
column (Children &&...children)
{
  return { hana::make_tuple (std::forward<Children> (children)...) };
}

inline TextLayout
text (std::string content, Rgba8 color = Rgba8 (255, 255, 255),
      Rgba8 bg_color = Rgba8::transparent ())
{
  return TextLayout{ std::move (content), color, bg_color };
}

inline BoxLayout
box (Rgba8 color = Rgba8 (100, 100, 100))
{
  return BoxLayout{ color };
}

// ============================================================================
// State Senders (Time Dimension) - Observable Streams
// ============================================================================

/// Signal: observable value that can be updated
/// Wraps a coro::queue for reactive state updates
template <typename T> class Signal
{
public:
  Signal () : queue_ (std::make_shared<coro::queue<T>> ()) {}

  /// Get the underlying queue for subscription
  /// Subscribers can co_await queue->pop() to get updates
  std::shared_ptr<coro::queue<T>>
  queue () const
  {
    return queue_;
  }

  /// Update the signal value (sends to queue)
  coro::task<>
  set (T value)
  {
    co_await queue_->push (std::move (value));
  }

private:
  std::shared_ptr<coro::queue<T>> queue_;
};

// ============================================================================
// Reactive Layouts (Dynamic Layouts driven by Signals)
// ============================================================================

/// Widget: A reactive Layout that updates based on a signal
/// Can be composed into layout trees like any other Layout
/// Uses shared state (like P2300's split) to be copyable while maintaining
/// subscription state. Calling run() creates a coroutine that subscribes
/// to state updates - this coroutine is the operation state and must be kept alive
template <typename State, typename ViewFn> class Widget
{
private:
  struct SharedState
  {
    std::shared_ptr<coro::queue<State>> state_queue;
    ViewFn view_fn;
    std::mutex mutex;
    std::optional<decltype (view_fn (std::declval<State> ()))> current_layout;

    SharedState (std::shared_ptr<coro::queue<State>> q, ViewFn vf)
        : state_queue (std::move (q)), view_fn (std::move (vf)),
          current_layout (std::nullopt)
    {
    }
  };

  std::shared_ptr<SharedState> state_;

public:
  Widget (Signal<State> &signal, ViewFn view_fn)
      : state_ (std::make_shared<SharedState> (signal.queue (),
                                                std::move (view_fn)))
  {
  }

  SizeHint width_hint = SizeHint::content ();
  SizeHint height_hint = SizeHint::content ();

  Size
  preferred_size () const
  {
    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->current_layout)
      return state_->current_layout->preferred_size ();
    return Size{ 0, 0 };
  }

  void
  render (Raster &raster, Size allocated) const
  {
    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->current_layout)
      state_->current_layout->render (raster, allocated);
  }

  /// Subscribe to state updates (creates the operation state coroutine)
  /// This coroutine must be kept alive for the widget to update
  ///
  /// The coroutine:
  /// 1. Subscribes to the state queue
  /// 2. On each state update, computes new layout via view_fn
  /// 3. Updates shared current_layout
  /// 4. Calls on_change() callback to notify that re-render is needed
  ///
  /// The on_change callback is application-specific - typically sets a dirty flag
  /// The application's render loop then calls render_with_layout() as needed
  coro::task<>
  run (coro::io_scheduler &sched, std::function<void ()> on_change) const
  {
    co_await sched.schedule ();

    while (true)
      {
        auto result = co_await state_->state_queue->pop ();
        if (!result)
          break;

        const auto &state = *result;

        // Update current layout (operation state)
        {
          std::lock_guard<std::mutex> lock (state_->mutex);
          state_->current_layout = state_->view_fn (state);
        }

        // Notify application to re-render
        on_change ();

        co_await sched.yield ();
      }
  }
};

// ============================================================================
// Frame Scheduler (Request Animation Frame)
// ============================================================================

/// FrameScheduler batches render requests and executes them atomically
/// Uses type erasure to store heterogeneous layout types
class FrameScheduler
{
public:
  /// Type-erased render job interface
  struct RenderJob
  {
    virtual ~RenderJob () = default;
    virtual void render (Raster &buffer) = 0;
  };

  /// Concrete render job that owns a layout
  template <Layout L> struct TypedRenderJob : RenderJob
  {
    Placement placement;
    L layout;

    TypedRenderJob (Placement p, L l)
        : placement (p), layout (std::move (l))
    {
    }

    void
    render (Raster &buffer) override
    {
      auto sub_raster = buffer.subraster (placement.x, placement.y,
                                          placement.w, placement.h);
      layout.render (sub_raster, Size{ placement.w, placement.h });
    }
  };

  /// Request to render in the next frame (thread-safe)
  template <Layout L>
  void
  request_animation_frame (Placement placement, L layout)
  {
    std::lock_guard<std::mutex> lock (mutex_);
    pending_jobs_.push_back (
        std::make_unique<TypedRenderJob<L>> (placement, std::move (layout)));
    dirty_ = true;
  }

  /// Execute pending render requests (called by compositor, thread-safe)
  void
  execute_frame (Raster &buffer)
  {
    // Move jobs out of pending_jobs_ under lock, then render without lock
    std::vector<std::unique_ptr<RenderJob>> jobs_to_render;
    {
      std::lock_guard<std::mutex> lock (mutex_);
      jobs_to_render = std::move (pending_jobs_);
      pending_jobs_.clear ();
      dirty_ = false;
    }

    // Render outside the lock (rendering can be slow)
    for (auto &job : jobs_to_render)
      {
        job->render (buffer);
      }
  }

  bool
  is_dirty () const
  {
    std::lock_guard<std::mutex> lock (mutex_);
    return dirty_;
  }

private:
  std::vector<std::unique_ptr<RenderJob>> pending_jobs_;
  bool dirty_ = false;
  mutable std::mutex mutex_;
};

// ============================================================================
// Alternative Pattern: Direct FrameScheduler Subscription
// ============================================================================

/// Alternative to Widget::run() - directly submits layouts to FrameScheduler
/// Instead of calling on_change() callback, this pushes layouts directly
/// Use this pattern when you want centralized frame scheduling rather than
/// application-managed render loops
template <typename T, typename ViewFn>
coro::task<>
widget_loop (coro::io_scheduler &sched,
             std::shared_ptr<coro::queue<T>> state_queue, ViewFn view_fn,
             Placement placement, FrameScheduler &frame_scheduler)
{
  co_await sched.schedule ();

  // Pull state updates and submit render requests
  while (true)
    {
      // Pop next state from queue (async)
      auto result = co_await state_queue->pop ();
      if (!result)
        break; // Queue stopped

      const auto &state = *result;

      // Transform state → layout and submit directly
      auto layout = view_fn (state);

      // Submit layout to frame scheduler - it takes ownership
      frame_scheduler.request_animation_frame (placement, std::move (layout));

      // Yield to let other tasks run
      co_await sched.yield ();
    }

  co_return;
}

// ============================================================================
// View Adaptor (State → Layout Transform)
// ============================================================================

/// ViewedStream: combines a state queue with a view function
/// This is what you get from: signal.queue() | view(fn)
template <typename T, typename ViewFn> struct ViewedStream
{
  std::shared_ptr<coro::queue<T>> state_queue;
  ViewFn view_fn;

  /// Start the widget loop for this view
  coro::task<>
  start (coro::io_scheduler &sched, Placement placement,
         FrameScheduler &frame_scheduler)
  {
    return widget_loop (sched, state_queue, view_fn, placement,
                        frame_scheduler);
  }
};

/// Pipe operator: queue<State> | view_function → ViewedStream
template <typename T, typename ViewFn>
ViewedStream<T, ViewFn>
operator| (std::shared_ptr<coro::queue<T>> queue, ViewFn &&view_fn)
{
  return { std::move (queue), std::forward<ViewFn> (view_fn) };
}

} // namespace nxb::ui3
