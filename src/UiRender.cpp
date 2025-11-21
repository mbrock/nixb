#include "UiRender.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <string_view>

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
