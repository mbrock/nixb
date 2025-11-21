#include "UiRender.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace nixb
{

namespace
{
std::string ellipsize_middle (std::string_view text, int max_width);

std::string
format_bytes (int64_t bytes)
{
  if (bytes < 0)
    return "0B";

  constexpr int64_t KiB = 1024;
  constexpr int64_t MiB = 1024 * KiB;
  constexpr int64_t GiB = 1024 * MiB;
  constexpr int64_t TiB = 1024 * GiB;

  if (bytes >= TiB)
    return fmt::format ("{:.1f}TiB", static_cast<double> (bytes) / TiB);
  if (bytes >= GiB)
    return fmt::format ("{:.1f}GiB", static_cast<double> (bytes) / GiB);
  if (bytes >= MiB)
    return fmt::format ("{:.1f}MiB", static_cast<double> (bytes) / MiB);
  if (bytes >= KiB)
    return fmt::format ("{:.1f}KiB", static_cast<double> (bytes) / KiB);
  return fmt::format ("{}B", bytes);
}

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

  // Allocate reasonable widths: 30 chars for label, 40 for bar, rest for stats
  int label_width = 30;
  int bar_width = 40;
  int stats_width = 20;

  // Adjust if terminal is narrow
  if (cols < label_width + bar_width + stats_width)
    {
      label_width = std::max (15, cols / 3);
      bar_width = std::max (20, cols - label_width - stats_width);
    }

  // Use Unicode block characters for smooth progress visualization
  // ▏ ▎ ▍ ▌ ▋ ▊ ▉ █ provide 8 levels of granularity
  static constexpr std::array<std::string_view, 9> blocks
      = { " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█" };

  // ANSI color codes for styling
  constexpr const char *gray_bg = "\x1b[48;5;240m"; // Gray background
  constexpr const char *reset = "\x1b[0m";

  // Calculate precise progress with sub-character granularity
  double precise_filled = frac * static_cast<double> (bar_width);
  int full_blocks = static_cast<int> (precise_filled);
  double remainder = precise_filled - static_cast<double> (full_blocks);
  int partial_index = static_cast<int> (remainder * 8.0 + 0.5);

  // Prepare label (truncate if needed)
  std::string display_label (label);
  if (static_cast<int> (display_label.size ()) > label_width)
    {
      display_label = ellipsize_middle (display_label, label_width);
    }

  fmt::memory_buffer buf;
  // Left-align label in its allocated width
  fmt::format_to (std::back_inserter (buf), "{:<{}}", display_label,
                  label_width);
  fmt::format_to (std::back_inserter (buf), " │");

  // Render progress bar
  for (int i = 0; i < full_blocks && i < bar_width; ++i)
    {
      fmt::format_to (std::back_inserter (buf), "█");
    }

  // Render partial block if there's room and we're not at 100%
  if (full_blocks < bar_width && partial_index > 0)
    {
      // Add gray background to partial block so unfilled side matches backdrop
      fmt::format_to (std::back_inserter (buf), "{}{}{}", gray_bg,
                      blocks[partial_index], reset);
      full_blocks++;
    }

  // Render empty space
  for (int i = full_blocks; i < bar_width; ++i)
    {
      fmt::format_to (std::back_inserter (buf), "░");
    }

  fmt::format_to (std::back_inserter (buf), "│");

  // Format done/total based on unit type
  if (p.unit == ProgressUnit::Bytes)
    {
      fmt::format_to (std::back_inserter (buf), " {}/{}",
                      format_bytes (p.done), format_bytes (total));
    }
  else
    {
      fmt::format_to (std::back_inserter (buf), " {}/{}", p.done, total);
    }

  if (p.running > 0 || p.failed > 0)
    {
      fmt::format_to (std::back_inserter (buf), " ◉{} ✗{}", p.running,
                      p.failed);
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
} // namespace

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

} // namespace nixb
