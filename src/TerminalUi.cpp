#include "TerminalUi.hpp"

#include "Ansi.hpp"
#include <fmt/core.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>

namespace nixb
{

namespace
{

std::string
render_progress_line (std::string_view label, const ActivityProgress &p,
                      int cols)
{
  int64_t total = p.expected;
  if (total <= 0)
    {
      total = p.done + p.running + p.failed;
    }
  if (total < p.done)
    {
      total = p.done;
    }
  if (total <= 0)
    {
      return fmt::format ("{} waiting...", label);
    }

  double frac = static_cast<double> (p.done)
                / static_cast<double> (std::max<int64_t> (total, 1));
  int reserved = static_cast<int> (label.size ()) + 20;
  int bar_width = std::max (10, cols - reserved);
  int filled = static_cast<int> (frac * static_cast<double> (bar_width) + 0.5);

  fmt::memory_buffer buf;
  fmt::format_to (std::back_inserter (buf), "{}", label);
  fmt::format_to (std::back_inserter (buf), " [");
  for (int i = 0; i < bar_width; ++i)
    {
      fmt::format_to (std::back_inserter (buf), "{}", i < filled ? '#' : '-');
    }
  fmt::format_to (std::back_inserter (buf), "]");
  fmt::format_to (std::back_inserter (buf), " {}/{}", p.done, total);
  if (p.running > 0 || p.failed > 0)
    {
      fmt::format_to (std::back_inserter (buf), " ({} running, {} failed)",
                      p.running, p.failed);
    }

  return fmt::to_string (buf);
}

std::string
ellipsize_middle (std::string_view text, int max_width)
{
  if (max_width <= 0)
    {
      return "";
    }
  if (static_cast<int> (text.size ()) <= max_width)
    {
      return std::string (text);
    }
  if (max_width <= 3)
    {
      return std::string (max_width, '.');
    }
  int head = (max_width - 3) / 2;
  int tail = max_width - 3 - head;
  return fmt::format ("{}...{}", text.substr (0, head),
                      text.substr (text.size () - tail));
}

std::string
render_activity_line (const UiActivityLine &line, int cols)
{
  std::string label = line.label;
  if (static_cast<int> (label.size ()) > cols)
    {
      label = ellipsize_middle (label, cols);
    }
  if (line.progress)
    {
      return render_progress_line (label, *line.progress, cols);
    }
  return label;
}
} // namespace

TerminalUi::TerminalUi (int status_lines, bool force)
    : status_lines_ (status_lines)
{
  if (!force)
    {
      if (!::isatty (STDOUT_FILENO))
        return;
      const char *term = std::getenv ("TERM");
      if (!term || std::string_view (term) == "dumb")
        return;
    }

  winsize ws{};
  if (::ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0
      || ws.ws_col == 0)
    {
      return;
    }

  rows_ = ws.ws_row;
  cols_ = ws.ws_col;

  if (rows_ <= 1)
    {
      return;
    }

  status_lines_ = std::clamp (status_lines_, 0, rows_ - 1);

  reconfigure_scroll_region ();

  enabled_ = true;
}

TerminalUi::~TerminalUi () { finish (); }

void
TerminalUi::print_log_block (std::string_view block)
{
  if (!enabled_)
    {
      fmt::print ("{}\n", block);
      return;
    }

  ansi::move_cursor (scroll_bottom_, 1);
  bool has_trailing_newline = !block.empty () && block.back () == '\n';
  fmt::print ("{}{}", block, has_trailing_newline ? "" : "\n");
  std::fflush (stdout);
}

void
TerminalUi::redraw (const UiState &state)
{
  if (!enabled_)
    return;

  last_state_ = state;

  int needed_lines = static_cast<int> (state.activity_lines.size ());
  int new_status_lines = std::clamp (needed_lines, 0, rows_ - 1);

  if (new_status_lines != status_lines_)
    {
      int old_status_lines = status_lines_;
      int old_scroll_bottom = scroll_bottom_;

      if (new_status_lines > old_status_lines && old_scroll_bottom > 0)
        {
          int scroll_diff = status_lines_ - old_status_lines;
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
          // Clear the rows now part of the scroll region so we don't leave
          // stale status text.
          int first_freed_row = rows_ - old_status_lines + 1;
          int last_freed_row = rows_ - new_status_lines;
          for (int r = first_freed_row; r <= last_freed_row; ++r)
            {
              ansi::move_cursor (r, 1);
              ansi::clear_line ();
            }
        }
    }

  int first_status_row = rows_ - status_lines_ + 1;
  int row = first_status_row;

  auto draw_line = [&] (std::string_view text)
    {
      ansi::move_cursor (row, 1);
      ansi::clear_line ();
      fmt::print ("{}", text);
      ++row;
    };

  for (int i = 0; i < status_lines_; ++i)
    {
      if (i < static_cast<int> (state.activity_lines.size ()))
        {
          draw_line (render_activity_line (state.activity_lines[i], cols_));
        }
      else
        {
          // This branch implies status_lines_ > activity_lines.size()
          // which happens if needed_lines > rows_-1, so status_lines_ is
          // capped but the loop continues up to status_lines_. Wait, if
          // needed_lines > rows_-1, status_lines_ = rows_-1. The loop goes
          // from 0 to rows_-2 (since < status_lines_). But
          // activity_lines.size() is larger. So this branch is NOT hit in that
          // case.
          //
          // This branch is strictly for: i >= activity_lines.size().
          // Since status_lines_ = min(activity_lines.size(), rows_-1),
          // this means i >= min(...) AND i < min(...).
          // This is impossible. The branch is dead code if logic is correct.
          draw_line ("");
        }
    }

  ansi::move_cursor (rows_, 1);
  std::fflush (stdout);
}

void
TerminalUi::reconfigure_scroll_region ()
{
  // Scroll region is from line 1 to scroll_bottom_.
  scroll_bottom_ = rows_ - status_lines_;
  if (scroll_bottom_ < 1)
    scroll_bottom_ = 1;

  ansi::set_scroll_region (1, scroll_bottom_);
  ansi::move_cursor (scroll_bottom_, 1);
  std::fflush (stdout);
}

void
TerminalUi::finish ()
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
  std::fflush (stdout);
  torn_down_ = true;
}

} // namespace nixb
