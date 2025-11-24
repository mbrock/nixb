#include "app.hpp"
#include "dom.hpp"
#include "layout.hpp"
#include "paint.hpp"
#include "widgets.hpp"

#include <chrono>
#include <coro/coro.hpp>
#include <coro/generator.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/queue.hpp>
#include <coro/sync_wait.hpp>
#include <coro/when_all.hpp>
#include <fmt/color.h>
#include <fmt/core.h>
#include <string>
#include <variant>
#include <vector>

namespace nxb::ui
{

namespace
{

using namespace std::chrono_literals;

coro::task<>
shutdown_watcher_task (coro::queue<ProgressState> &bar1_updates,
                       coro::queue<ProgressState> &bar2_updates)
{
  co_await shutdown_event ();
  co_await bar1_updates.shutdown ();
  co_await bar2_updates.shutdown ();
  co_return;
}

coro::task<>
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

coro::generator<std::monostate>
dom_view_generator (Dom &dom, LayoutEngine &layout, Painter &painter,
                    TerminalCompositor &compositor, const NodeId container,
                    const TermSize &size_state)
{
  // ReSharper disable once CppDFAEndlessLoop
  while (true)
    {
      const auto [width, height] = size_state;

      auto &container_node = dom.get_mut (container);
      if (const auto *elem = std::get_if<Element> (&container_node.content))
        {
          const bool width_changed
              = !elem->style.width.is_grow
                && elem->style.width.value != static_cast<std::size_t> (width);
          const bool height_changed
              = !elem->style.height.is_grow
                && elem->style.height.value
                       != static_cast<std::size_t> (height);
          if (width_changed || height_changed)
            {
              Style updated = elem->style;
              updated.width = Size::fixed (width);
              updated.height = Size::fixed (height);
              dom.update_style (container, updated);
            }
        }

      if (dom.is_dirty ())
        {
          layout.compute (dom, width, height);
        }

      auto &back_buffer = compositor.back_buffer ();
      back_buffer.clear ();
      painter.paint (dom, back_buffer, compositor.glyphs ());
      dom.mark_clean ();

      co_yield std::monostate{};
    }
}

coro::task<>
view_driver_task (coro::io_scheduler &scheduler, Dom &dom,
                  LayoutEngine &layout, Painter &painter,
                  TerminalCompositor &compositor, const NodeId container)
{
  co_await scheduler.schedule ();

  TermSize size_state = terminal_size ();
  auto generator = dom_view_generator (dom, layout, painter, compositor,
                                       container, size_state);
  auto iter = generator.begin ();
  const auto end = generator.end ();

  auto check_resize_queue = [&] () -> bool {
    bool resized = false;
    while (true)
      {
        auto value = resize_channel ().try_pop ();
        if (value.has_value ())
          {
            size_state = *value;
            resized = true;
            continue;
          }

        if (const auto err = value.error ();
            err == coro::queue_consume_result::empty
            || err == coro::queue_consume_result::try_lock_failure
            || err == coro::queue_consume_result::stopped)
          break;
      }
    return resized;
  };

  while (!shutdown_requested ())
    {
      const bool resized = check_resize_queue ();

      if (const bool dirty = dom.is_dirty () || resized; !dirty)
        {
          co_await scheduler.yield_for (16ms);
          continue;
        }

      if (iter == end)
        break;

      (void)*iter;
      ++iter;
      signal_damage ();
    }

  co_return;
}

ProgressBarNodes
make_progress_row (Dom &dom, const NodeId container, std::string label_text,
                   const fmt::color color, const int bar_width)
{
  Style row_style = Style::defaults ();
  row_style.flex_dir = FlexDir::Row;
  row_style.align = Align::Center;
  row_style.justify = Justify::Start;
  const NodeId row = dom.create_element (row_style);
  dom.append_child (container, row);

  const NodeId label
      = dom.create_text (fmt::format ("{:<20}", label_text), color);
  dom.append_child (row, label);

  Style bar_container_style = Style::defaults ();
  bar_container_style.flex_dir = FlexDir::Row;
  bar_container_style.width = Size::fixed (bar_width);
  bar_container_style.height = Size::fixed (1);
  bar_container_style.bg_glyph = '.';
  const NodeId bar_container = dom.create_element (bar_container_style);
  dom.append_child (row, bar_container);

  Style fill_style = Style::defaults ();
  fill_style.flex_dir = FlexDir::Row;
  fill_style.width = Size::fixed (0);
  fill_style.height = Size::fixed (1);
  fill_style.bg_glyph = '=';
  fill_style.fg_color = fmt::color::green;
  const NodeId bar_fill = dom.create_element (fill_style);
  dom.append_child (bar_container, bar_fill);

  const NodeId percent = dom.create_text ("  0%", fmt::color::white);
  dom.append_child (row, percent);

  return ProgressBarNodes{ .label = label,
                           .bar_fill = bar_fill,
                           .percent = percent,
                           .bar_width = bar_width };
}

int
run_progress_hud ()
{
  auto scheduler
      = coro::io_scheduler::make_shared (coro::io_scheduler::options{});
  init_ui_runtime (*scheduler);
  TerminalGuard guard;

  GlyphTable glyphs;
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

  constexpr int bar_width = 40;
  auto bar1_nodes = make_progress_row (dom, container, "nixpkgs.tar.gz",
                                       fmt::color::white, bar_width);
  auto bar2_nodes = make_progress_row (dom, container, "rustc.tar.xz",
                                       fmt::color::white, bar_width);

  auto status = dom.create_text ("Initializing...", fmt::color::yellow);
  dom.append_child (container, status);

  coro::queue<ProgressState> bar1_updates;
  coro::queue<ProgressState> bar2_updates;

  std::vector<coro::task<>> widgets;
  widgets.push_back (
      progress_bar_widget (*scheduler, dom, bar1_nodes, bar1_updates));
  widgets.push_back (
      progress_bar_widget (*scheduler, dom, bar2_nodes, bar2_updates));

  TerminalCompositor compositor (terminal_width (), terminal_height (),
                                 glyphs);

  std::vector<coro::task<>> all_tasks;
  all_tasks.push_back (compositor.present_loop (*scheduler));
  all_tasks.push_back (view_driver_task (*scheduler, dom, layout, painter,
                                         compositor, container));
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

} // namespace

} // namespace nxb::ui

int
main ()
{
  return nxb::ui::run_progress_hud ();
}
