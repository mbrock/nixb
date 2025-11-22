#include "UiRender.hpp"
#include "IdColor.hpp"
#include "fmt/color.h"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace nixb
{

// Background color for HUD - dark slate blue grey
fmt::rgb
get_hud_background_color ()
{
  return DEFAULT_HUD_BG_COLOR;
}

std::string
format_bytes (int64_t bytes)
{
  if (bytes < 0)
    return "0B";

  constexpr int64_t KiB = 1024;
  constexpr int64_t MiB = 1024 * KiB;
  constexpr int64_t GiB = 1024 * MiB;
  constexpr int64_t TiB = 1024 * GiB;

  // No decimals - just show whole units with short names
  if (bytes >= TiB)
    return fmt::format ("{}T", bytes / TiB);
  if (bytes >= GiB)
    return fmt::format ("{}G", bytes / GiB);
  if (bytes >= MiB)
    return fmt::format ("{}M", bytes / MiB);
  if (bytes >= KiB)
    return fmt::format ("{}K", bytes / KiB);
  return fmt::format ("{}B", bytes);
}

namespace
{
std::string ellipsize_middle (std::string_view text, int max_width);

// Terminal colors as RGB
inline fmt::rgb
terminal_green ()
{
  return fmt::rgb (100, 200, 140);
}

inline fmt::rgb
terminal_yellow ()
{
  return fmt::rgb (255, 255, 0);
}

inline fmt::rgb
terminal_blue ()
{
  return fmt::rgb (0, 128, 255);
}

inline fmt::rgb
terminal_red ()
{
  return fmt::rgb (255, 0, 0);
}

inline fmt::rgb
terminal_white ()
{
  return fmt::rgb (255, 255, 255);
}

// Render a simple counter line (for Count units)
void
render_counter_line (HudRaster &raster, int row, std::string_view label,
                     const ActivityProgress &p, int64_t total, int cols)
{
  int max_label_width = cols - 20; // Reserve space for counter
  std::string display_label (label);
  if (static_cast<int> (display_label.size ()) > max_label_width)
    display_label = ellipsize_middle (display_label, max_label_width);

  int col = 1;

  // Write label (no special color)
  raster.write_text (row, col, display_label);
  col += display_label.size ();

  // Write stats with colors
  if (p.done > 0)
    {
      std::string done_text = fmt::format (" {} done", p.done);
      raster.write_text (row, col, done_text, terminal_green ());
      col += done_text.size ();
    }
  if (p.expected > 0)
    {
      std::string expected_text = fmt::format (" {} expected", p.expected);
      raster.write_text (row, col, expected_text, terminal_yellow ());
      col += expected_text.size ();
    }
  if (p.running > 0)
    {
      std::string running_text = fmt::format (" {} running", p.running);
      raster.write_text (row, col, running_text, terminal_blue ());
      col += running_text.size ();
    }
  if (p.failed > 0)
    {
      std::string failed_text = fmt::format (" {} failed", p.failed);
      raster.write_text (row, col, failed_text, terminal_red ());
      col += failed_text.size ();
    }
}

// Render a full progress bar (for Bytes units)
void
render_bar_line (HudRaster &raster, int row, std::string_view label,
                 const ActivityProgress &p, int64_t total, int cols,
                 bool is_finished, double fade_factor,
                 const std::optional<std::string> &url = {})
{
  double frac = total > 0
                    ? static_cast<double> (p.done)
                          / static_cast<double> (std::max<int64_t> (total, 1))
                    : 0.0;

  // Calculate alpha for fade effect
  float alpha = 1.0f;
  if (is_finished && total > 0)
    {
      // Fade from 1.0 (fully visible) to 0.0 (invisible)
      alpha = 1.0f - static_cast<float> (fade_factor);
    }

  // Layout constants
  constexpr int left_margin = 1;
  constexpr int url_width
      = 16; // Enough for "cache.nixos.org" (15 chars) + 1 space
  constexpr int right_margin = 1;

  // Format stats with fixed width: " 9% of 1024M"
  // Calculate percentage
  int percent = (total > 0) ? static_cast<int> ((static_cast<double> (p.done)
                                                 / static_cast<double> (total))
                                                * 100.0)
                            : 0;

  std::string stats
      = fmt::format ("{:3}% of {}", percent, format_bytes (total));
  constexpr int stats_width = 14; // Fixed width for "100% of 9999M"

  // Calculate available space for name, bar, and separators
  int available = cols - left_margin - right_margin - stats_width;
  if (url)
    available -= url_width;

  // Allocate space
  int name_width = url ? 30 : 40; // Less space when URL is present
  int bar_width
      = std::max (15, available - name_width - 2); // 2 for separators

  // Adjust if we don't have enough space
  if (bar_width < 15)
    {
      name_width = std::max (15, available - 15 - 2);
      bar_width = 15;
    }

  std::string display_label (label);
  if (static_cast<int> (display_label.size ()) > name_width)
    display_label = ellipsize_middle (display_label, name_width);

  // Unicode block characters for smooth progress
  static constexpr std::array<std::string_view, 9> blocks
      = { " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█" };

  double precise_filled = frac * static_cast<double> (bar_width);
  int full_blocks = static_cast<int> (precise_filled);
  int partial_index
      = static_cast<int> ((precise_filled - full_blocks) * 8.0 + 0.5);

  int col = 0;

  // Left margin
  raster.set_cell (row, col, " ", std::nullopt, std::nullopt, alpha);
  ++col;

  // Write name (bold)
  std::optional<fmt::rgb> label_color;
  raster.write_text (row, col, display_label, label_color, std::nullopt, alpha,
                     true);
  col += display_label.size ();

  // Pad name to name_width
  for (int i = static_cast<int> (display_label.size ()); i < name_width; ++i)
    {
      raster.set_cell (row, col, " ", label_color, std::nullopt, alpha, true);
      ++col;
    }

  // Write URL if present (dim color)
  if (url)
    {
      // Dim gray color for URL
      fmt::rgb dim_color (128, 128, 128);
      std::string url_display = *url;
      if (static_cast<int> (url_display.size ()) > url_width)
        url_display = ellipsize_middle (url_display, url_width);

      raster.write_text (row, col, url_display, dim_color, std::nullopt, alpha,
                         false);
      col += url_display.size ();

      // Pad URL to url_width
      for (int i = static_cast<int> (url_display.size ()); i < url_width; ++i)
        {
          raster.set_cell (row, col, " ", dim_color, std::nullopt, alpha,
                           false);
          ++col;
        }
    }

  // Left box separator
  raster.set_cell (row, col, " ", label_color, std::nullopt, alpha);
  ++col;

  // Bar background color - lighter to distinguish from default bg
  fmt::rgb bar_bg_color = DEFAULT_HUD_BG_COLOR;

  // Filled blocks foreground color
  fmt::rgb bar_color
      = (is_finished && total > 0) ? terminal_green () : terminal_white ();

  // Full blocks with background
  for (int i = 0; i < full_blocks && i < bar_width; ++i)
    {
      raster.set_cell (row, col, "█", bar_color, std::nullopt, alpha);
      ++col;
    }

  // Partial block (use same color as filled)
  if (full_blocks < bar_width && partial_index > 0)
    {
      raster.set_cell (row, col, blocks[partial_index], bar_color,
                       bar_bg_color, alpha);
      ++col;
      full_blocks++;
    }

  // Empty space - same background to create bar effect
  for (int i = full_blocks; i < bar_width; ++i)
    {
      raster.set_cell (row, col, " ", std::nullopt, bar_bg_color, alpha);
      ++col;
    }

  // Right box separator
  raster.set_cell (row, col, " ", std::nullopt, std::nullopt, alpha);
  ++col;

  // Calculate where stats should start to be right-aligned
  int stats_start = cols - right_margin - stats_width;

  // Pad to stats position if needed
  while (col < stats_start)
    {
      raster.set_cell (row, col, " ", std::nullopt, std::nullopt, alpha);
      ++col;
    }

  // Write stats (right-aligned with margin)
  raster.write_text (row, col, stats, std::nullopt, std::nullopt, alpha);
}

void
render_progress_line (HudRaster &raster, int row, std::string_view label,
                      const ActivityProgress &p, int cols, bool is_finished,
                      double fade_factor,
                      const std::optional<std::string> &url = {})
{
  // Calculate total, adjusting for edge cases
  int64_t total = std::max (p.expected, p.done);
  if (total <= 0)
    total = p.done + p.running + p.failed;

  if (p.unit == ProgressUnit::Count)
    render_counter_line (raster, row, label, p, total, cols);
  else
    render_bar_line (raster, row, label, p, total, cols, is_finished,
                     fade_factor, url);
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

void
render_activity_line (HudRaster &raster, int row, const UiActivityLine &line,
                      int cols)
{
  std::string label = line.label;

  if (line.progress)
    {
      render_progress_line (raster, row, label, *line.progress, cols,
                            line.is_finished, line.fade_factor, line.url);
      return;
    }

  // No progress - render label with dependency info
  std::string dep_suffix;
  if (line.num_input_deps > 0 || line.num_dependents > 0)
    {
      if (line.num_input_deps > 0 && line.num_dependents > 0)
        {
          dep_suffix = fmt::format (" ↓{} ↑{}", line.num_input_deps,
                                    line.num_dependents);
        }
      else if (line.num_input_deps > 0)
        {
          dep_suffix = fmt::format (" ↓{}", line.num_input_deps);
        }
      else
        {
          dep_suffix = fmt::format (" ↑{}", line.num_dependents);
        }
    }

  std::string full_label = label + dep_suffix;
  if (static_cast<int> (full_label.size ()) > cols)
    {
      // Try ellipsizing just the label part if that would fit
      int needed_width = cols - dep_suffix.size ();
      if (needed_width > 10)
        {
          label = ellipsize_middle (label, needed_width);
          full_label = label + dep_suffix;
        }
      else
        {
          full_label = ellipsize_middle (full_label, cols);
        }
    }

  int col = 1;
  raster.write_text (row, col, label);
  col += label.size ();

  if (!dep_suffix.empty () && col < cols)
    {
      // Render dependency info in dim color
      fmt::rgb dim_color (150, 150, 170);
      raster.write_text (row, col, dep_suffix, dim_color);
    }
}

// Blend foreground color towards background based on alpha using OKLCH color
// space
inline fmt::rgb
blend_color (fmt::rgb fg, fmt::rgb bg, float alpha)
{
  return nixb::blend_oklch (fg, bg, alpha);
}

std::vector<std::string>
raster_to_ansi (const HudRaster &raster, fmt::rgb default_bg_color)
{
  std::vector<std::string> rows;
  rows.reserve (raster.rows ());

  for (int row = 0; row < raster.rows (); ++row)
    {
      fmt::memory_buffer output;

      // Track current colors and bold state to minimize changes
      std::optional<fmt::rgb> current_fg;
      std::optional<fmt::rgb> current_bg;
      bool current_bold = false;

      for (int col = 0; col < raster.cols (); ++col)
        {
          const HudCell &cell = raster.get_cell (row, col);

          fmt::rgb bg = cell.bg_color.value_or (default_bg_color);

          if (!current_bg || current_bg->r != bg.r || current_bg->g != bg.g
              || current_bg->b != bg.b)
            {
              fmt::format_to (std::back_inserter (output),
                              "\x1b[48;2;{};{};{}m", bg.r, bg.g, bg.b);
              current_bg = bg;
            }

          // Emit bold change if needed
          if (cell.bold != current_bold)
            {
              if (cell.bold)
                {
                  fmt::format_to (std::back_inserter (output), "\x1b[1m");
                }
              else
                {
                  // Reset bold (also resets color, so we'll need to re-emit)
                  fmt::format_to (std::back_inserter (output), "\x1b[22m");
                  current_fg.reset ();
                }
              current_bold = cell.bold;
            }

          // Emit foreground color change if needed
          if (cell.fg_color)
            {
              // Blend foreground color towards background based on alpha
              fmt::rgb blended_fg
                  = blend_color (*cell.fg_color, bg, cell.alpha);

              if (!current_fg || current_fg->r != blended_fg.r
                  || current_fg->g != blended_fg.g
                  || current_fg->b != blended_fg.b)
                {
                  fmt::format_to (std::back_inserter (output),
                                  "\x1b[38;2;{};{};{}m", blended_fg.r,
                                  blended_fg.g, blended_fg.b);
                  current_fg = blended_fg;
                }
            }
          else if (current_fg)
            {
              // Reset to default foreground (or blend with alpha if needed)
              if (cell.alpha < 1.0f)
                {
                  // Blend default white towards background
                  fmt::rgb blended
                      = blend_color (fmt::rgb (255, 255, 255), bg, cell.alpha);
                  fmt::format_to (std::back_inserter (output),
                                  "\x1b[38;2;{};{};{}m", blended.r, blended.g,
                                  blended.b);
                  current_fg = blended;
                }
              else
                {
                  fmt::format_to (std::back_inserter (output), "\x1b[39m");
                  current_fg.reset ();
                }
            }
          else if (cell.alpha < 1.0f)
            {
              // No explicit color, but has alpha - blend default white
              fmt::rgb blended
                  = blend_color (fmt::rgb (255, 255, 255), bg, cell.alpha);
              fmt::format_to (std::back_inserter (output),
                              "\x1b[38;2;{};{};{}m", blended.r, blended.g,
                              blended.b);
              current_fg = blended;
            }

          // Emit the glyph (as a string)
          fmt::format_to (std::back_inserter (output), "{}", cell.glyph);
        }

      rows.push_back (fmt::to_string (output));
    }

  return rows;
}

} // namespace nixb
