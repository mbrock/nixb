#include "Ui.hpp"

#include "Ansi.hpp"
#include "UiRender.hpp"
#include <fmt/core.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string_view>
#include <unordered_set>

namespace nixb
{

// ============================================================================
// DumbBackend: non-TTY fallback
// ============================================================================

class DumbBackend : public UiBackend
{
public:
  void
  println (std::string_view line) override
  {
    // Ensure line ends with newline
    bool has_nl = !line.empty () && line.back () == '\n';
    fmt::print ("{}{}", line, has_nl ? "" : "\n");
    std::fflush (stdout);
  }

  void
  update_hud (const UiState &) override
  {
    // No-op for dumb backend
  }

  bool
  enabled () const override
  {
    return false;
  }
};

// ============================================================================
// TerminalBackend: ANSI terminal with scroll regions and status HUD
// ============================================================================

class TerminalBackend : public UiBackend
{
public:
  explicit TerminalBackend (int rows, int cols)
      : status_lines_ (0), smoothed_status_lines_ (0.0f), rows_ (rows),
        cols_ (cols)
  {
    // Caller has already validated that this is a TTY with valid dimensions
    reconfigure_scroll_region ();
    enabled_ = true;
  }

  ~TerminalBackend () override { finish (); }

  void
  println (std::string_view line) override
  {
    if (!enabled_)
      {
        return;
      }

    ansi::move_cursor (scroll_bottom_, 1);
    bool has_trailing_newline = !line.empty () && line.back () == '\n';
    fmt::print ("{}{}", line, has_trailing_newline ? "" : "\n");
    std::fflush (stdout);
  }

  void
  update_hud (const UiState &state) override
  {
    if (!enabled_)
      return;

    last_state_ = state;

    int needed_lines = static_cast<int> (state.activity_lines.size ());
    int clamped_needed = std::clamp (needed_lines, 0, rows_ - 8);

    // Asymmetric dampening: immediate increase, smoothed decrease
    // Apply EMA on every frame tick for time-based smoothing
    if (clamped_needed > smoothed_status_lines_)
      {
        // Jump up immediately
        smoothed_status_lines_ = static_cast<float> (clamped_needed);
      }
    else
      {
        // Always smooth toward target (happens every frame)
        constexpr double alpha = 0.05;
        smoothed_status_lines_ = static_cast<float> (
            alpha * clamped_needed + (1.0 - alpha) * smoothed_status_lines_);
      }

    int new_status_lines
        = static_cast<int> (std::round (smoothed_status_lines_));
    if (new_status_lines != status_lines_)
      {
        apply_status_resize (new_status_lines);
      }

    draw_status_lines (state);

    ansi::move_cursor (rows_, 1);
    std::fflush (stdout);
  }

  bool
  enabled () const override
  {
    return enabled_;
  }

private:
  void
  apply_status_resize (int new_status_lines)
  {
    int old_status_lines = status_lines_;
    int old_scroll_bottom = scroll_bottom_;

    // Shrink the scroll region only after pushing visible log lines up so they
    // remain on screen.
    if (new_status_lines > old_status_lines && old_scroll_bottom > 0)
      {
        int scroll_diff = new_status_lines - old_status_lines;
        ansi::move_cursor (old_scroll_bottom, 1);
        ansi::scroll_up (scroll_diff);
      }

    status_lines_ = new_status_lines;
    reconfigure_scroll_region ();

    if (new_status_lines < old_status_lines)
      {
        int scroll_diff = old_status_lines - new_status_lines;
        ansi::move_cursor (1, 1);
        ansi::scroll_down (scroll_diff);

        // Newly freed rows belong to the scroll region; clear stale status
        // text.
        int first_freed_row = rows_ - old_status_lines + 1;
        int last_freed_row = rows_ - new_status_lines;
        for (int r = first_freed_row; r <= last_freed_row; ++r)
          {
            ansi::move_cursor (r, 1);
            ansi::clear_line ();
          }
      }
  }

  void
  draw_status_lines (const UiState &state)
  {
    int first_status_row = rows_ - status_lines_ + 1;

    // Create HUD raster
    HudRaster raster (status_lines_, cols_);

    // Render all activity lines into the raster
    for (int i = 0; i < status_lines_; ++i)
      {
        if (i < static_cast<int> (state.activity_lines.size ()))
          {
            render_activity_line (raster, i, state.activity_lines[i], cols_);
          }
      }

    // Get background color
    fmt::rgb bg_color = get_hud_background_color ();

    // Convert raster to ANSI with uniform background (returns vector of rows)
    std::vector<std::string> raster_rows = raster_to_ansi (raster, bg_color);

    // Build terminal output with positioning
    fmt::memory_buffer output;

    // Generate ANSI codes
    std::string reset = "\x1b[0m";

    // Draw top border above HUD
    if (first_status_row > 1)
      {
        fmt::format_to (std::back_inserter (output), "\x1b[{};{}H",
                        first_status_row - 1, 1);
        fmt::format_to (std::back_inserter (output), "\x1b[K");
        fmt::format_to (std::back_inserter (output), "\x1b[48;2;{};{};{}m",
                        bg_color.r, bg_color.g, bg_color.b);
        // Draw horizontal box line
        for (int i = 0; i < cols_; ++i)
          {
            fmt::format_to (std::back_inserter (output), "▔");
          }
        fmt::format_to (std::back_inserter (output), "{}", reset);
      }

    // Position and emit each row
    int row = first_status_row;
    for (int i = 0; i < status_lines_; ++i)
      {
        // Move to row
        fmt::format_to (std::back_inserter (output), "\x1b[{};{}H", row, 1);

        // Clear line
        fmt::format_to (std::back_inserter (output), "\x1b[K");
        fmt::format_to (std::back_inserter (output), "\x1b[48;2;{};{};{}m",
                        bg_color.r, bg_color.g, bg_color.b);

        // Emit the raster row content
        if (i + 1 < static_cast<int> (raster_rows.size ()))
          {
            fmt::format_to (std::back_inserter (output), "{}", raster_rows[i]);
          }
        else
          {
            fmt::format_to (std::back_inserter (output), "{}",
                            std::string (cols_, ' '));
          }

        // Reset at end of line
        fmt::format_to (std::back_inserter (output), "{}", reset);

        ++row;
      }

    // Write entire frame at once
    fmt::print ("{}", fmt::to_string (output));
  }

  void
  reconfigure_scroll_region ()
  {
    ansi::hide_cursor ();

    // Scroll region is from line 1 to scroll_bottom_.
    // Account for: status_lines_ + 1 border line
    scroll_bottom_ = rows_ - status_lines_ - 1;
    if (scroll_bottom_ < 1)
      scroll_bottom_ = 1;

    ansi::set_scroll_region (1, scroll_bottom_);
    ansi::move_cursor (scroll_bottom_, 1);
    std::fflush (stdout);
  }

  void
  finish ()
  {
    if (!enabled_ || torn_down_)
      return;

    ansi::reset_scroll_region ();

    int first_status_row = rows_ - status_lines_ + 1;
    for (int row = first_status_row; row <= rows_; ++row)
      {
        ansi::move_cursor (row, 1);
        ansi::clear_line ();
      }

    ansi::move_cursor (rows_, 1);
    fmt::print ("\n");
    ansi::show_cursor ();
    std::fflush (stdout);
    torn_down_ = true;
  }

  bool enabled_ = false;
  int status_lines_ = 0;
  float smoothed_status_lines_ = 0.0f; // EMA-smoothed line count
  int rows_ = 0;
  int cols_ = 0;
  int scroll_bottom_ = 0;
  bool torn_down_ = false;
  UiState last_state_;
};

// ============================================================================
// LogStream implementation
// ============================================================================

void
LogStream::println (std::string_view line)
{
  backend_.println (line);
}

// ============================================================================
// ActivityHud implementation
// ============================================================================

void
ActivityHud::present (const UiState &state)
{
  last_state_ = state;

  auto now = std::chrono::steady_clock::now ();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
      now - last_render_time_);

  // Rate limit: only render if enough time has passed
  if (elapsed < min_frame_interval_)
    {
      return; // Skip this frame, state is cached in last_state_
    }

  last_render_time_ = now;

  // Apply EMA smoothing to progress values
  UiState smoothed_state = state;
  std::unordered_set<int64_t> current_ids;

  for (auto &line : smoothed_state.activity_lines)
    {
      if (!line.progress)
        {
          continue;
        }

      current_ids.insert (line.id);
      auto it = smoothed_progress_.find (line.id);

      if (it == smoothed_progress_.end ())
        {
          // First time seeing this activity, initialize with current value
          smoothed_progress_[line.id] = *line.progress;
        }
      else
        {
          // Apply EMA: smoothed = alpha * new + (1 - alpha) * old
          ActivityProgress &smoothed = it->second;
          const ActivityProgress &current = *line.progress;

          // Use much faster smoothing when finished to "zip" to 100%
          double alpha = line.is_finished ? 0.8 : ema_alpha_;

          auto smooth = [alpha] (int64_t old_val, int64_t new_val) -> int64_t {
            return static_cast<int64_t> (alpha * new_val
                                         + (1.0 - alpha) * old_val);
          };

          smoothed.done = smooth (smoothed.done, current.done);
          smoothed.expected = smooth (smoothed.expected, current.expected);
          smoothed.running = smooth (smoothed.running, current.running);
          smoothed.failed = smooth (smoothed.failed, current.failed);
          smoothed.unit = current.unit; // Unit doesn't get smoothed

          // When finished, zip the progress toward 100%
          if (line.is_finished)
            {
              smoothed.done = smooth (smoothed.done, smoothed.expected);
            }

          smoothed_progress_[line.id] = smoothed;
        }

      // Use the smoothed value for rendering
      line.progress = smoothed_progress_[line.id];
    }

  // Clean up smoothed values for activities that no longer exist
  for (auto it = smoothed_progress_.begin (); it != smoothed_progress_.end ();)
    {
      if (current_ids.find (it->first) == current_ids.end ())
        {
          it = smoothed_progress_.erase (it);
        }
      else
        {
          ++it;
        }
    }

  backend_.update_hud (smoothed_state);
}

// ============================================================================
// UiSession implementation
// ============================================================================

UiSession::UiSession (std::unique_ptr<UiBackend> backend)
    : backend_ (std::move (backend)),
      log_ (std::make_unique<LogStream> (*backend_)),
      hud_ (std::make_unique<ActivityHud> (*backend_))
{
}

UiSession::~UiSession () = default;

UiSession
UiSession::create (bool enable)
{
  if (!enable || !::isatty (STDOUT_FILENO))
    {
      return UiSession (std::make_unique<DumbBackend> ());
    }

  const char *term = std::getenv ("TERM");
  if (!term || std::string_view (term) == "dumb")
    {
      return UiSession (std::make_unique<DumbBackend> ());
    }

  // Get terminal size
  winsize ws{};
  if (::ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0
      || ws.ws_col == 0 || ws.ws_row <= 1)
    {
      return UiSession (std::make_unique<DumbBackend> ());
    }

  // Create terminal backend with detected size
  auto backend = std::make_unique<TerminalBackend> (ws.ws_row, ws.ws_col);
  return UiSession (std::move (backend));
}

} // namespace nixb
