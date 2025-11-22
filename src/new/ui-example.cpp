#include "ui-app.hpp"
#include "ui-dom.hpp"
#include "ui-layout.hpp"
#include "ui-paint.hpp"
#include "ui-widgets.hpp"

#include <chrono>
#include <cmath>
#include <coro/io_scheduler.hpp>
#include <coro/queue.hpp>
#include <coro/sync_wait.hpp>
#include <coro/when_all.hpp>
#include <fmt/color.h>
#include <fmt/core.h>
#include <string>
#include <vector>

namespace nxb::ui
{

coro::task<void>
shutdown_watcher_task (coro::queue<ProgressState> &bar1_updates,
                       coro::queue<ProgressState> &bar2_updates)
{
  co_await shutdown_event ();
  co_await bar1_updates.shutdown ();
  co_await bar2_updates.shutdown ();
  co_return;
}

coro::task<void>
simulation_task (coro::io_scheduler &scheduler,
                 coro::queue<ProgressState> &bar1_updates,
                 coro::queue<ProgressState> &bar2_updates)
{
  co_await scheduler.schedule ();

  for (int i = 0; i <= 100; i += 10)
    {
      co_await bar1_updates.push (ProgressState{
          .fraction = i / 100.0f,
          .label = "nixpkgs.tar.gz",
          .finished = (i == 100),
      });
      co_await scheduler.yield_for (std::chrono::milliseconds (100));
    }

  for (int i = 0; i <= 100; i += 5)
    {
      co_await bar2_updates.push (ProgressState{
          .fraction = i / 100.0f,
          .label = "rustc.tar.xz",
          .finished = (i == 100),
      });
      co_await scheduler.yield_for (std::chrono::milliseconds (80));
    }

  if (!shutdown_requested ())
    {
      co_await bar1_updates.shutdown ();
      co_await bar2_updates.shutdown ();
    }

  co_return;
}

static ProgressBarNodes
make_progress_row (Dom &dom, NodeId container, std::string label_text,
                   fmt::color color, int bar_width)
{
  Style row_style = Style::defaults ();
  row_style.flex_dir = FlexDir::Row;
  row_style.align = Align::Center;
  row_style.justify = Justify::Start;
  NodeId row = dom.create_element (row_style);
  dom.append_child (container, row);

  NodeId label = dom.create_text (fmt::format ("{:<20}", label_text), color);
  dom.append_child (row, label);

  Style bar_container_style = Style::defaults ();
  bar_container_style.flex_dir = FlexDir::Row;
  bar_container_style.width = Size::fixed (bar_width);
  bar_container_style.height = Size::fixed (1);
  bar_container_style.bg_glyph = '.';
  NodeId bar_container = dom.create_element (bar_container_style);
  dom.append_child (row, bar_container);

  Style fill_style = Style::defaults ();
  fill_style.flex_dir = FlexDir::Row;
  fill_style.width = Size::fixed (0);
  fill_style.height = Size::fixed (1);
  fill_style.bg_glyph = '=';
  fill_style.fg_color = fmt::color::green;
  NodeId bar_fill = dom.create_element (fill_style);
  dom.append_child (bar_container, bar_fill);

  NodeId percent = dom.create_text ("  0%", fmt::color::white);
  dom.append_child (row, percent);

  return ProgressBarNodes{ .label = label,
                           .bar_fill = bar_fill,
                           .percent = percent,
                           .bar_width = bar_width };
}

int
run_progress_hud ()
{
  auto scheduler = coro::io_scheduler::make_unique ();
  init_ui_runtime (*scheduler);
  TerminalGuard guard;

  nxb::GlyphTable glyphs;
  LayoutEngine layout;
  Painter painter;
  Dom dom;

  Style container_style = Style::defaults ();
  container_style.flex_dir = FlexDir::Column;
  container_style.width = Size::fixed (terminal_width ());
  container_style.height = Size::fixed (terminal_height ());
  container_style.justify = Justify::Start;
  container_style.align = Align::Start;
  container_style.bg_glyph = '.';
  NodeId container = dom.create_element (container_style);
  dom.append_child (dom.root (), container);

  auto header = dom.create_text ("Build Progress:", fmt::color::white);
  dom.append_child (container, header);

  const int bar_width = 40;
  auto bar1_nodes = make_progress_row (dom, container, "nixpkgs.tar.gz",
                                       fmt::color::white, bar_width);
  auto bar2_nodes = make_progress_row (dom, container, "rustc.tar.xz",
                                       fmt::color::white, bar_width);

  auto status = dom.create_text ("Initializing...", fmt::color::yellow);
  dom.append_child (container, status);

  coro::queue<ProgressState> bar1_updates;
  coro::queue<ProgressState> bar2_updates;

  std::vector<coro::task<void>> widgets;
  widgets.push_back (
      progress_bar_widget (*scheduler, dom, bar1_nodes, bar1_updates));
  widgets.push_back (
      progress_bar_widget (*scheduler, dom, bar2_nodes, bar2_updates));

  std::vector<coro::task<void>> all_tasks;
  all_tasks.push_back (
      render_loop_task (*scheduler, dom, glyphs, layout, painter, container));
  all_tasks.push_back (
      simulation_task (*scheduler, bar1_updates, bar2_updates));
  all_tasks.push_back (shutdown_watcher_task (bar1_updates, bar2_updates));
  for (auto &widget : widgets)
    {
      all_tasks.push_back (std::move (widget));
    }

  try
    {
      coro::sync_wait (coro::when_all (std::move (all_tasks)));

      if (shutdown_requested ())
        fmt::print ("Interrupted by user\n");
      else
        fmt::print ("Completed normally\n");
    }
  catch (const std::exception &e)
    {
      fmt::print (stderr, "Error: {}\n", e.what ());
      return 1;
    }

  return 0;
}

} // namespace nxb::ui

int
main ()
{
  return nxb::ui::run_progress_hud ();
}
