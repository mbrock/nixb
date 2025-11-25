#include "app.hpp"
#include "ui3.hpp"

#include <chrono>
#include <coro/coro.hpp>
#include <coro/sync_wait.hpp>
#include <coro/when_all.hpp>
#include <fmt/core.h>

namespace nxb::ui3
{

using namespace std::chrono_literals;

// ============================================================================
// State & View
// ============================================================================

struct ProgressState
{
  std::string label;
  float fraction = 0.0f;
  bool finished = false;
};

/// Custom layout for the responsive bar portion only
struct ResponsiveBarLayout
{
  float fraction;
  bool finished;

  SizeHint width_hint = SizeHint::flex (1);
  SizeHint height_hint = SizeHint::content ();

  Size
  preferred_size () const
  {
    return { 10, 1 }; // Minimum bar size
  }

  void
  render_with_layout (Raster &raster, Size allocated) const
  {
    // Unicode block characters for smooth sub-character progress
    static constexpr std::array<std::string_view, 9> blocks
        = { " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█" };

    std::size_t bar_width = allocated.w;

    // Colors
    Rgba8 bar_fg = finished ? Rgba8 (0, 255, 255) : Rgba8 (128, 162, 162);
    Rgba8 bar_bg = Rgba8 (50, 50, 50);

    // Calculate precise fill based on actual allocated width
    double precise_filled = fraction * static_cast<double> (bar_width);
    int full_blocks = static_cast<int> (precise_filled);
    int partial_index
        = static_cast<int> ((precise_filled - full_blocks) * 8.0 + 0.5);

    if (partial_index >= 8)
      {
        full_blocks++;
        partial_index = 0;
      }

    // Render bar background
    for (std::size_t x = 0; x < bar_width; ++x)
      raster.set_bg (x, 0, bar_bg);

    // Render filled blocks
    std::size_t x = 0;
    for (int i = 0; i < full_blocks && x < bar_width; ++i, ++x)
      {
        raster.write_text (x, 0, "█", bar_fg, bar_bg);
      }

    // Render partial block
    if (partial_index > 0 && x < bar_width)
      {
        raster.write_text (x, 0, std::string (blocks[partial_index]), bar_fg,
                           bar_bg);
      }
  }
};

/// Pure view function: ProgressState → LayoutSender
/// Uses row layout for label + bar + percentage, with responsive bar
auto
progress_bar_view (const ProgressState &state)
{
  // Label (fixed width)
  auto label = text (fmt::format ("{:<20}", state.label), Rgba8 (255, 255, 255));
  label.width_hint = SizeHint::fixed_size (22);

  // Responsive bar (grows to fill available space)
  auto bar = ResponsiveBarLayout{ state.fraction, state.finished };

  // Percentage (fixed width)
  auto percent_text
      = text (fmt::format ("{:>4.0f}%", state.fraction * 100),
              Rgba8 (200, 200, 255));
  percent_text.width_hint = SizeHint::content ();

  // Compose using row layout
  return row (label, bar, percent_text);
}

// ============================================================================
// Animation Logic
// ============================================================================

coro::task<>
animate_download (coro::io_scheduler &scheduler, Signal<ProgressState> &signal,
                  std::string label, int duration_ms)
{
  co_await scheduler.schedule ();
  co_await signal.set ({ label, 0.0f, false });

  for (int i = 0; i <= 100; i++)
    {
      co_await signal.set ({ label, i / 100.0f, i == 100 });
      co_await scheduler.yield_for (std::chrono::milliseconds (duration_ms));
    }
}

// Helper to create root layout (now just uses column!)
template <typename Bar1, typename Bar2>
auto
make_root_layout (Bar1 bar1, Bar2 bar2)
{
  return column (text ("Download Progress", Rgba8 (120, 200, 255)),
                 text (std::string (80, '-'), Rgba8 (80, 80, 100)),
                 bar1,
                 bar2,
                 text (""),  // spacing
                 text ("Building packages...", Rgba8 (255, 200, 100)));
}

// ============================================================================
// Runtime Infrastructure
// ============================================================================

namespace
{

/// Helper: coordinate animations and trigger shutdown when complete
coro::task<>
run_animations (coro::io_scheduler &scheduler, nxb::ui::UIRuntime &runtime,
                std::vector<coro::task<>> animations,
                std::vector<Signal<ProgressState> *> signals)
{
  co_await scheduler.schedule ();
  co_await coro::when_all (std::move (animations));

  runtime.request_shutdown ();
  for (auto *sig : signals)
    co_await sig->queue ()->shutdown ();
}

} // namespace

// ============================================================================
// Application Entry Point
// ============================================================================

int
run ()
{
  // Setup runtime and terminal
  auto scheduler
      = coro::io_scheduler::make_shared (coro::io_scheduler::options{});
  nxb::ui::UIRuntime runtime (*scheduler);
  std::size_t width = static_cast<std::size_t> (runtime.terminal_width ());
  std::size_t height = static_cast<std::size_t> (runtime.terminal_height ());

  GlyphTable glyphs;
  nxb::ui::TerminalCompositor compositor (width, height, glyphs);

  // Define reactive state
  Signal<ProgressState> nixpkgs_signal;
  Signal<ProgressState> rustc_signal;

  // Create widgets (LayoutSenders driven by signals)
  Widget nixpkgs_bar (nixpkgs_signal,
                      [] (const ProgressState &s)
                      { return progress_bar_view (s); });

  Widget rustc_bar (rustc_signal,
                    [] (const ProgressState &s) { return progress_bar_view (s); });

  // Create root layout - now properly uses column layout!
  auto root_layout = make_root_layout (nixpkgs_bar, rustc_bar);

  // Track if we need to re-render
  std::atomic<bool> dirty{ false };
  auto request_render = [&] () { dirty.store (true, std::memory_order_release); };

  // Render loop
  auto render_loop = [&] () -> coro::task<>
  {
    co_await scheduler->schedule ();

    // Initial render
    {
      auto &buffer = compositor.back_buffer ();
      buffer.clear ();
      root_layout.render_with_layout (buffer, Size{ width, height });
      compositor.present_frame ();
    }

    while (!runtime.shutdown_requested ())
      {
        // Handle terminal resize
        if (auto size_opt = runtime.resize_channel ().try_pop ())
          {
            width = size_opt->width;
            height = size_opt->height;
            compositor.resize (width, height);
            dirty.store (true, std::memory_order_release);
          }

        // Re-render if dirty
        if (dirty.exchange (false, std::memory_order_acq_rel))
          {
            auto &buffer = compositor.back_buffer ();
            buffer.clear ();
            root_layout.render_with_layout (buffer, Size{ width, height });
            compositor.present_frame ();
          }

        co_await scheduler->yield_for (16ms); // ~60fps
      }
  };

  // Define animations
  std::vector<coro::task<>> animations;
  animations.push_back (
      animate_download (*scheduler, nixpkgs_signal, "nixpkgs.tar.gz", 20));
  animations.push_back (
      animate_download (*scheduler, rustc_signal, "rustc.tar.xz", 15));

  // Assemble all tasks
  std::vector<coro::task<>> tasks;
  tasks.push_back (runtime.signal_loop ());
  tasks.push_back (render_loop ());
  tasks.push_back (run_animations (*scheduler, runtime, std::move (animations),
                                   { &nixpkgs_signal, &rustc_signal }));
  tasks.push_back (nixpkgs_bar.run (*scheduler, request_render));
  tasks.push_back (rustc_bar.run (*scheduler, request_render));

  // Run application
  try
    {
      nxb::ui::TerminalGuard guard;
      coro::sync_wait (coro::when_all (std::move (tasks)));
    }
  catch (const std::exception &e)
    {
      fmt::print (stderr, "Error: {}\n", e.what ());
      return 1;
    }

  // Report how we exited
  switch (runtime.shutdown_reason ())
    {
    case nxb::ui::ShutdownReason::Completed:
      fmt::print ("Completed!\n");
      break;
    case nxb::ui::ShutdownReason::Interrupted:
      fmt::print ("Interrupted.\n");
      break;
    case nxb::ui::ShutdownReason::Running:
      break;
    }

  return 0;
}

} // namespace nxb::ui3

int
main ()
{
  return nxb::ui3::run ();
}
