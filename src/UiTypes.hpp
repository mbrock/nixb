#pragma once

#include <cstdint>
#include <fmt/color.h>
#include <optional>
#include <string>
#include <vector>

namespace nixb
{

// Default background color for HUD
constexpr fmt::rgb DEFAULT_HUD_BG_COLOR{ 0, 0, 0 }; // { 20, 30, 40 };

enum class ProgressUnit
{
  Count, // Plain numbers (default)
  Bytes  // Byte sizes (show as KiB, MiB, GiB, etc.)
};

struct ActivityProgress
{
  int64_t done = 0;
  int64_t expected = 0;
  int64_t running = 0;
  int64_t failed = 0;
  ProgressUnit unit = ProgressUnit::Count;
};

struct UiActivityLine
{
  int64_t id = 0;
  std::string label;
  std::optional<std::string>
      url; // Optional URL for display (e.g., cache.nixos.org)
  std::optional<ActivityProgress> progress;
  bool is_finished = false;
  double fade_factor = 0.0;  // 0.0 = just finished (bright green), 1.0 = about
                             // to vanish (faded out)
  size_t num_input_deps = 0; // Number of input dependencies
  size_t num_dependents = 0; // Number of activities that depend on this one
};

struct UiState
{
  std::vector<UiActivityLine> activity_lines;
};

// HUD Raster System
// -----------------
// Represents the HUD as a grid of glyphs with optional colors.
// Colors default to white (fg) and DEFAULT_HUD_BG_COLOR (bg) when not set.

struct HudCell
{
  std::string glyph = " ";
  std::optional<fmt::rgb> fg_color; // nullopt = default (white)
  std::optional<fmt::rgb> bg_color; // nullopt = default (DEFAULT_HUD_BG_COLOR)
  float alpha = 1.0f; // Opacity: 1.0 = fully opaque, 0.0 = fully transparent
  bool bold = false;  // Whether to render text as bold
};

class HudRaster
{
public:
  HudRaster (int rows, int cols)
      : rows_ (rows), cols_ (cols), cells_ (rows * cols)
  {
  }

  int
  rows () const
  {
    return rows_;
  }

  int
  cols () const
  {
    return cols_;
  }

  // Set a cell at (row, col) with a glyph, optional colors, alpha, and bold
  void
  set_cell (int row, int col, std::string_view glyph,
            std::optional<fmt::rgb> fg_color = {},
            std::optional<fmt::rgb> bg_color = {}, float alpha = 1.0f,
            bool bold = false)
  {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_)
      return;
    int idx = row * cols_ + col;
    cells_[idx].glyph = glyph;
    cells_[idx].fg_color = fg_color;
    cells_[idx].bg_color = bg_color;
    cells_[idx].alpha = alpha;
    cells_[idx].bold = bold;
  }

  // Write a string at (row, col) with optional colors, alpha, and bold
  // Properly handles UTF-8 multi-byte characters
  void
  write_text (int row, int col, std::string_view text,
              std::optional<fmt::rgb> fg_color = {},
              std::optional<fmt::rgb> bg_color = {}, float alpha = 1.0f,
              bool bold = false)
  {
    int c = col;
    size_t i = 0;
    while (i < text.size () && c < cols_)
      {
        // Determine UTF-8 character byte length
        unsigned char byte = static_cast<unsigned char> (text[i]);
        size_t char_len = 1;

        if ((byte & 0x80) == 0)
          {
            // ASCII: 0xxxxxxx
            char_len = 1;
          }
        else if ((byte & 0xE0) == 0xC0)
          {
            // 2-byte: 110xxxxx
            char_len = 2;
          }
        else if ((byte & 0xF0) == 0xE0)
          {
            // 3-byte: 1110xxxx
            char_len = 3;
          }
        else if ((byte & 0xF8) == 0xF0)
          {
            // 4-byte: 11110xxx
            char_len = 4;
          }

        // Make sure we don't read past the end
        if (i + char_len > text.size ())
          char_len = text.size () - i;

        // Write the complete character to one cell
        set_cell (row, c, text.substr (i, char_len), fg_color, bg_color, alpha,
                  bold);

        i += char_len;
        ++c;
      }
  }

  // Get a cell (read-only)
  const HudCell &
  get_cell (int row, int col) const
  {
    static const HudCell empty;
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_)
      return empty;
    return cells_[row * cols_ + col];
  }

  // Clear entire raster
  void
  clear ()
  {
    for (auto &cell : cells_)
      {
        cell.glyph = " ";
        cell.fg_color.reset ();
        cell.bg_color.reset ();
      }
  }

  // Clear a specific row
  void
  clear_row (int row)
  {
    if (row < 0 || row >= rows_)
      return;
    for (int col = 0; col < cols_; ++col)
      {
        int idx = row * cols_ + col;
        cells_[idx].glyph = " ";
        cells_[idx].fg_color.reset ();
        cells_[idx].bg_color.reset ();
      }
  }

private:
  int rows_;
  int cols_;
  std::vector<HudCell> cells_;
};

} // namespace nixb
