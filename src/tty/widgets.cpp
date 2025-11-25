#include "widgets.hpp"

#include <algorithm>
#include <cmath>

#include <fmt/core.h>

namespace nxb::ui
{

coro::task<>
progress_bar (coro::io_scheduler &scheduler, Dom &dom, const NodeId parent,
              std::string label_text, const int bar_width,
              coro::queue<ProgressUpdate> &updates)
{
  co_await scheduler.schedule ();
  // Create nodes BEFORE first suspension - this affects coroutine frame layout
  Style row_style = Style::defaults ();
  row_style.flex_dir = FlexDir::Row;
  row_style.align = Align::Center;
  row_style.justify = Justify::Start;
  Node row (dom, dom.create_element (row_style));
  dom.append_child (parent, row.id ());

  Node label (dom, dom.create_text (fmt::format ("{:<20}", label_text),
                                    fmt::color::white));
  dom.append_child (row.id (), label.id ());

  Style bar_container_style = Style::defaults ();
  bar_container_style.flex_dir = FlexDir::Row;
  bar_container_style.width = Size::fixed (bar_width);
  bar_container_style.height = Size::fixed (1);
  bar_container_style.bg_color = Rgba8 (fmt::color::cyan, 100);
  Node bar_container (dom, dom.create_element (bar_container_style));
  dom.append_child (row.id (), bar_container.id ());

  Style fill_style = Style::defaults ();
  fill_style.flex_dir = FlexDir::Row;
  fill_style.width = Size::fixed (0);
  fill_style.height = Size::fixed (1);
  fill_style.bg_color = Rgba8 (fmt::color::green, 100);
  Node bar_fill (dom, dom.create_element (fill_style));
  dom.append_child (bar_container.id (), bar_fill.id ());

  Node percent (dom, dom.create_text ("  0%", fmt::color::white));
  dom.append_child (row.id (), percent.id ());

  // Animation loop
  float current = 0.0f;

  while (true)
    {
      auto update_result = co_await updates.pop ();
      if (!update_result)
        break;

      auto [fraction, new_label, finished] = *update_result;
      float target = fraction;

      while (current < target)
        {
          current = std::min (current + 0.05f, target);

          dom.update_text (label.id (), fmt::format ("{:<20}", new_label));
          dom.update_text (
              percent.id (),
              fmt::format ("{:>5.0f}%", std::round (current * 100)));

          const int filled_cells = std::clamp (
              static_cast<int> (current * bar_width), 0, bar_width);

          const auto &fill_node = dom.get (bar_fill.id ());
          if (auto *elem = std::get_if<Element> (&fill_node.content))
            {
              Style new_style = elem->style;
              new_style.width = Size::fixed (filled_cells);
              new_style.fg_color
                  = Rgba8 (finished ? fmt::color::cyan : fmt::color::green);
              dom.update_style (bar_fill.id (), new_style);
            }

          co_await scheduler.yield_for (std::chrono::milliseconds (16));
        }

      if (finished)
        break;
    }

  // When coroutine ends, all Node locals are destroyed, removing them from DOM
  co_return;
}

} // namespace nxb::ui
