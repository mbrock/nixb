#pragma once

#include "dom.hpp"

#include <coro/coro.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/queue.hpp>
#include <coro/task.hpp>

namespace nxb::ui
{

/// Progress bar update message
struct ProgressUpdate
{
  float fraction = 0.0f;
  std::string label;
  bool finished = false;
};

/// Progress bar coroutine
/// Creates DOM nodes as local variables; they're removed when coroutine ends
coro::task<> progress_bar (coro::io_scheduler &scheduler, Dom &dom,
                           NodeId parent, std::string label, int bar_width,
                           coro::queue<ProgressUpdate> &updates);

} // namespace nxb::ui
