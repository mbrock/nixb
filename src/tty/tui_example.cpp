#include "app.hpp"
#include "tui.hpp"
#include "units.hpp"

#include <coro/coro.hpp>
#include <fmt/core.h>
#include <mp-units/framework.h>
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
// Colors
// ============================================================================

const Rgba8 cyan{ 100, 200, 255 };
const Rgba8 green{ 100, 255, 150 };
const Rgba8 yellow{ 255, 220, 100 };
const Rgba8 dim_white{ 180, 180, 180 };
const Rgba8 bright_white{ 255, 255, 255 };

// ============================================================================
// View Functions (State → Layout)
// ============================================================================

// Progress bar row: [label] [████▌░░░░░] [100%]
auto
activity_row (const Activity &act)
{
  // Label styled based on completion state
  auto label_style = act.finished ? (fg (green) | bold) : fg (dim_white);
  auto label = text (fmt::format ("{:<20}", act.label), label_style);

  // Percentage with color gradient
  auto pct_val = act.progress.numerical_value_in (percent);
  auto pct_style = pct_val >= 100 ? fg (green)
                   : pct_val >= 50 ? fg (yellow)
                                   : fg (dim_white);
  auto pct_text
      = text (fmt::format ("{:>4.0f}%", pct_val), pct_style | bold);

  return row (label, progress_bar (act.progress), pct_text);
}

auto
build_ui (const AppState &state)
{
  // Styled title using spans
  auto title = styled_text (span ("Build ", fg (bright_white) | bold),
                            span ("Progress", fg (cyan) | italic));

  return column (title, hrule (), list (state.activities, activity_row),
                 text (""));
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

  auto size = runtime.terminal_size ();

  GlyphTable glyphs;
  nxb::ui::TerminalCompositor compositor (size, glyphs);

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
        state.activities[0].progress
            = std::min (100.0, frame / 80.0 * 100) * percent;
        state.activities[1].progress
            = std::min (100.0, frame / 60.0 * 100) * percent;
        state.activities[2].progress
            = std::min (100.0, frame / 100.0 * 100) * percent;

        state.activities[0].finished = state.activities[0].progress >= 1 * one;
        state.activities[1].finished = state.activities[1].progress >= 1 * one;
        state.activities[2].finished = state.activities[2].progress >= 1 * one;

        co_await scheduler->yield_for (30ms);
      }

    co_await scheduler->yield_for (500ms);
    runtime.request_shutdown ();
  };

  auto render_loop = [&] () -> coro::task<> {
    co_await scheduler->schedule ();

    while (!runtime.shutdown_requested ())
      {
        if (auto sz = runtime.resize_channel ().try_pop ())
          {
            size = sz.value ();
            compositor.resize (size);
          }

        auto layout = build_ui (state);

        auto &buffer = compositor.back_buffer ();
        buffer.clear ();
        auto view = buffer.view ();
        layout.render (view, size);
        compositor.present_frame ();

        co_await scheduler->yield_for (16ms); // ~60fps
      }
  };

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
