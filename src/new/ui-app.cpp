#include "ui-app.hpp"

#include "ansi.hpp"
#include "tty-raster-diff.hpp"
#include "ui-dom.hpp"
#include "ui-layout.hpp"
#include "ui-paint.hpp"

#include <algorithm>
#include <atomic>
#include <coro/io_scheduler.hpp>
#include <csignal>
#include <fmt/core.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace
{

coro::event g_shutdown_event{};
coro::io_scheduler *g_scheduler = nullptr;
std::atomic<int> g_term_width{ 80 };
std::atomic<int> g_term_height{ 24 };
std::atomic<bool> g_resize_requested{ false };
std::atomic<bool> g_shutdown_flag{ false };

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
signal_handler (int signal)
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

coro::task<void>
render_loop_task (coro::io_scheduler &scheduler, Dom &dom,
                  nxb::GlyphTable &glyphs, LayoutEngine &layout,
                  Painter &painter, NodeId container_node)
{
  co_await scheduler.schedule ();

  auto make_raster = [] (int width, int height) -> Raster {
    width = std::max (width, 10);
    height = std::max (height, 5);
    return Raster (width, height);
  };

  int current_width = terminal_width ();
  int current_height = terminal_height ();
  Raster front = make_raster (current_width, current_height);
  Raster back = make_raster (current_width, current_height);

  while (true)
    {
      if (consume_resize_request ())
        {
          refresh_terminal_size_internal ();
          current_width = terminal_width ();
          current_height = terminal_height ();
          front = make_raster (current_width, current_height);
          back = make_raster (current_width, current_height);

          const auto &node = dom.get (container_node);
          if (auto *elem = std::get_if<Element> (&node.content))
            {
              Style updated = elem->style;
              updated.width = Size::fixed (current_width);
              updated.height = Size::fixed (current_height);
              dom.update_style (container_node, updated);
            }
        }

      if (dom.is_dirty ())
        {
          layout.compute (dom, current_width, current_height);
        }

      painter.paint (dom, back, glyphs);

      fmt::memory_buffer buf;
      ansi::Writer w (buf);
      w.move_to (1, 1);

      for (const auto &run : diff_rasters (front, back))
        {
          w.move_to (run.y + 1, run.x + 1);

          if (run.bg_reset)
            w.bg_default ();
          else if (run.bg_change)
            w.bg (run.bg_change->to_rgb ());

          if (run.fg_reset)
            w.fg_default ();
          else if (run.fg_change)
            w.fg (run.fg_change->to_rgb ());

          for (auto gid : run.glyphs)
            {
              if (auto text = glyphs.get (gid))
                w.text (*text);
            }
        }

      fmt::print ("{}", fmt::to_string (buf));
      std::fflush (stdout);

      fmt::memory_buffer reset_buf;
      ansi::Writer reset_writer (reset_buf);
      reset_writer.reset ();
      fmt::print ("{}", fmt::to_string (reset_buf));

      std::swap (front, back);

      co_await scheduler.yield_for (std::chrono::milliseconds (16));

      if (g_shutdown_event.is_set ())
        break;
    }

  co_return;
}

} // namespace nxb::ui
