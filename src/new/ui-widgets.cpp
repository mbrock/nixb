#include "ui-widgets.hpp"

#include <coro/sync_wait.hpp>
#include <coro/when_all.hpp>
#include <fmt/core.h>

namespace nxb::ui
{

coro::task<void>
progress_bar_widget (coro::io_scheduler &scheduler, Dom &dom, NodeId my_node,
                     coro::queue<ProgressState> &updates)
{
  // Schedule on the io_scheduler
  co_await scheduler.schedule ();

  float current = 0.0f;

  while (true)
    {
      // Wait for update from queue
      auto update_result = co_await updates.pop ();
      if (!update_result)
        break; // Queue shutdown

      auto update = *update_result;

      // Smooth animation toward target
      float target = update.fraction;
      while (current < target)
        {
          current += 0.05f;
          if (current > target)
            current = target;

          // Update my DOM node
          std::string bar_filled (int (current * 20), '=');
          std::string bar_empty (int ((1 - current) * 20), ' ');
          auto bar = fmt::format ("[{}{}] {} {:.0f}%", bar_filled, bar_empty,
                                  update.label, current * 100);
          dom.update_text (my_node, bar);

          // Sleep for 16ms (60fps)
          co_await scheduler.yield_for (std::chrono::milliseconds (16));
        }

      if (update.finished)
        break;
    }

  co_return;
}

coro::task<void>
text_widget (Dom &dom, NodeId my_node, std::string text)
{
  // Simple text display
  dom.update_text (my_node, text);
  co_return;
}

coro::task<void>
flex_container_widget (Dom &dom, NodeId my_node,
                       std::vector<coro::task<void>> children)
{
  // Wait for all children concurrently
  co_await coro::when_all (std::move (children));
  co_return;
}

} // namespace nxb::ui
