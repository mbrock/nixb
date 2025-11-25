#include "app.hpp"
#include "tui.hpp"
#include "units.hpp"

#include <coro/coro.hpp>
#include <fmt/core.h>
#include <vector>

namespace nxb::tui_example
{

using namespace std::chrono_literals;
using namespace nxb::tui;

// ============================================================================
// Application State (plain data)
// ============================================================================

struct Activity
{
  std::string label;
  percent_t progress{ 0.0 * percent };
  bool finished = false;
};

struct AppState
{
  std::vector<Activity> activities;
};

// ============================================================================
// View Functions (State → Layout)
// ============================================================================

// Progress bar row: [label] [████▌░░░░░] [100%]
auto
activity_row (const Activity &act)
{
  auto label = text (fmt::format ("{:<20}", act.label));

  auto pct_text = text (
      fmt::format ("{:>4.0f}%", act.progress.numerical_value_in (percent)));

  return row (label, progress_bar (act.progress), pct_text);
}

auto
build_ui (const AppState &state)
{
  return column (text ("Build Progress"), hrule (),
                 list (state.activities, activity_row), text (""));
}

// ============================================================================
// Main Loop
// ============================================================================

int
run ()
{
  // Setup
  auto scheduler
      = coro::io_scheduler::make_shared (coro::io_scheduler::options{});
  nxb::ui::UIRuntime runtime (*scheduler);

  std::size_t width = static_cast<std::size_t> (runtime.terminal_width ());
  std::size_t height = static_cast<std::size_t> (runtime.terminal_height ());

  GlyphTable glyphs;
  nxb::ui::TerminalCompositor compositor (width, height, glyphs);

  // Application state
  AppState state;
  state.activities.push_back ({ "nixpkgs.tar.gz", 0.0 * percent, false });
  state.activities.push_back ({ "rustc.tar.xz", 0.0 * percent, false });
  state.activities.push_back ({ "llvm-17.src.tar.xz", 0.0 * percent, false });

  // Animation task - updates state
  auto animate = [&] () -> coro::task<> {
    co_await scheduler->schedule ();

    for (int frame = 0; frame <= 100; ++frame)
      {
        // Update activities at different speeds
        state.activities[0].progress
            = std::min (100.0, frame / 80.0 * 100) * percent;
        state.activities[1].progress
            = std::min (100.0, frame / 60.0 * 100) * percent;
        state.activities[2].progress
            = std::min (100.0, frame / 100.0 * 100) * percent;

        state.activities[0].finished
            = state.activities[0].progress.numerical_value_in (percent)
              >= 100.0;
        state.activities[1].finished
            = state.activities[1].progress.numerical_value_in (percent)
              >= 100.0;
        state.activities[2].finished
            = state.activities[2].progress.numerical_value_in (percent)
              >= 100.0;

        co_await scheduler->yield_for (30ms);
      }

    // Wait a moment then exit
    co_await scheduler->yield_for (500ms);
    runtime.request_shutdown ();
  };

  // Render loop - reads state, builds layout, renders
  auto render_loop = [&] () -> coro::task<> {
    co_await scheduler->schedule ();

    while (!runtime.shutdown_requested ())
      {
        // Handle resize
        if (auto sz = runtime.resize_channel ().try_pop ())
          {
            width = sz->width;
            height = sz->height;
            compositor.resize (width, height);
          }

        // Build layout from current state (immediate mode!)
        auto layout = build_ui (state);

        // Render to back buffer
        auto &buffer = compositor.back_buffer ();
        buffer.clear ();
        layout.render (buffer, nxb::Size{ width * nxb::ch, height * nxb::ln });
        compositor.present_frame ();

        co_await scheduler->yield_for (16ms); // ~60fps
      }
  };

  // Run
  try
    {
      nxb::ui::TerminalGuard guard;
      std::vector<coro::task<>> tasks;
      tasks.push_back (runtime.signal_loop ());
      tasks.push_back (render_loop ());
      tasks.push_back (animate ());
      coro::sync_wait (coro::when_all (std::move (tasks)));
    }
  catch (const std::exception &e)
    {
      fmt::print (stderr, "Error: {}\n", e.what ());
      return 1;
    }

  fmt::print ("Done!\n");
  return 0;
}

} // namespace nxb::tui_example

int
main ()
{
  return nxb::tui_example::run ();
}
