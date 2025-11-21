#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nixb
{

struct ActivityProgress
{
  int64_t done = 0;
  int64_t expected = 0;
  int64_t running = 0;
  int64_t failed = 0;
};

struct UiActivityLine
{
  int64_t id = 0;
  std::string label;
  std::optional<ActivityProgress> progress;
};

struct UiState
{
  std::vector<UiActivityLine> activity_lines;
};

// Wraps ANSI terminal control required for the bottom progress bar UI.
class TerminalUi
{
public:
  explicit TerminalUi (int status_lines = 0, bool force = false);
  ~TerminalUi ();

  bool
  enabled () const
  {
    return enabled_;
  }

  // Append a scrolling block of text (normal log output).
  void print_log_block (std::string_view block);

  // Redraw bottom progress bars from the current UiState.
  void redraw (const UiState &state);

  // Reset terminal state and clear status lines.
  void finish ();

  // Max footer height allowed by the terminal (rows - 1).
  int
  max_status_lines () const
  {
    return enabled_ ? rows_ - 1 : 0;
  }

private:
  void apply_status_resize (int new_status_lines);
  void draw_status_lines (const UiState &state);
  void reconfigure_scroll_region ();

  bool enabled_ = false;
  int status_lines_ = 0;
  int rows_ = 0;
  int cols_ = 0;
  int scroll_bottom_ = 0;
  bool torn_down_ = false;
  UiState last_state_;
};

} // namespace nixb
