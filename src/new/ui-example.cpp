#include "ui-dom.hpp"
#include "ui-layout.hpp"
#include "ui-paint.hpp"
#include "ui-widgets.hpp"

#include "ansi.hpp"
#include "tty-raster-diff.hpp"

#include <coro/event.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/sync_wait.hpp>
#include <coro/when_all.hpp>
#include <fmt/core.h>

#include <csignal>

using namespace nxb;
using namespace nxb::ui;

// Global event for signal handling
static coro::event g_shutdown_event;
static coro::io_scheduler *g_scheduler = nullptr;

static void
signal_handler (int signal)
{
  if (signal == SIGINT || signal == SIGTERM)
    {
      g_shutdown_event.set ();
    }
}

/// Render loop coroutine (no captures!)
coro::task<void>
render_loop_task (coro::io_scheduler &scheduler, Dom &dom, Raster &front,
                  Raster &back, GlyphTable &glyphs, LayoutEngine &layout,
                  Painter &painter)
{
  co_await scheduler.schedule ();

  // Render loop runs continuously until told to stop
  bool running = true;
  while (running)
    {
      // Check if DOM needs layout
      if (dom.is_dirty ())
        {
          layout.compute (dom, 80, 10);
        }

      // Paint to back buffer
      painter.paint (dom, back, glyphs);

      // Emit diff to terminal
      fmt::memory_buffer buf;
      ansi::Writer w (buf);

      // Move to top-left
      w.move_to (1, 1);

      // Diff and emit changes
      for (const auto &run : diff_rasters (front, back))
        {
          w.move_to (run.y + 1, run.x + 1);

          // Emit glyphs
          for (auto gid : run.glyphs)
            {
              if (auto text = glyphs.get (gid))
                {
                  w.text (*text);
                }
            }
        }

      fmt::print ("{}", fmt::to_string (buf));
      std::fflush (stdout);

      // Swap buffers
      std::swap (front, back);

      // Wait for next frame (16ms ~= 60fps)
      co_await scheduler.yield_for (std::chrono::milliseconds (16));

      // Check if we should stop (non-blocking check after each frame)
      if (g_shutdown_event.is_set ())
        running = false;
    }

  co_return;
}

/// Shutdown watcher - suspends until signal, then notifies queues
coro::task<void>
shutdown_watcher_task (coro::queue<ProgressState> &bar1_updates,
                       coro::queue<ProgressState> &bar2_updates)
{
  // Wait for shutdown event (suspends here!)
  co_await g_shutdown_event;

  // Signal received - shutdown queues to wake consumers
  co_await bar1_updates.shutdown ();
  co_await bar2_updates.shutdown ();

  co_return;
}

/// Simulation loop coroutine (no captures!)
coro::task<void>
simulation_task (coro::io_scheduler &scheduler,
                 coro::queue<ProgressState> &bar1_updates,
                 coro::queue<ProgressState> &bar2_updates)
{
  co_await scheduler.schedule ();

  // Simulate download 1
  for (int i = 0; i <= 100; i += 10)
    {
      co_await bar1_updates.push (ProgressState{
          .fraction = i / 100.0f,
          .label = "nixpkgs.tar.gz",
          .finished = (i == 100),
      });

      co_await scheduler.yield_for (std::chrono::milliseconds (100));
    }

  // Simulate download 2
  for (int i = 0; i <= 100; i += 5)
    {
      co_await bar2_updates.push (ProgressState{
          .fraction = i / 100.0f,
          .label = "rustc.tar.xz",
          .finished = (i == 100),
      });

      co_await scheduler.yield_for (std::chrono::milliseconds (80));
    }

  // Done - shutdown queues (unless already shutdown by signal)
  if (!g_shutdown_event.is_set ())
    {
      co_await bar1_updates.shutdown ();
      co_await bar2_updates.shutdown ();
    }

  co_return;
}

/// Example: Multi-progress bar HUD with concurrent widget coroutines
coro::task<void>
example_multi_progress_hud (coro::io_scheduler &scheduler)
{
  // Schedule this task onto the io_scheduler
  co_await scheduler.schedule ();

  // Create DOM
  Dom dom;

  // Build UI structure:
  // root
  //   container (flex-col)
  //     header (text)
  //     bar1 (text)
  //     bar2 (text)
  //     status (text)

  auto container = dom.create_element (Style{
      .flex_dir = FlexDir::Column,
      .width = Size::fixed (80),
      .height = Size::fixed (10),
      .bg_glyph = '.',
  });
  dom.append_child (dom.root (), container);

  // Header
  auto header = dom.create_text ("Build Progress:");
  dom.append_child (container, header);

  // Progress bars
  auto bar1 = dom.create_text ("");
  auto bar2 = dom.create_text ("");
  dom.append_child (container, bar1);
  dom.append_child (container, bar2);

  // Status text
  auto status = dom.create_text ("Initializing...");
  dom.append_child (container, status);

  // Create queues for communication
  coro::queue<ProgressState> bar1_updates;
  coro::queue<ProgressState> bar2_updates;

  // Spawn widget coroutines
  std::vector<coro::task<void>> widgets;
  widgets.push_back (progress_bar_widget (scheduler, dom, bar1, bar1_updates));
  widgets.push_back (progress_bar_widget (scheduler, dom, bar2, bar2_updates));

  // Create rasters for rendering
  Raster front (80, 10);
  Raster back (80, 10);
  GlyphTable glyphs;
  LayoutEngine layout;
  Painter painter;

  // Build task list (no lambda captures!)
  std::vector<coro::task<void>> all_tasks;
  all_tasks.push_back (
      render_loop_task (scheduler, dom, front, back, glyphs, layout, painter));
  all_tasks.push_back (
      simulation_task (scheduler, bar1_updates, bar2_updates));
  all_tasks.push_back (shutdown_watcher_task (bar1_updates, bar2_updates));
  for (auto &widget : widgets)
    {
      all_tasks.push_back (std::move (widget));
    }

  co_await coro::when_all (std::move (all_tasks));

  co_return;
}

int
main ()
{
  // Create io_scheduler
  auto scheduler = coro::io_scheduler::make_unique ();
  g_scheduler = scheduler.get ();

  // Install signal handlers
  std::signal (SIGINT, signal_handler);
  std::signal (SIGTERM, signal_handler);

  // RAII guard for terminal cleanup
  struct TerminalGuard
  {
    TerminalGuard ()
    {
      ansi::hide_cursor ();
      ansi::clear_screen ();
    }
    ~TerminalGuard ()
    {
      ansi::show_cursor ();
      ansi::clear_screen ();
      ansi::move_to (1, 1);
    }
  } guard;

  try
    {
      // Run example (pass scheduler by reference)
      coro::sync_wait (example_multi_progress_hud (*scheduler));

      if (g_shutdown_event.is_set ())
        {
          fmt::print ("Interrupted by user\n");
        }
      else
        {
          fmt::print ("Completed normally\n");
        }
    }
  catch (const std::exception &e)
    {
      fmt::print (stderr, "Error: {}\n", e.what ());
      return 1;
    }

  return 0;
}
