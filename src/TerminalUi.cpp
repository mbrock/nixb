#include "TerminalUi.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>

namespace nixb {

namespace {
constexpr const char *CSI = "\x1b[";

std::string render_progress_line(std::string_view label,
                                 const ActivityProgress &p, int cols) {
  int64_t total = p.done + p.expected + p.running + p.failed;
  if (total <= 0) {
    return fmt::format("{} waiting...", label);
  }

  double frac = static_cast<double>(p.done) /
                static_cast<double>(std::max<int64_t>(total, 1));
  int reserved = static_cast<int>(label.size()) + 20;
  int bar_width = std::max(10, cols - reserved);
  int filled = static_cast<int>(frac * static_cast<double>(bar_width) + 0.5);

  fmt::memory_buffer buf;
  fmt::format_to(std::back_inserter(buf), "{}", label);
  fmt::format_to(std::back_inserter(buf), " [");
  for (int i = 0; i < bar_width; ++i) {
    fmt::format_to(std::back_inserter(buf), "{}", i < filled ? '#' : '-');
  }
  fmt::format_to(std::back_inserter(buf), "]");
  fmt::format_to(std::back_inserter(buf), " {}/{}", p.done, total);
  if (p.running > 0 || p.failed > 0) {
    fmt::format_to(std::back_inserter(buf), " ({} running, {} failed)",
                   p.running, p.failed);
  }

  return fmt::to_string(buf);
}

std::string ellipsize_middle(std::string_view text, int max_width) {
  if (max_width <= 0) {
    return "";
  }
  if (static_cast<int>(text.size()) <= max_width) {
    return std::string(text);
  }
  if (max_width <= 3) {
    return std::string(max_width, '.');
  }
  int head = (max_width - 3) / 2;
  int tail = max_width - 3 - head;
  return fmt::format("{}...{}", text.substr(0, head), text.substr(text.size() - tail));
}

std::string render_build_line(const SingleBuildState &b, int cols) {
  std::string base = fmt::format("[{}] {}", b.status, b.label);
  if (static_cast<int>(base.size()) <= cols) {
    return base;
  }
  return ellipsize_middle(base, cols);
}

std::string render_transfer_line(const SingleTransferState &t, int cols) {
  std::string label = fmt::format("[dl] {}", t.label);
  return render_progress_line(label, t.progress, cols);
}
} // namespace

TerminalUi::TerminalUi(int status_lines, bool force)
    : status_lines_(status_lines) {
  if (!force) {
    if (!::isatty(STDOUT_FILENO))
      return;
    const char *term = std::getenv("TERM");
    if (!term || std::string_view(term) == "dumb")
      return;
  }

  winsize ws{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0 ||
      ws.ws_col == 0) {
    return;
  }

  rows_ = ws.ws_row;
  cols_ = ws.ws_col;

  if (rows_ <= 1) {
    return;
  }

  status_lines_ = std::clamp(status_lines_, 1, rows_ - 1);

  reconfigure_scroll_region();

  enabled_ = true;
}

TerminalUi::~TerminalUi() {
  finish();
}

void TerminalUi::print_log_block(std::string_view block) {
  if (!enabled_) {
    fmt::print("{}", block);
    return;
  }

  fmt::print("{}{};1H{}2K", CSI, scroll_bottom_, CSI);
  fmt::print("{}", block);
  std::fflush(stdout);
}

void TerminalUi::redraw(const UiState &state) {
  if (!enabled_)
    return;

  last_state_ = state;

  int first_status_row = rows_ - status_lines_ + 1;
  int row = first_status_row;

  auto draw_line = [&](std::string_view text) {
    fmt::print("{}{};1H{}2K{}", CSI, row, CSI, text);
    ++row;
  };

  if (state.builds_aggregate) {
    draw_line(render_progress_line("[builds]", *state.builds_aggregate, cols_));
  } else {
    draw_line("[builds] waiting...");
  }

  for (const auto &b : state.active_builds) {
    draw_line(render_build_line(b, cols_));
  }

  if (!state.active_transfers.empty()) {
    for (const auto &t : state.active_transfers) {
      draw_line(render_transfer_line(t, cols_));
    }
  } else {
    draw_line("[downloads] waiting...");
  }

  if (!state.current_phase.empty()) {
    draw_line(state.current_phase);
  } else {
    draw_line("");
  }

  while (row <= rows_) {
    draw_line("");
  }

  fmt::print("{}{};1H", CSI, rows_);
  std::fflush(stdout);
}

void TerminalUi::update_status_height(int desired_status_lines,
                                      const UiState &state) {
  if (!enabled_)
    return;

  desired_status_lines = std::clamp(desired_status_lines, 1, rows_ - 1);
  if (desired_status_lines != status_lines_) {
    status_lines_ = desired_status_lines;
    reconfigure_scroll_region();
  }

  redraw(state);
}

void TerminalUi::reconfigure_scroll_region() {
  scroll_bottom_ = rows_ - status_lines_;
  if (scroll_bottom_ < 1) {
    scroll_bottom_ = 1;
  }
  fmt::print("{}1;{}r", CSI, scroll_bottom_);
  fmt::print("{}1;1H", CSI);
  std::fflush(stdout);
}

void TerminalUi::finish() {
  if (!enabled_ || torn_down_)
    return;

  fmt::print("{}r", CSI);
  int first_status_row = rows_ - status_lines_ + 1;
  for (int row = first_status_row; row <= rows_; ++row) {
    fmt::print("{}{};1H{}2K", CSI, row, CSI);
  }
  fmt::print("{}{};1H", CSI, rows_);
  fmt::print("\n");
  std::fflush(stdout);
  torn_down_ = true;
}

} // namespace nixb
