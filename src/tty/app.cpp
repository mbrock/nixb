#include "app.hpp"

#include "ansi.hpp"
#include "raster-diff.hpp"

#include <algorithm>
#include <atomic>
#include <coro/io_scheduler.hpp>
#include <csignal>
#include <iostream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace
{

coro::event g_shutdown_event{};
coro::event g_damage_event{};
coro::queue<nxb::ui::TermSize> g_resize_queue;
coro::io_scheduler *g_scheduler = nullptr;
std::atomic g_term_width{ 80 };
std::atomic g_term_height{ 24 };
std::atomic g_resize_requested{ false };
std::atomic g_shutdown_flag{ false };
std::atomic<std::uint64_t> g_damage_counter{ 0 };

void
refresh_terminal_size_internal () noexcept
{
  struct winsize ws{};
  if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0
      && ws.ws_row > 0)
    {
      g_term_width.store (ws.ws_col, std::memory_order_release);
      g_term_height.store (ws.ws_row, std::memory_order_release);
    }
}

void
signal_handler (const int signal)
{
  if (signal == SIGWINCH)
    {
      g_resize_requested.store (true, std::memory_order_release);
      return;
    }

  if (signal == SIGINT || signal == SIGTERM)
    {
      g_shutdown_flag.store (true, std::memory_order_release);
      g_shutdown_event.set ();
      if (g_scheduler != nullptr)
        {
          g_scheduler->shutdown ();
        }
    }
}

bool
consume_resize_request ()
{
  return g_resize_requested.exchange (false, std::memory_order_acq_rel);
}

} // namespace

namespace nxb::ui
{

coro::event &
shutdown_event ()
{
  return g_shutdown_event;
}

bool
shutdown_requested ()
{
  return g_shutdown_flag.load (std::memory_order_acquire);
}

coro::event &
damage_event ()
{
  return g_damage_event;
}

void
signal_damage ()
{
  g_damage_counter.fetch_add (1, std::memory_order_acq_rel);
  g_damage_event.set ();
}

coro::queue<TermSize> &
resize_channel ()
{
  return g_resize_queue;
}

int
terminal_width ()
{
  return g_term_width.load (std::memory_order_acquire);
}

int
terminal_height ()
{
  return g_term_height.load (std::memory_order_acquire);
}

TermSize
terminal_size ()
{
  return TermSize{ terminal_width (), terminal_height () };
}

TerminalGuard::TerminalGuard ()
{
  ansi::hide_cursor ();
  ansi::clear_screen ();
}

TerminalGuard::~TerminalGuard ()
{
  ansi::show_cursor ();
  ansi::clear_screen ();
  ansi::move_to (1, 1);
}

void
init_ui_runtime (coro::io_scheduler &scheduler)
{
  g_scheduler = &scheduler;
  std::signal (SIGINT, signal_handler);
  std::signal (SIGTERM, signal_handler);
  std::signal (SIGWINCH, signal_handler);
  refresh_terminal_size_internal ();
}

TerminalCompositor::TerminalCompositor (const int width, const int height,
                                        GlyphTable &glyphs)
    : front_ (std::max (width, 10), std::max (height, 5)),
      back_ (std::max (width, 10), std::max (height, 5)), glyphs_ (glyphs)
{
}

void
TerminalCompositor::resize (int width, int height)
{
  width = std::max (width, 10);
  height = std::max (height, 5);
  front_ = Raster (width, height);
  back_ = Raster (width, height);
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
  w.move_to (1, 1);

  for (const auto &[x, y, glyphs, fg_change, bg_change, fg_reset, bg_reset] :
       diff_rasters (front_, back_))
    {
      w.move_to (y + 1, x + 1);

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

  // Emit the accumulated ANSI to the output stream
  out.write (buf.data (), buf.size ());
  out.flush ();

  fmt::memory_buffer reset_buf;
  ansi::Writer reset_writer (reset_buf);
  reset_writer.reset ();
  out.write (reset_buf.data (), reset_buf.size ());

  std::swap (front_, back_);
}

coro::task<>
TerminalCompositor::present_loop (coro::io_scheduler &scheduler)
{
  co_await scheduler.schedule ();

  std::uint64_t handled_damage = 0;

  auto publish_size = [] (const TermSize size) -> coro::task<>
    {
      co_await resize_channel ().push (size);
      co_return;
    };

  co_await publish_size (terminal_size ());

  while (!shutdown_requested ())
    {
      if (consume_resize_request ())
        {
          refresh_terminal_size_internal ();
          const auto size = terminal_size ();
          resize (size.width, size.height);
          co_await publish_size (size);
        }

      const auto current_damage
          = g_damage_counter.load (std::memory_order_acquire);
      if (current_damage == handled_damage)
        {
          damage_event ().reset ();
          if (g_damage_counter.load (std::memory_order_acquire)
              == handled_damage)
            {
              co_await damage_event ();
            }
          continue;
        }

      handled_damage = current_damage;
      present_frame ();
    }

  co_return;
}

} // namespace nxb::ui
