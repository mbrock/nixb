#include "Ui.hpp"

#include "Ansi.hpp"
#include "UiRender.hpp"
#include <fmt/core.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string_view>

namespace nixb {

// ============================================================================
// DumbBackend: non-TTY fallback
// ============================================================================

class DumbBackend : public UiBackend {
public:
  void println(std::string_view line) override {
    // Ensure line ends with newline
    bool has_nl = !line.empty() && line.back() == '\n';
    fmt::print("{}{}", line, has_nl ? "" : "\n");
    std::fflush(stdout);
  }

  void update_hud(const UiState &) override {
    // No-op for dumb backend
  }

  bool enabled() const override { return false; }
};

// ============================================================================
// TerminalBackend: ANSI terminal with scroll regions and status HUD
// ============================================================================

class TerminalBackend : public UiBackend {
public:
  explicit TerminalBackend(int rows, int cols)
      : status_lines_(0), rows_(rows), cols_(cols) {
    // Caller has already validated that this is a TTY with valid dimensions
    reconfigure_scroll_region();
    enabled_ = true;
  }

  ~TerminalBackend() override { finish(); }

  void println(std::string_view line) override {
    if (!enabled_) {
      return;
    }

    ansi::move_cursor(scroll_bottom_, 1);
    bool has_trailing_newline = !line.empty() && line.back() == '\n';
    fmt::print("{}{}", line, has_trailing_newline ? "" : "\n");
    std::fflush(stdout);
  }

  void update_hud(const UiState &state) override {
    if (!enabled_)
      return;

    last_state_ = state;

    int needed_lines = static_cast<int>(state.activity_lines.size());
    int new_status_lines = std::clamp(needed_lines, 0, rows_ - 1);
    if (new_status_lines != status_lines_) {
      apply_status_resize(new_status_lines);
    }

    draw_status_lines(state);

    ansi::move_cursor(rows_, 1);
    std::fflush(stdout);
  }

  bool enabled() const override { return enabled_; }

private:
  void apply_status_resize(int new_status_lines) {
    int old_status_lines = status_lines_;
    int old_scroll_bottom = scroll_bottom_;

    // Shrink the scroll region only after pushing visible log lines up so they
    // remain on screen.
    if (new_status_lines > old_status_lines && old_scroll_bottom > 0) {
      int scroll_diff = new_status_lines - old_status_lines;
      ansi::move_cursor(old_scroll_bottom, 1);
      ansi::scroll_up(scroll_diff);
    }

    status_lines_ = new_status_lines;
    reconfigure_scroll_region();

    if (new_status_lines < old_status_lines) {
      int scroll_diff = old_status_lines - new_status_lines;
      ansi::move_cursor(1, 1);
      ansi::scroll_down(scroll_diff);

      // Newly freed rows belong to the scroll region; clear stale status text.
      int first_freed_row = rows_ - old_status_lines + 1;
      int last_freed_row = rows_ - new_status_lines;
      for (int r = first_freed_row; r <= last_freed_row; ++r) {
        ansi::move_cursor(r, 1);
        ansi::clear_line();
      }
    }
  }

  void draw_status_lines(const UiState &state) {
    int first_status_row = rows_ - status_lines_ + 1;
    int row = first_status_row;

    // Build entire output in memory to reduce flicker
    fmt::memory_buffer output;

    // Hide cursor while updating
    ansi::hide_cursor();

    for (int i = 0; i < status_lines_; ++i) {
      // Generate cursor positioning and line clear in buffer
      fmt::format_to(std::back_inserter(output), "\x1b[{};{}H", row, 1);
      fmt::format_to(std::back_inserter(output), "\x1b[2K");

      if (i < static_cast<int>(state.activity_lines.size())) {
        fmt::format_to(std::back_inserter(output), "{}",
                       render_activity_line(state.activity_lines[i], cols_));
      }

      ++row;
    }

    // Write entire frame at once
    fmt::print("{}", fmt::to_string(output));

    // Show cursor again
    ansi::show_cursor();
  }

  void reconfigure_scroll_region() {
    // Scroll region is from line 1 to scroll_bottom_.
    scroll_bottom_ = rows_ - status_lines_;
    if (scroll_bottom_ < 1)
      scroll_bottom_ = 1;

    ansi::set_scroll_region(1, scroll_bottom_);
    ansi::move_cursor(scroll_bottom_, 1);
    std::fflush(stdout);
  }

  void finish() {
    if (!enabled_ || torn_down_)
      return;

    ansi::reset_scroll_region();

    int first_status_row = rows_ - status_lines_ + 1;
    for (int row = first_status_row; row <= rows_; ++row) {
      ansi::move_cursor(row, 1);
      ansi::clear_line();
    }

    ansi::move_cursor(rows_, 1);
    fmt::print("\n");
    std::fflush(stdout);
    torn_down_ = true;
  }

  bool enabled_ = false;
  int status_lines_ = 0;
  int rows_ = 0;
  int cols_ = 0;
  int scroll_bottom_ = 0;
  bool torn_down_ = false;
  UiState last_state_;
};

// ============================================================================
// LogStream implementation
// ============================================================================

void LogStream::println(std::string_view line) { backend_.println(line); }

// ============================================================================
// ActivityHud implementation
// ============================================================================

void ActivityHud::present(const UiState &state) {
  last_state_ = state;
  backend_.update_hud(state);
}

// ============================================================================
// UiSession implementation
// ============================================================================

UiSession::UiSession(std::unique_ptr<UiBackend> backend)
    : backend_(std::move(backend)),
      log_(std::make_unique<LogStream>(*backend_)),
      hud_(std::make_unique<ActivityHud>(*backend_)) {}

UiSession::~UiSession() = default;

UiSession UiSession::create(bool force) {
  // TTY detection logic (rationalized - only here, not in TerminalUi)
  if (!force) {
    if (!::isatty(STDOUT_FILENO)) {
      return UiSession(std::make_unique<DumbBackend>());
    }

    const char *term = std::getenv("TERM");
    if (!term || std::string_view(term) == "dumb") {
      return UiSession(std::make_unique<DumbBackend>());
    }
  }

  // Get terminal size
  winsize ws{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0 ||
      ws.ws_col == 0 || ws.ws_row <= 1) {
    return UiSession(std::make_unique<DumbBackend>());
  }

  // Create terminal backend with detected size
  auto backend = std::make_unique<TerminalBackend>(ws.ws_row, ws.ws_col);
  return UiSession(std::move(backend));
}

} // namespace nixb
