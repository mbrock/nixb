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

struct SingleBuildState
{
  int64_t id = 0;
  std::string label;
  std::string status; // queued/running
};

struct SingleTransferState
{
  int64_t id = 0;
  std::string label;
  ActivityProgress progress;
};

struct UiState
{
  std::optional<ActivityProgress> builds_aggregate;
  std::string current_phase;
  std::vector<SingleBuildState> active_builds;
  std::vector<SingleTransferState> active_transfers;
};

// Wraps ANSI terminal control required for the bottom progress bar UI.
class TerminalUi
{
public:
  explicit TerminalUi (int status_lines = 3, bool force = false);
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

  // Allow adjusting footer height and redraw when it changes.
  void update_status_height (int desired_status_lines, const UiState &state);

  // Reset terminal state and clear status lines.
  void finish ();

  // Max footer height allowed by the terminal (rows - 1).
  int
  max_status_lines () const
  {
    return enabled_ ? rows_ - 1 : 0;
  }

private:
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
