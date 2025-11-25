#pragma once

#include "glyph-table.hpp"
#include "raster.hpp"
#include "signal-pipe.hpp"
#include "tty/units.hpp"

#include <atomic>
#include <coro/coro.hpp>
#include <coro/event.hpp>
#include <coro/queue.hpp>
#include <coro/task.hpp>
#include <iosfwd>

namespace coro
{
class io_scheduler;
}

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

/// Runtime state for the UI system.
/// Encapsulates signal handling, terminal size tracking, and event
/// coordination.
class UIRuntime
{
public:
  explicit UIRuntime (coro::io_scheduler &scheduler);
  ~UIRuntime () = default;

  // Non-copyable, non-moveable (owns signal state)
  UIRuntime (const UIRuntime &) = delete;
  UIRuntime &operator= (const UIRuntime &) = delete;
  UIRuntime (UIRuntime &&) = delete;
  UIRuntime &operator= (UIRuntime &&) = delete;

  /// Access the scheduler.
  [[nodiscard]] coro::io_scheduler &
  scheduler () const noexcept
  {
    return *scheduler_;
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

  /// Coroutine that handles signals from the pipe.
  /// Should be run as part of the main task group.
  coro::task<> signal_loop ();

private:
  void refresh_terminal_size () noexcept;

  coro::io_scheduler *scheduler_;
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

} // namespace nxb::ui
