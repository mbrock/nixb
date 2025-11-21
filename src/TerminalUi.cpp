#include "TerminalUi.hpp"

#include "Ansi.hpp"
#include "UiRender.hpp"
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

// Redraws the footer and adjusts the scroll region to match the number of
// activity lines. Growing the footer scrolls the log region up; shrinking
// scrolls it down and clears newly freed rows.
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
      apply_status_resize (new_status_lines);
    }

  draw_status_lines (state);

  ansi::move_cursor (rows_, 1);
  std::fflush (stdout);
}

void
TerminalUi::apply_status_resize (int new_status_lines)
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

      // Newly freed rows belong to the scroll region; clear stale status text.
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
TerminalUi::draw_status_lines (const UiState &state)
{
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
          draw_line ("");
        }
    }
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
