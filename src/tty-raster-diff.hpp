#pragma once

#include "tty-raster.hpp"

#include <coro/generator.hpp>
#include <optional>
#include <span>

namespace nxb
{

/// A run of consecutive cells with changed content and consistent colors.
/// Designed to minimize ANSI escape sequences by batching color-consistent
/// regions.
struct ChangeRun
{
  std::size_t x; // Starting column
  std::size_t y; // Row

  /// Slice into the back raster's glyph array (zero-copy)
  std::span<const GlyphTable::GlyphId> glyphs;

  /// Foreground color change (nullopt = color unchanged from last run)
  std::optional<Rgba8> fg_change;

  /// Background color change (nullopt = color unchanged from last run)
  std::optional<Rgba8> bg_change;

  /// Reset foreground to terminal default
  bool fg_reset = false;

  /// Reset background to terminal default
  bool bg_reset = false;
};

/// Iterator state for diff scanning (hidden from public API)
namespace detail
{
struct DiffState
{
  const Raster *front;
  const Raster *back;
  std::optional<Rgba8> current_fg;
  std::optional<Rgba8> current_bg;
};

/// Find the first cell difference in a line starting from start_x
std::optional<std::size_t>
find_next_diff_in_line (const DiffState &state, std::size_t y,
                        std::size_t start_x) noexcept;

/// Find where the color-consistent run ends
std::size_t find_run_end (const DiffState &state, std::size_t y,
                          std::size_t start_x, std::optional<Rgba8> run_fg,
                          std::optional<Rgba8> run_bg) noexcept;

} // namespace detail

/// Generate change runs between two rasters using coroutines.
/// Automatically tracks terminal color state to minimize escape sequences.
///
/// Usage:
///   for (const auto& run : diff_rasters(front, back)) {
///       // Emit ANSI codes for run.fg_change, run.bg_change, run.fg_reset,
///       // etc. // Write run.glyphs to terminal
///   }
coro::generator<ChangeRun> diff_rasters (const Raster &front,
                                         const Raster &back);

} // namespace nxb
