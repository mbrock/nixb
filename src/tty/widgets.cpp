#include "widgets.hpp"

#include <algorithm>
#include <cmath>

#include <coro/when_all.hpp>
#include <fmt/core.h>

namespace nxb::ui
{

coro::task<>
progress_bar_widget (coro::io_scheduler &scheduler, Dom &dom,
                     const ProgressBarNodes nodes,
                     coro::queue<ProgressState> &updates)
{
  co_await scheduler.schedule ();

  float current = 0.0f;

  while (true)
    {
      auto update_result = co_await updates.pop ();
      if (!update_result)
        break;

      auto [fraction, label, finished] = *update_result;
      float target = fraction;

      while (current < target)
        {
          current = std::min (current + 0.05f, target);

          dom.update_text (nodes.label, fmt::format ("{:<20}", label));

          dom.update_text (
              nodes.percent,
              fmt::format ("{:>5.0f}%", std::round (current * 100.0f)));

          const int filled_cells = std::clamp (static_cast<int> (current * nodes.bar_width),
                                               0, nodes.bar_width);

          const auto &fill_node = dom.get (nodes.bar_fill);
          if (auto *elem = std::get_if<Element> (&fill_node.content))
            {
              Style new_style = elem->style;
              new_style.width = Size::fixed (filled_cells);
              new_style.fg_color
                  = Rgba8(finished ? fmt::color::cyan : fmt::color::green);
              dom.update_style (nodes.bar_fill, new_style);
            }

          co_await scheduler.yield_for (std::chrono::milliseconds (16));
        }

      if (finished)
        break;
    }

  co_return;
}

coro::task<>
text_widget (Dom &dom, const NodeId my_node, const std::string text)
{
  // Simple text display
  dom.update_text (my_node, text);
  co_return;
}

coro::task<>
flex_container_widget (std::vector<coro::task<>> children)
{
  // Wait for all children concurrently
  co_await coro::when_all (std::move (children));
  co_return;
}

} // namespace nxb::ui
