#include "UiRender.hpp"
#include "fmt/color.h"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace nixb
{

std::string
format_bytes (int64_t bytes)
{
  if (bytes < 0)
    return "0B";

  constexpr int64_t KiB = 1024;
  constexpr int64_t MiB = 1024 * KiB;
  constexpr int64_t GiB = 1024 * MiB;
  constexpr int64_t TiB = 1024 * GiB;

  // No decimals - just show whole units
  if (bytes >= TiB)
    return fmt::format ("{}TiB", bytes / TiB);
  if (bytes >= GiB)
    return fmt::format ("{}GiB", bytes / GiB);
  if (bytes >= MiB)
    return fmt::format ("{}MiB", bytes / MiB);
  if (bytes >= KiB)
    return fmt::format ("{}KiB", bytes / KiB);
  return fmt::format ("{}B", bytes);
}

namespace
{
std::string ellipsize_middle (std::string_view text, int max_width);

// Render a simple counter line (for Count units)
std::string
render_counter_line (std::string_view label, const ActivityProgress &p,
                     int64_t total, int cols)
{
  fmt::memory_buffer buf;

  int max_label_width = cols - 20; // Reserve space for counter
  std::string display_label (label);
  if (static_cast<int> (display_label.size ()) > max_label_width)
    display_label = ellipsize_middle (display_label, max_label_width);

  fmt::format_to (std::back_inserter (buf), "{}", display_label);
  if (p.done > 0)
    fmt::format_to (std::back_inserter (buf), "{} done",
                    fmt::styled (p.done, fmt::fg (fmt::terminal_color::green)
                                             | fmt::emphasis::bold));
  if (p.expected > 0)
    fmt::format_to (
        std::back_inserter (buf), " {} expected",
        fmt::styled (p.expected, fmt::fg (fmt::terminal_color::yellow)
                                     | fmt::emphasis::bold));
  if (p.running > 0)
    fmt::format_to (std::back_inserter (buf), " {} running",
                    fmt::styled (p.running, fmt::fg (fmt::terminal_color::blue)
                                                | fmt::emphasis::bold));
  if (p.failed > 0)
    fmt::format_to (std::back_inserter (buf), " {} failed",
                    fmt::styled (p.failed, fmt::fg (fmt::terminal_color::red)
                                               | fmt::emphasis::bold));

  return fmt::to_string (buf);
}

// Render a full progress bar (for Bytes units)
std::string
render_bar_line (std::string_view label, const ActivityProgress &p,
                 int64_t total, int cols)
{
  double frac = total > 0
                    ? static_cast<double> (p.done)
                          / static_cast<double> (std::max<int64_t> (total, 1))
                    : 0.0;

  int label_width = std::max (20, cols * 4 / 10);
  int stats_width = 25;
  int bar_width = std::max (20, cols - label_width - stats_width - 4);

  // Unicode block characters for smooth progress
  static constexpr std::array<std::string_view, 9> blocks
      = { " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█" };
  constexpr const char *gray_bg = "\x1b[48;5;240m";
  constexpr const char *reset = "\x1b[0m";

  double precise_filled = frac * static_cast<double> (bar_width);
  int full_blocks = static_cast<int> (precise_filled);
  int partial_index
      = static_cast<int> ((precise_filled - full_blocks) * 8.0 + 0.5);

  std::string display_label (label);
  if (static_cast<int> (display_label.size ()) > label_width)
    display_label = ellipsize_middle (display_label, label_width);

  fmt::memory_buffer buf;
  fmt::format_to (std::back_inserter (buf), "{:<{}} │", display_label,
                  label_width);

  // Filled blocks
  for (int i = 0; i < full_blocks && i < bar_width; ++i)
    fmt::format_to (std::back_inserter (buf), "█");

  // Partial block
  if (full_blocks < bar_width && partial_index > 0)
    {
      fmt::format_to (std::back_inserter (buf), "{}{}{}", gray_bg,
                      blocks[partial_index], reset);
      full_blocks++;
    }

  // Empty blocks
  for (int i = full_blocks; i < bar_width; ++i)
    fmt::format_to (std::back_inserter (buf), "░");

  fmt::format_to (std::back_inserter (buf), "│ {:>6} / {:<6}",
                  format_bytes (p.done), format_bytes (total));

  return fmt::to_string (buf);
}

std::string
render_progress_line (std::string_view label, const ActivityProgress &p,
                      int cols)
{
  // Calculate total, adjusting for edge cases
  int64_t total = std::max (p.expected, p.done);
  if (total <= 0)
    total = p.done + p.running + p.failed;

  return p.unit == ProgressUnit::Count
             ? render_counter_line (label, p, total, cols)
             : render_bar_line (label, p, total, cols);
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

  if (line.progress)
    {
      return render_progress_line (label, *line.progress, cols);
    }

  if (static_cast<int> (label.size ()) > cols)
    {
      label = ellipsize_middle (label, cols);
    }
  return label;
}

} // namespace nixb
