#pragma once

#include "glyph-table.hpp"
#include "tty-raster.hpp"
#include <coro/event.hpp>
#include <coro/queue.hpp>
#include <coro/task.hpp>

namespace coro
{
class io_scheduler;
}

namespace nxb::ui
{

class Dom;
struct NodeId;
class LayoutEngine;
class Painter;

struct TermSize
{
  int width;
  int height;
};

/// Global shutdown event (set on SIGINT/SIGTERM).
coro::event &shutdown_event ();
bool shutdown_requested ();

/// Damage signalling between view generators and compositor.
coro::event &damage_event ();
void signal_damage ();

/// Resize notifications (published by runtime, consumed by view drivers).
coro::queue<TermSize> &resize_channel ();

/// Current terminal dimensions (tracked via SIGWINCH).
int terminal_width ();
int terminal_height ();
TermSize terminal_size ();

/// Initialize runtime state (signals, term size, scheduler pointer).
void init_ui_runtime (coro::io_scheduler &scheduler);

/// Guard that hides cursor/clears screen on scope exit.
struct TerminalGuard
{
  TerminalGuard ();
  ~TerminalGuard ();
};

class TerminalCompositor
{
public:
  TerminalCompositor (int width, int height, nxb::GlyphTable &glyphs);

  void resize (int width, int height);
  nxb::Raster &back_buffer () noexcept;
  nxb::GlyphTable &glyphs () noexcept;

  coro::task<void> present_loop (coro::io_scheduler &scheduler);

private:
  void present_frame ();

  nxb::Raster front_;
  nxb::Raster back_;
  nxb::GlyphTable &glyphs_;
};

} // namespace nxb::ui
