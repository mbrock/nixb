#pragma once

#include "raster.hpp"

#include <optional>
#include <ranges>
#include <span>

namespace nxb
{

// ============================================================================
// Data types
// ============================================================================

/// A detected change: position, glyphs, and colors (before color optimization).
struct RawChange
{
  Pos origin;
  std::span<const GlyphTable::GlyphId> glyphs;
  Rgba8 fg;
  Rgba8 bg;
};

/// A run of changed cells, with color deltas for minimal ANSI output.
struct ChangeRun
{
  Pos origin;
  std::span<const GlyphTable::GlyphId> glyphs;
  std::optional<Rgba8> fg_change;
  std::optional<Rgba8> bg_change;
  bool fg_reset = false;
  bool bg_reset = false;
};

/// Tracks terminal color state, converting RawChange → ChangeRun with deltas.
struct ColorState
{
  std::optional<Rgba8> fg;
  std::optional<Rgba8> bg;

  ChangeRun
  operator() (const RawChange &raw)
  {
    ChangeRun run{ raw.origin, raw.glyphs, {}, {}, false, false };

    if (raw.fg == DEFAULT_COLOR && fg)
      {
        run.fg_reset = true;
        fg = std::nullopt;
      }
    else if (raw.fg != DEFAULT_COLOR && raw.fg != fg)
      {
        run.fg_change = raw.fg;
        fg = raw.fg;
      }

    if (raw.bg == DEFAULT_COLOR && bg)
      {
        run.bg_reset = true;
        bg = std::nullopt;
      }
    else if (raw.bg != DEFAULT_COLOR && raw.bg != bg)
      {
        run.bg_change = raw.bg;
        bg = raw.bg;
      }

    return run;
  }
};

// ============================================================================
// Pipeline building blocks
// ============================================================================

/// Did this cell change? (comparing old vs new from a zipped pair)
constexpr auto is_changed = [] (const auto &pair) {
  const auto &[old_cell, new_cell] = pair;
  return old_cell != new_cell;
};

/// Should two cell pairs belong in the same run?
/// Yes if: both changed (or both unchanged) AND same colors in new buffer.
constexpr auto same_run = [] (const auto &a, const auto &b) {
  const auto &[old_a, new_a] = a;
  const auto &[old_b, new_b] = b;
  return is_changed (a) == is_changed (b)  // same change status
         && new_a.fg == new_b.fg           // same foreground
         && new_a.bg == new_b.bg;          // same background
};

// ============================================================================
// The diff pipeline
// ============================================================================

/// Extract changed runs from a single row.
/// Groups cells by run boundaries, keeps only changed runs, converts to
/// RawChange.
inline auto
row_changes (height_t y, auto cells, const Raster &back)
{
  return cells
         | std::views::chunk_by (same_run)
         | std::views::filter ([] (auto chunk) {
             return is_changed (*chunk.begin ());
           })
         | std::views::transform ([y, &back] (auto chunk) {
             const auto &[old_cell, new_cell] = *chunk.begin ();
             return RawChange{
               .origin = Pos::at (new_cell.col, y),
               .glyphs = back.glyph_span (y, new_cell.col,
                                          std::ranges::distance (chunk)),
               .fg = new_cell.fg,
               .bg = new_cell.bg,
             };
           });
}

/// Iterate changed regions between two rasters as a lazy range.
///
/// Pipeline:
///   zip_rows(front, back)    -- pair up rows from both rasters
///   | transform(row_changes) -- extract changed runs from each row
///   | join                   -- flatten into single stream
///
inline auto
raw_changes (const Raster &front, const Raster &back)
{
  return zip_rows (front, back)
         | std::views::transform ([&back] (auto row_pair) {
             auto [y, cells] = row_pair;
             return row_changes (y, cells, back);
           })
         | std::views::join;
}

/// Iterate changed regions, tracking color state for minimal ANSI output.
template <typename F>
void
diff_rasters (const Raster &front, const Raster &back, F &&emit)
{
  ColorState colors;
  for (const auto &raw : raw_changes (front, back))
    emit (colors (raw));
}

} // namespace nxb
