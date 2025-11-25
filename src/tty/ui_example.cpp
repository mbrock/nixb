#include "app.hpp"
#include "ui.hpp"

#include <chrono>
#include <coro/coro.hpp>
#include <coro/sync_wait.hpp>
#include <coro/when_all.hpp>
#include <fmt/core.h>

namespace nxb::ui2
{

// A progress bar using Row for layout!
struct ProgressBar
{
  // Use Row to handle layout automatically
  Row<Text, Box, Box, Text> ui = row (
      Text{ .content = "loading...",
            .color = Rgba8 (255, 255, 255),
            .width_hint = SizeHint::fixed_size (22) },
      Box{ .color = Rgba8 (0, 150, 0),
           .width_hint = SizeHint::fixed_size (0) },
      Box{ .color = Rgba8 (50, 50, 50), .width_hint = SizeHint::flex () },
      Text{ .content = "  0%",
            .color = Rgba8 (255, 255, 255),
            .width_hint = SizeHint::fixed_size (5) });

  Rect rect{};
  int bar_width = 40;

  // Accessors for the children
  Text &
  label ()
  {
    return ui.get<0> ();
  }

  Box &
  fill ()
  {
    return ui.get<1> ();
  }

  Box &
  bg ()
  {
    return ui.get<2> ();
  }

  Text &
  percent ()
  {
    return ui.get<3> ();
  }

  void
  update (const std::string &new_label, float progress, bool finished)
  {
    label ().content = fmt::format ("{:<20}", new_label);

    int filled = static_cast<int> (progress * bar_width);
    fill ().width_hint = SizeHint::fixed_size (filled);
    fill ().color = finished ? Rgba8 (0, 255, 255) : Rgba8 (0, 150, 0);

    bg ().width_hint = SizeHint::fixed_size (bar_width - filled);

    percent ().content = fmt::format ("{:>4.0f}%", progress * 100);
  }

  void
  layout (Rect container)
  {
    rect = container;
    ui.layout (container); // Row handles it!
  }

  void
  render (Raster &raster) const
  {
    ui.render (raster); // Row handles it!
  }

  Size
  preferred_size () const
  {
    return ui.preferred_size ();
  }

  SizeHint height_hint = SizeHint::fixed_size (1);
};

// The whole UI using Column for layout!
struct AppUI
{
  Column<Text, ProgressBar, ProgressBar, Text> ui = column (
      Text{ .content = "Progress:", .color = Rgba8 (255, 255, 255) },
      ProgressBar{}, ProgressBar{},
      Text{ .content = "Building packages...", .color = Rgba8 (255, 255, 0) });

  void
  init ()
  {
    ui.bg_color = Rgba8 (72, 61, 139); // dark slate blue
    bar1 ().update ("nixpkgs.tar.gz", 0.0f, false);
    bar2 ().update ("rustc.tar.xz", 0.0f, false);
  }

  // Accessors
  ProgressBar &
  bar1 ()
  {
    return ui.get<1> ();
  }

  ProgressBar &
  bar2 ()
  {
    return ui.get<2> ();
  }

  void
  do_layout (std::size_t width, std::size_t height)
  {
    ui.layout ({ 0, 0, width, height }); // Column handles it!
  }

  void
  render (Raster &raster) const
  {
    ui.render (raster); // Column handles it!
  }
};

namespace
{

using namespace std::chrono_literals;

coro::task<>
simulation_task (coro::io_scheduler &scheduler, AppUI &ui, bool &dirty,
                 bool &running)
{
  co_await scheduler.schedule ();

  // Animate bar 1
  for (int i = 0; i <= 100; i += 10)
    {
      ui.bar1 ().update ("nixpkgs.tar.gz", i / 100.0f, i == 100);
      dirty = true;
      co_await scheduler.yield_for (100ms);
    }

  // Animate bar 2
  for (int i = 0; i <= 100; i += 5)
    {
      ui.bar2 ().update ("rustc.tar.xz", i / 100.0f, i == 100);
      dirty = true;
      co_await scheduler.yield_for (80ms);
    }

  running = false;
  co_return;
}

coro::task<>
render_loop (coro::io_scheduler &scheduler, AppUI &ui,
             nxb::ui::TerminalCompositor &comp, int width, int height,
             bool &dirty, bool &running)
{
  co_await scheduler.schedule ();

  while (running)
    {
      if (dirty)
        {
          ui.do_layout (width, height);

          auto &buffer = comp.back_buffer ();
          buffer.clear ();
          ui.render (buffer);
          comp.present_frame ();

          dirty = false;
        }

      co_await scheduler.yield_for (16ms);
    }

  co_return;
}

int
run ()
{
  auto scheduler
      = coro::io_scheduler::make_shared (coro::io_scheduler::options{});

  nxb::ui::UIRuntime runtime (*scheduler);
  int width = runtime.terminal_width ();
  int height = runtime.terminal_height ();

  GlyphTable glyphs;
  nxb::ui::TerminalCompositor compositor (width, height, glyphs);

  AppUI ui;
  ui.init ();

  bool dirty = true;
  bool running = true;

  std::vector<coro::task<>> tasks;
  tasks.push_back (simulation_task (*scheduler, ui, dirty, running));
  tasks.push_back (
      render_loop (*scheduler, ui, compositor, width, height, dirty, running));

  try
    {
      nxb::ui::TerminalGuard guard;
      coro::sync_wait (coro::when_all (std::move (tasks)));
    }
  catch (const std::exception &e)
    {
      fmt::print (stderr, "Error: {}\n", e.what ());
      return 1;
    }

  fmt::print ("Completed!\n");
  return 0;
}

} // namespace

} // namespace nxb::ui2

int
main ()
{
  return nxb::ui2::run ();
}
