#pragma once

#include "dom.hpp"

#include <coro/io_scheduler.hpp>
#include <coro/queue.hpp>
#include <coro/task.hpp>

namespace nxb::ui
{

/// Progress bar state
struct ProgressState
{
  float fraction = 0.0f;
  std::string label;
  bool finished = false;
};

/// DOM nodes for a progress bar row
struct ProgressBarNodes
{
  NodeId label;
  NodeId bar_fill;
  NodeId percent;
  int bar_width = 0;
};

/// Widget coroutine: Animated progress bar
/// Updates its own DOM node, runs until finished
/// Uses coro::queue for communication
coro::task<> progress_bar_widget (coro::io_scheduler &scheduler, Dom &dom,
                                  ProgressBarNodes nodes,
                                  coro::queue<ProgressState> &updates);

/// Widget coroutine: Text display
coro::task<> text_widget (Dom &dom, NodeId my_node, std::string text);

/// Container coroutine: Flex container managing child widgets
coro::task<> flex_container_widget (std::vector<coro::task<>> children);

} // namespace nxb::ui
