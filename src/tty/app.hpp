#pragma once

#include "glyph-table.hpp"
#include "raster.hpp"
#include "signal-pipe.hpp"
#include "tty/units.hpp"

#include <atomic>
#include <chrono>
#include <coro/coro.hpp>
#include <coro/event.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/queue.hpp>
#include <coro/task.hpp>
#include <iosfwd>

namespace nxb::ui
{

using TermSize = nxb::Size;

/// Reason for shutdown.
enum class ShutdownReason : std::uint8_t
{
  Running,    ///< Not shutting down
  Completed,  ///< Normal completion requested by application
  Interrupted ///< User interrupt (SIGINT/SIGTERM)
};

/// Guard that hides cursor/clears screen on scope entry, restores on exit.
struct TerminalGuard
{
  TerminalGuard ();
  ~TerminalGuard ();
};

// Forward declaration
class TerminalCompositor;

/// Runtime state for the UI system.
/// Owns scheduler, glyph table, compositor, and coordinates signals/events.
class UIRuntime
{
public:
  UIRuntime ();
  ~UIRuntime ();

  // Non-copyable, non-moveable (owns resources)
  UIRuntime (const UIRuntime &) = delete;
  UIRuntime &operator= (const UIRuntime &) = delete;
  UIRuntime (UIRuntime &&) = delete;
  UIRuntime &operator= (UIRuntime &&) = delete;

  /// Access the scheduler.
  [[nodiscard]] coro::io_scheduler &
  scheduler () noexcept
  {
    return *scheduler_;
  }

  /// Access the glyph table.
  [[nodiscard]] GlyphTable &
  glyphs () noexcept
  {
    return glyphs_;
  }

  /// Check if shutdown has been requested.
  [[nodiscard]] bool
  shutdown_requested () const noexcept
  {
    return shutdown_reason_.load (std::memory_order_acquire)
           != ShutdownReason::Running;
  }

  /// Get the reason for shutdown.
  [[nodiscard]] ShutdownReason
  shutdown_reason () const noexcept
  {
    return shutdown_reason_.load (std::memory_order_acquire);
  }

  /// Request graceful shutdown (normal completion).
  void request_shutdown ();

  /// Request shutdown due to signal (called from signal_loop).
  void request_interrupt ();

  /// Signal that the view has been damaged and needs redraw.
  void signal_damage ();

  /// Current terminal dimensions.
  [[nodiscard]] TermSize terminal_size () const noexcept;
  [[nodiscard]] width_t terminal_width () const noexcept;
  [[nodiscard]] height_t terminal_height () const noexcept;

  // =========================================================================
  // Render loop helpers
  // =========================================================================

  /// Wait for next frame. Handles resize, yields for frame_time.
  /// Returns false when shutdown is requested.
  /// Call this at the start of your render loop.
  coro::task<bool> next_frame (std::chrono::milliseconds frame_time);

  /// Render a layout to the screen.
  /// Clears back buffer, renders layout, presents frame.
  template <typename Layout>
  void
  render (const Layout &layout)
  {
    render_impl ([&layout] (RasterView &view, Size size)
                   { layout.render (view, size); });
  }

  /// Run a render loop until shutdown.
  /// BuildUI is called each frame to produce the layout.
  /// Note: pass by value to avoid dangling references in coroutine.
  template <typename BuildUI>
  coro::task<>
  run_render_loop (BuildUI build_ui, std::chrono::milliseconds frame_time)
  {
    co_await scheduler_->schedule ();

    while (co_await next_frame (frame_time))
      {
        render (build_ui ());
      }
  }

  /// Coroutine that handles signals from the pipe.
  /// Should be run as part of the main task group.
  coro::task<> signal_loop ();

  // =========================================================================
  // Low-level access (for advanced use)
  // =========================================================================

  /// Channel for resize notifications.
  coro::queue<TermSize> &
  resize_channel () noexcept
  {
    return resize_queue_;
  }

  /// Event signaled when damage occurs.
  coro::event &
  damage_event () noexcept
  {
    return damage_event_;
  }

  /// Direct access to compositor (for testing or advanced use).
  [[nodiscard]] TerminalCompositor &compositor () noexcept;

private:
  void refresh_terminal_size () noexcept;
  void render_impl (std::function<void (RasterView &, Size)> render_fn);

  std::shared_ptr<coro::io_scheduler> scheduler_;
  GlyphTable glyphs_;
  std::unique_ptr<TerminalCompositor> compositor_;
  SignalPipe signals_;

  coro::event damage_event_;
  coro::queue<TermSize> resize_queue_;

  std::atomic<nxb::width_t> term_width_{ 80 * ch };
  std::atomic<nxb::height_t> term_height_{ 24 * ln };
  std::atomic<ShutdownReason> shutdown_reason_{ ShutdownReason::Running };
  std::atomic<std::uint64_t> damage_counter_{ 0 };
};

class TerminalCompositor
{
public:
  TerminalCompositor (nxb::Size size, GlyphTable &glyphs);
  void resize (nxb::Size size);

  Raster &back_buffer () noexcept;
  GlyphTable &glyphs () const noexcept;
  nxb::Size size () const noexcept;

  /// Present loop that waits for damage events and renders frames.
  coro::task<> present_loop (UIRuntime &runtime);

  // Public for testing the rendering pipeline without async runtime
  void present_frame ();
  void present_frame (std::ostream &out);

private:
  Raster front_;
  Raster back_;
  GlyphTable &glyphs_;
};

// ============================================================================
// Convenient app runner
// ============================================================================

/// Run a TUI application.
/// - initial_state: the starting state
/// - build_ui: (const State&) → Layout
/// - update: (UIRuntime&, State&) → coro::task<> (should call
/// request_shutdown)
/// - frame_time: how long between frames (default 16ms ≈ 60fps)
template <typename State, typename BuildUI, typename Update>
int
run (State initial_state, BuildUI build_ui, Update update,
     std::chrono::milliseconds frame_time = std::chrono::milliseconds{ 16 })
{
  UIRuntime runtime;
  State state = std::move (initial_state);

  try
    {
      TerminalGuard guard;
      std::vector<coro::task<>> tasks;

      tasks.push_back (runtime.signal_loop ());

      tasks.push_back (runtime.run_render_loop (
          [&state, build_ui] { return build_ui (state); }, frame_time));

      auto task = [] (UIRuntime &runtime, State &state,
                      Update update) -> coro::task<>
        {
          co_await runtime.scheduler ().schedule ();
          co_await update (runtime, state);
          runtime.request_shutdown ();
        };

      tasks.push_back (task (runtime, state, update));

      coro::sync_wait (coro::when_all (std::move (tasks)));

      runtime.request_shutdown ();
    }
  catch (const std::exception &e)
    {
      fmt::print (stderr, "Error: {}\n", e.what ());
      return 1;
    }

  return 0;
}

} // namespace nxb::ui
