#include "app.hpp"
#include "ansi.hpp"
#include "raster-diff.hpp"
#include "units.hpp"

#include <algorithm>
#include <coro/io_scheduler.hpp>
#include <csignal>
#include <iostream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace nxb::ui
{

TerminalGuard::TerminalGuard ()
{
  ansi::hide_cursor ();
  ansi::clear_screen ();
}

TerminalGuard::~TerminalGuard ()
{
  ansi::show_cursor ();
  ansi::clear_screen ();
  ansi::move_to (Pos::origin ());
  std::cout << "\033[0m" << std::flush; // Reset SGR
}

UIRuntime::UIRuntime (coro::io_scheduler &scheduler) : scheduler_ (&scheduler)
{
  signals_.watch (SIGINT, SIGTERM, SIGWINCH);
  refresh_terminal_size ();
}

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

TerminalCompositor::TerminalCompositor (const nxb::Size size,
                                        GlyphTable &glyphs)
    : front_ (size.w, size.h, glyphs), back_ (size.w, size.h, glyphs),
      glyphs_ (glyphs)
{
}

void
TerminalCompositor::resize (nxb::Size size)
{
  front_ = Raster (size.w, size.h, glyphs_);
  back_ = Raster (size.w, size.h, glyphs_);
  ansi::clear_screen ();
}

Raster &
TerminalCompositor::back_buffer () noexcept
{
  return back_;
}

GlyphTable &
TerminalCompositor::glyphs () const noexcept
{
  return glyphs_;
}

void
TerminalCompositor::present_frame ()
{
  present_frame (std::cout);
}

void
TerminalCompositor::present_frame (std::ostream &out)
{
  fmt::memory_buffer buf;
  ansi::Writer w (buf);
  w.move_to (Pos::origin ());

  for (const auto &[pos, glyphs, fg_change, bg_change, fg_reset, bg_reset] :
       diff_rasters (front_, back_))
    {
      w.move_to (pos);

      if (bg_reset)
        w.bg_default ();
      else if (bg_change)
        w.bg (bg_change->to_rgb ());

      if (fg_reset)
        w.fg_default ();
      else if (fg_change)
        w.fg (fg_change->to_rgb ());

      for (const auto gid : glyphs)
        {
          if (auto text = glyphs_.get (gid))
            w.text (*text);
        }
    }

  out.write (buf.data (), static_cast<std::streamsize> (buf.size ()));
  out.flush ();

  std::swap (front_, back_);

  back_ = front_;
}

coro::task<>
TerminalCompositor::present_loop (UIRuntime &runtime)
{
  co_await runtime.scheduler ().schedule ();
  co_await runtime.resize_channel ().push (runtime.terminal_size ());

  while (!runtime.shutdown_requested ())
    {
      // Check for resize events
      while (true)
        {
          auto value = runtime.resize_channel ().try_pop ();
          if (value.has_value ())
            {
              resize (value.value ());
              continue;
            }
          break;
        }

      // Wait for damage
      runtime.damage_event ().reset ();
      co_await runtime.damage_event ();

      if (runtime.shutdown_requested ())
        break;

      present_frame ();
    }

  co_return;
}

} // namespace nxb::ui
