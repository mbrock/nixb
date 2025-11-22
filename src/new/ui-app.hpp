#pragma once

#include <coro/event.hpp>
#include <coro/task.hpp>
#include <cstddef>
#include "glyph-table.hpp"

namespace coro {
class io_scheduler;
}

namespace nxb::ui
{

class Dom;
struct NodeId;
class LayoutEngine;
class Painter;

/// Global shutdown event (set on SIGINT/SIGTERM).
coro::event &shutdown_event();
bool shutdown_requested();

/// Current terminal dimensions (tracked via SIGWINCH).
int terminal_width();
int terminal_height();

/// Initialize runtime state (signals, term size, scheduler pointer).
void init_ui_runtime(coro::io_scheduler &scheduler);

/// Guard that hides cursor/clears screen on scope exit.
struct TerminalGuard
{
  TerminalGuard();
  ~TerminalGuard();
};

/// Render loop that handles resizing, diffing, and ANSI output.
coro::task<void> render_loop_task(coro::io_scheduler &scheduler, Dom &dom,
                                  nxb::GlyphTable &glyphs, LayoutEngine &layout,
                                  Painter &painter, NodeId container_node);

} // namespace nxb::ui


