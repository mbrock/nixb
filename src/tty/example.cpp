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
simulation_task (UIRuntime &runtime, coro::queue<ProgressState> &bar1_updates,
                 coro::queue<ProgressState> &bar2_updates)
{
  co_await runtime.scheduler ().schedule ();

  for (int i = 0; i <= 100; i += 10)
    {
      co_await bar1_updates.push (ProgressState{
          .fraction = i / 100.0f,
          .label = "nixpkgs.tar.gz",
          .finished = (i == 100),
      });
      co_await runtime.scheduler ().yield_for (
          std::chrono::milliseconds (100));
    }

  for (int i = 0; i <= 100; i += 5)
    {
      co_await bar2_updates.push (ProgressState{
          .fraction = i / 100.0f,
          .label = "rustc.tar.xz",
          .finished = (i == 100),
      });
      co_await runtime.scheduler ().yield_for (std::chrono::milliseconds (80));
    }

  co_await bar1_updates.shutdown_drain (
      runtime.scheduler ().shared_from_this ());
  co_await bar2_updates.shutdown_drain (
      runtime.scheduler ().shared_from_this ());

  if (!runtime.shutdown_requested ())
    runtime.request_shutdown ();

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
view_driver_task (UIRuntime &runtime, Dom &dom, LayoutEngine &layout,
                  Painter &painter, TerminalCompositor &compositor,
                  const NodeId container)
{
  co_await runtime.scheduler ().schedule ();

  TermSize size_state = runtime.terminal_size ();
  auto generator = dom_view_generator (dom, layout, painter, compositor,
                                       container, size_state);
  auto iter = generator.begin ();
  const auto end = generator.end ();

  auto check_resize_queue = [&] () -> bool
    {
      bool resized = false;
      while (true)
        {
          auto value = runtime.resize_channel ().try_pop ();
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

  while (!runtime.shutdown_requested ())
    {
      const bool resized = check_resize_queue ();

      if (const bool dirty = dom.is_dirty () || resized; !dirty)
        {
          co_await runtime.scheduler ().yield_for (16ms);
          continue;
        }

      if (iter == end)
        break;

      (void)*iter;
      ++iter;
      runtime.signal_damage ();
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
  bar_container_style.bg_color = Rgba8 (fmt::color::cyan, 100);
  const NodeId bar_container = dom.create_element (bar_container_style);
  dom.append_child (row, bar_container);

  Style fill_style = Style::defaults ();
  fill_style.flex_dir = FlexDir::Row;
  fill_style.width = Size::fixed (0);
  fill_style.height = Size::fixed (1);
  fill_style.bg_color = Rgba8 (fmt::color::green, 100);
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
  UIRuntime runtime (*scheduler);

  GlyphTable glyphs;
  LayoutEngine layout;
  Painter painter;
  Dom dom;

  Style container_style = Style::defaults ();
  container_style.flex_dir = FlexDir::Column;
  container_style.width = Size::fixed (runtime.terminal_width ());
  container_style.height = Size::fixed (runtime.terminal_height ());
  container_style.justify = Justify::Start;
  container_style.align = Align::Start;
  container_style.bg_color = fmt::color::dark_slate_blue;
  NodeId container = dom.create_element (container_style);
  dom.append_child (dom.root (), container);

  auto header = dom.create_text ("Progress:", fmt::color::white);
  dom.append_child (container, header);

  constexpr int bar_width = 40;
  auto bar1_nodes = make_progress_row (dom, container, "nixpkgs.tar.gz",
                                       fmt::color::white, bar_width);
  auto bar2_nodes = make_progress_row (dom, container, "rustc.tar.xz",
                                       fmt::color::white, bar_width);

  auto status = dom.create_text ("Building packages...", fmt::color::yellow);
  dom.append_child (container, status);

  coro::queue<ProgressState> bar1_updates;
  coro::queue<ProgressState> bar2_updates;

  std::vector<coro::task<>> widgets;
  widgets.push_back (
      progress_bar_widget (*scheduler, dom, bar1_nodes, bar1_updates));
  widgets.push_back (
      progress_bar_widget (*scheduler, dom, bar2_nodes, bar2_updates));

  TerminalCompositor compositor (runtime.terminal_width (),
                                 runtime.terminal_height (), glyphs);

  std::vector<coro::task<>> all_tasks;
  all_tasks.push_back (runtime.signal_loop ());
  all_tasks.push_back (compositor.present_loop (runtime));
  all_tasks.push_back (
      view_driver_task (runtime, dom, layout, painter, compositor, container));
  all_tasks.push_back (simulation_task (runtime, bar1_updates, bar2_updates));
  for (auto &widget : widgets)
    {
      all_tasks.push_back (std::move (widget));
    }

  try
    {
      TerminalGuard guard;
      coro::sync_wait (coro::when_all (std::move (all_tasks)));
    }
  catch (const std::exception &e)
    {
      fmt::print (stderr, "Error: {}\n", e.what ());
      return 1;
    }

  switch (runtime.shutdown_reason ())
    {
    case ShutdownReason::Running:
      assert (false);
      break;
    case ShutdownReason::Completed:
      fmt::print ("Completed normally\n");
      break;
    case ShutdownReason::Interrupted:
      fmt::print ("Interrupted by user\n");
      break;
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
