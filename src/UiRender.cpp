#include "UiRender.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace nixb {

std::string format_bytes(int64_t bytes) {
  if (bytes < 0)
    return "0B";

  constexpr int64_t KiB = 1024;
  constexpr int64_t MiB = 1024 * KiB;
  constexpr int64_t GiB = 1024 * MiB;
  constexpr int64_t TiB = 1024 * GiB;

  // No decimals - just show whole units
  if (bytes >= TiB)
    return fmt::format("{}TiB", bytes / TiB);
  if (bytes >= GiB)
    return fmt::format("{}GiB", bytes / GiB);
  if (bytes >= MiB)
    return fmt::format("{}MiB", bytes / MiB);
  if (bytes >= KiB)
    return fmt::format("{}KiB", bytes / KiB);
  return fmt::format("{}B", bytes);
}

namespace {
std::string ellipsize_middle(std::string_view text, int max_width);

std::string render_progress_line(std::string_view label,
                                 const ActivityProgress &p, int cols) {
  int64_t total = p.expected;
  if (total <= 0) {
    total = p.done + p.running + p.failed;
  }
  if (total < p.done) {
    total = p.done;
  }

  // Calculate progress fraction, treating unknown totals as empty bar
  double frac;
  if (total <= 0) {
    total = 1;  // Treat as empty bar
    frac = 0.0; // No progress yet
  } else {
    frac = static_cast<double>(p.done) /
           static_cast<double>(std::max<int64_t>(total, 1));
  }

  // Dynamically allocate widths based on terminal size
  // Try to give: ~40% to label, ~40% to bar, ~20% to stats
  int label_width = std::max(20, cols * 4 / 10);
  int stats_width = 25; // Fixed for consistency
  int bar_width =
      std::max(20, cols - label_width - stats_width - 4); // -4 for separators

  // Use Unicode block characters for smooth progress visualization
  // ▏ ▎ ▍ ▌ ▋ ▊ ▉ █ provide 8 levels of granularity
  static constexpr std::array<std::string_view, 9> blocks = {
      " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};

  // ANSI color codes for styling
  constexpr const char *gray_bg = "\x1b[48;5;240m"; // Gray background
  constexpr const char *reset = "\x1b[0m";

  // Calculate precise progress with sub-character granularity
  double precise_filled = frac * static_cast<double>(bar_width);
  int full_blocks = static_cast<int>(precise_filled);
  double remainder = precise_filled - static_cast<double>(full_blocks);
  int partial_index = static_cast<int>(remainder * 8.0 + 0.5);

  // Prepare label (truncate if needed)
  std::string display_label(label);
  if (static_cast<int>(display_label.size()) > label_width) {
    display_label = ellipsize_middle(display_label, label_width);
  }

  fmt::memory_buffer buf;
  // Left-align label in its allocated width
  fmt::format_to(std::back_inserter(buf), "{:<{}}", display_label, label_width);
  fmt::format_to(std::back_inserter(buf), " │");

  // Render progress bar
  for (int i = 0; i < full_blocks && i < bar_width; ++i) {
    fmt::format_to(std::back_inserter(buf), "█");
  }

  // Render partial block if there's room and we're not at 100%
  if (full_blocks < bar_width && partial_index > 0) {
    // Add gray background to partial block so unfilled side matches backdrop
    fmt::format_to(std::back_inserter(buf), "{}{}{}", gray_bg,
                   blocks[partial_index], reset);
    full_blocks++;
  }

  // Render empty space
  for (int i = full_blocks; i < bar_width; ++i) {
    fmt::format_to(std::back_inserter(buf), "░");
  }

  fmt::format_to(std::back_inserter(buf), "│");

  // Format done/total with column alignment
  if (p.unit == ProgressUnit::Bytes) {
    // Right-align done, left-align total for neat columns
    fmt::format_to(std::back_inserter(buf), " {:>6} / {:<6}",
                   format_bytes(p.done), format_bytes(total));
  } else {
    fmt::format_to(std::back_inserter(buf), " {:>4} / {:<4}", p.done, total);
  }

  if (p.running > 0 || p.failed > 0) {
    fmt::format_to(std::back_inserter(buf), " ◉{} ✗{}", p.running, p.failed);
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
  return fmt::format("{}...{}", text.substr(0, head),
                     text.substr(text.size() - tail));
}
} // namespace

std::string render_activity_line(const UiActivityLine &line, int cols) {
  std::string label = line.label;

  if (line.progress) {
    // Progress lines don't need ellipsis - they're column-formatted
    return render_progress_line(label, *line.progress, cols);
  }

  // Only ellipsize non-progress lines if they're too long
  if (static_cast<int>(label.size()) > cols) {
    label = ellipsize_middle(label, cols);
  }
  return label;
}

} // namespace nixb
