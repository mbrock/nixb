#include "app.hpp"
#include "ansi.hpp"
#include "units.hpp"

#include <coro/io_scheduler.hpp>
#include <csignal>
#include <iostream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace nxb::ui
{

UIRuntime::UIRuntime ()
    : scheduler_ (
          coro::io_scheduler::make_shared (coro::io_scheduler::options{}))
{
  signals_.watch (SIGINT, SIGTERM, SIGWINCH);
  refresh_terminal_size ();

  // Create compositor with initial terminal size
  compositor_
      = std::make_unique<TerminalCompositor> (terminal_size (), glyphs_);
}

UIRuntime::~UIRuntime () = default;

void
UIRuntime::refresh_terminal_size () noexcept
{
  struct winsize ws{};
  if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0
      && ws.ws_row > 0)
    {
      term_width_.store (ws.ws_col * ch, std::memory_order_release);
      term_height_.store (ws.ws_row * ln, std::memory_order_release);
    }
}

void
UIRuntime::request_shutdown ()
{
  auto expected = ShutdownReason::Running;
  if (!shutdown_reason_.compare_exchange_strong (
          expected, ShutdownReason::Completed, std::memory_order_acq_rel))
    return; // Already shutting down

  damage_event_.set (); // Wake present_loop
  // Write to signal pipe to wake signal_loop
  SignalPipe::notify (0); // Signal 0 = shutdown request
}

void
UIRuntime::request_interrupt ()
{
  auto expected = ShutdownReason::Running;
  if (!shutdown_reason_.compare_exchange_strong (
          expected, ShutdownReason::Interrupted, std::memory_order_acq_rel))
    return; // Already shutting down

  damage_event_.set ();

  stop_source_.request_stop ();
}

void
UIRuntime::signal_damage ()
{
  damage_counter_.fetch_add (1, std::memory_order_acq_rel);
  damage_event_.set ();
}

TermSize
UIRuntime::terminal_size () const noexcept
{
  return TermSize{ terminal_width (), terminal_height () };
}

nxb::width_t
UIRuntime::terminal_width () const noexcept
{
  return term_width_.load (std::memory_order_acquire);
}

nxb::height_t
UIRuntime::terminal_height () const noexcept
{
  return term_height_.load (std::memory_order_acquire);
}

coro::task<bool>
UIRuntime::next_frame (std::chrono::milliseconds frame_time)
{
  if (shutdown_requested ())
    co_return false;

  // Process any pending resize events
  while (auto sz = resize_queue_.try_pop ())
    compositor_->resize (sz.value ());

  // Yield for frame time
  co_await scheduler_->yield_for (frame_time);

  co_return !shutdown_requested ();
}

void
UIRuntime::render_impl (std::function<void (RasterView &, Size)> render_fn)
{
  auto &buffer = compositor_->back_buffer ();
  buffer.clear ();
  auto view = buffer.view ();
  auto size = compositor_->size ();
  render_fn (view, size);
  compositor_->present_frame ();
}

void
UIRuntime::update_hud_height (height_t hud_h)
{
  compositor_->set_hud_height (hud_h, terminal_height ());
}

void
UIRuntime::println (std::string_view line)
{
  auto hud_h = compositor_->hud_height ();
  auto term_h = terminal_height ();

  // No scroll region in full-screen mode
  if (hud_h >= term_h)
    return;

  fmt::memory_buffer buf;
  ansi::Writer w (buf);

  // Move to bottom of scroll region (one row above HUD) and print with
  // trailing newline. The newline causes the scroll region to scroll up.
  // Reset SGR first to avoid HUD styling leaking into log output.
  auto scroll_bottom = terminal_origin_v + (term_h - hud_h) - 1 * ln;
  w.move_to (Pos{ terminal_origin + 0 * ch, scroll_bottom });
  w.reset ();
  w.text (line);
  //  w.text (fmt::format ("{}", hud_h.numerical_value_in (ln)));
  w.clear_line_from_cursor ();
  w.text ("\n");

  std::cout.write (buf.data (), static_cast<std::streamsize> (buf.size ()));
  std::cout.flush ();
}

TerminalCompositor &
UIRuntime::compositor () noexcept
{
  return *compositor_;
}

coro::task<>
UIRuntime::signal_loop ()
{
  co_await scheduler_->schedule ();

  while (!shutdown_requested ())
    {
      // Poll the signal pipe for readability
      co_await scheduler_->poll (signals_.read_fd (), coro::poll_op::read);

      // Drain all pending signals
      while (auto sig = signals_.try_read ())
        {
          switch (*sig)
            {
            case 0: // Internal shutdown request (normal completion)
              signal_damage ();
              co_return;

            case SIGINT:
            case SIGTERM:
              request_interrupt ();
              co_return;

            case SIGWINCH:
              refresh_terminal_size ();
              co_await resize_queue_.push (terminal_size ());
              signal_damage ();
              break;

            default:
              break;
            }
        }
    }

  co_return;
}

coro::task<>
UIRuntime::present_loop ()
{
  co_await scheduler_->schedule ();
  co_await resize_queue_.push (terminal_size ());

  while (!shutdown_requested ())
    {
      // Check for resize events
      while (auto value = resize_queue_.try_pop ())
        compositor_->resize (*value);

      // Wait for damage
      damage_event_.reset ();
      co_await damage_event_;

      if (shutdown_requested ())
        break;

      compositor_->present_frame ();
    }

  co_return;
}

} // namespace nxb::ui
