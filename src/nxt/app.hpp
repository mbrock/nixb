#pragma once

#include "nxt/ansi.hpp"
#include "nxt/async.hpp"
#include "nxt/compositor.hpp"
#include "nxt/glyph-table.hpp"
#include "nxt/raster.hpp"
#include "nxt/signal-pipe.hpp"
#include "nxt/units.hpp"

#include <atomic>
#include <chrono>
#include <mp-units/framework.h>
#include <mp-units/framework/construction_helpers.h>
#include <mp-units/systems/isq/base_quantities.h>
#include <mp-units/systems/si/chrono.h>
#include <mp-units/systems/si/units.h>
#include <stop_token>

namespace nxb::ui
{

  using TermSize = nxb::Size;

  // Re-export TerminalGuard for convenience
  using ansi::TerminalGuard;

  /// Runtime state for the UI system.
  /// Owns scheduler, glyph table, compositor, and coordinates
  /// signals/events.
  class UIRuntime
  {
  public:
    UIRuntime ();
    ~UIRuntime ();

    // Non-copyable, non-moveable (owns resources)
    UIRuntime (const UIRuntime &) = delete;
    UIRuntime &operator= (const UIRuntime &) = delete;
    UIRuntime (UIRuntime &&) = delete;
    UIRuntime &operator= (UIRuntime &&) = delete;

    /// Access the scheduler.
    [[nodiscard]] nxb::io_scheduler &
    scheduler () noexcept
    {
      return *scheduler_;
    }

    /// Access the glyph table.
    [[nodiscard]] GlyphTable &
    glyphs () noexcept
    {
      return glyphs_;
    }

    /// Check if shutdown has been requested.
    [[nodiscard]] bool
    shutdown_requested () const noexcept
    {
      return stop_source_.stop_requested ();
    }

    /// Request shutdown.
    void request_shutdown ();

    /// Run a task, then request shutdown when it completes.
    nxb::task<>
    shutdown_after (
      nxb::task<> t)
    {
      co_await t;
      request_shutdown ();
    }

    template <class rep_type, class period_type>
    nxb::task<>
    sleep (
      std::chrono::duration<rep_type, period_type> duration)
    {
      co_await scheduler ().yield_for (duration);
    }

    /// Signal that the view has been damaged and needs redraw.
    void signal_damage ();

    /// Stop token
    std::stop_token
    get_stop_token () const noexcept
    {
      return stop_source_.get_token ();
    }

    /// Current terminal dimensions.
    [[nodiscard]] TermSize terminal_size () const noexcept;
    [[nodiscard]] width_t terminal_width () const noexcept;
    [[nodiscard]] height_t terminal_height () const noexcept;

    template <typename... Tasks>
    auto
    run (
      Tasks &&...tasks)
    {
      auto tg = nxb::task_group (
        this->scheduler (), std::forward<Tasks> (tasks)...);
      return tg.run_all ();
    }

    // =========================================================================
    // Render loop helpers
    // =========================================================================

    /// Render a layout to the screen.
    /// Computes HUD height from layout hint, sets up scroll region,
    /// renders.
    template <typename Layout>
    void
    render (
      const Layout &layout)
    {
      // Compute HUD height from layout
      auto hint = layout.height_hint ();
      auto term_h = terminal_height ();

      // If layout wants to grow, use full screen; otherwise use min
      // height
      height_t hud_h = hint.flex > 0 * one ? term_h : hint.min;
      hud_h = std::min (hud_h, term_h); // Clamp to terminal

      update_hud_height (hud_h);

      render_impl ([&layout] (RasterView &view, Size size) {
        layout.render (view, size);
      });
    }

    /// Print a line to the scroll region (only works when HUD
    /// height < terminal). In full-screen mode, this is a no-op.
    void println (std::string_view line);

    /// Clean up before exit - clears HUD region.
    void cleanup ();

    /// Run a render loop until shutdown.
    /// BuildUI is called each frame to produce the layout.
    /// Waits for damage signal, but rate-limits to frame_time.
    /// Note: pass by value to avoid dangling references in
    /// coroutine.
    template <typename BuildUI>
    nxb::task<>
    run_render_loop (
      BuildUI build_ui,
      std::chrono::milliseconds frame_time
      = std::chrono::milliseconds{ 16 })
    {
      // Initial render
      render (build_ui ());
      auto last_render = std::chrono::steady_clock::now ();

      while (!shutdown_requested ())
        {
          // Wait for damage
          damage_event_.reset ();
          co_await damage_event_;

          if (shutdown_requested ())
            break;

          // Rate limit: wait until frame_time has passed since last
          // render
          auto now = std::chrono::steady_clock::now ();
          auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds> (now - last_render);
          if (elapsed < frame_time)
            co_await scheduler_->yield_for (frame_time - elapsed);

          // Process any pending resizes
          while (auto sz = resize_queue_.try_pop ())
            compositor_->resize (*sz);

          render (build_ui ());
          last_render = std::chrono::steady_clock::now ();
        }
    }

    /// Coroutine that handles signals from the pipe.
    /// Should be run as part of the main task group.
    nxb::task<> signal_loop ();

    // =========================================================================
    // Low-level access (for advanced use)
    // =========================================================================

    /// Channel for resize notifications.
    nxb::queue<TermSize> &
    resize_channel () noexcept
    {
      return resize_queue_;
    }

    /// Event signaled when damage occurs.
    nxb::event &
    damage_event () noexcept
    {
      return damage_event_;
    }

    /// Direct access to compositor (for testing or advanced use).
    [[nodiscard]] TerminalCompositor &compositor () noexcept;

  private:
    void refresh_terminal_size () noexcept;
    void render_impl (
      std::function<void (RasterView &, Size)> render_fn);
    void update_hud_height (height_t hud_h);

    std::shared_ptr<nxb::io_scheduler> scheduler_;
    GlyphTable glyphs_;
    std::unique_ptr<TerminalCompositor> compositor_;
    SignalPipe signals_;

    nxb::event damage_event_;
    nxb::queue<TermSize> resize_queue_;

    std::atomic<nxb::width_t> term_width_{ 80 * ch };
    std::atomic<nxb::height_t> term_height_{ 24 * ln };
    std::atomic<std::uint64_t> damage_counter_{ 0 };

    std::stop_source stop_source_;
  };

  // ============================================================================
  // Convenient app runner
  // ============================================================================

  /// Run a TUI application.
  /// - initial_state: the starting state
  /// - build_ui: (const State&) → Layout
  /// - update: (UIRuntime&, State&) → nxb::task<> (should call
  /// request_shutdown)
  template <typename State, typename BuildUI, typename Update>
  int
  run (
    State initial_state, BuildUI build_ui, Update update)
  {
    UIRuntime runtime;
    State state = std::move (initial_state);

    try
      {
        TerminalGuard guard;
        nxb::task_group tasks (runtime.scheduler ());

        tasks
          << runtime.signal_loop ()
          << runtime.run_render_loop (
               [&state, build_ui] { return build_ui (state); })
          << runtime.shutdown_after (update (runtime, state));

        tasks.run_all ();
        runtime.cleanup ();
      }
    catch (const std::exception &e)
      {
        runtime.cleanup ();
        fmt::print (stderr, "Error: {}\n", e.what ());
        return 1;
      }
    catch (...)
      {
        runtime.cleanup ();
        throw;
      }

    return 0;
  }

} // namespace nxb::ui
