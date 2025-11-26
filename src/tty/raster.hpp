#pragma once

#include "glyph-table.hpp"
#include "units.hpp"

#include <experimental/mdspan>
#include <fmt/color.h>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace nxb
{

/// Packed RGBA color (RGBA8888 format: 0xRRGGBBAA in memory)
/// Interoperable with fmt::rgb for seamless integration with fmt's color
/// system.
struct Rgba8
{
  std::uint32_t value;

  /// Construct from RGBA components
  constexpr Rgba8 (const std::uint8_t r, const std::uint8_t g,
                   const std::uint8_t b, const std::uint8_t a = 255) noexcept
      : value (r | g << 8 | b << 16 | a << 24)
  {
  }

  /// Construct from fmt::rgb (opaque)
  constexpr Rgba8 (const fmt::rgb rgb, const std::uint8_t a = 255) noexcept
      : value (rgb.r | rgb.g << 8 | rgb.b << 16 | a << 24)
  {
  }

  /// Construct from fmt::color (opaque)
  constexpr Rgba8 (const fmt::color c, const std::uint8_t a = 255) noexcept
      : Rgba8 (fmt::rgb (c), a)
  {
  }

  /// Transparent black (default for terminal)
  static constexpr Rgba8
  transparent () noexcept
  {
    return { 0, 0, 0, 0 };
  }

  /// Opaque white
  static constexpr Rgba8
  white () noexcept
  {
    return { 255, 255, 255, 255 };
  }

  /// Extract color components
  [[nodiscard]] constexpr std::uint8_t
  r () const noexcept
  {
    return value & 0xFF;
  }
  [[nodiscard]] constexpr std::uint8_t
  g () const noexcept
  {
    return value >> 8 & 0xFF;
  }
  [[nodiscard]] constexpr std::uint8_t
  b () const noexcept
  {
    return value >> 16 & 0xFF;
  }
  [[nodiscard]] constexpr std::uint8_t
  a () const noexcept
  {
    return value >> 24 & 0xFF;
  }

  /// Convert to fmt::rgb (discards alpha)
  [[nodiscard]] constexpr fmt::rgb
  to_rgb () const noexcept
  {
    return fmt::rgb (r (), g (), b ());
  }

  /// Format
  friend std::ostream &operator<< (std::ostream &os, const Rgba8 &c);

  constexpr auto operator<=> (const Rgba8 &) const = default;
};

// ============================================================================
// Type aliases for mdspan views
// ============================================================================

using mdspan_extents = std::experimental::extents<std::size_t,
                                                   std::dynamic_extent,
                                                   std::dynamic_extent>;
using glyph_view_t
    = std::experimental::mdspan<GlyphTable::GlyphId, mdspan_extents>;
using const_glyph_view_t
    = std::experimental::mdspan<const GlyphTable::GlyphId, mdspan_extents>;
using color_view_t = std::experimental::mdspan<Rgba8, mdspan_extents>;
using const_color_view_t
    = std::experimental::mdspan<const Rgba8, mdspan_extents>;

/// Default color for terminal cells (transparent = use terminal default)
inline constexpr Rgba8 DEFAULT_COLOR = Rgba8::transparent ();

// ============================================================================
// mdspan to range adapter
// ============================================================================

/// Convert a 2D mdspan to a flat range (row-major order).
/// Works with any layout (contiguous or strided from submdspan).
template <typename T, typename Extents, typename Layout, typename Accessor>
auto
as_range (std::experimental::mdspan<T, Extents, Layout, Accessor> m)
{
  const auto rows = m.extent (0);
  const auto cols = m.extent (1);
  return std::views::iota (std::size_t{ 0 }, rows * cols)
         | std::views::transform ([=] (std::size_t i) -> T & {
             return m[i / cols, i % cols];
           });
}

/// Get a single row from a 2D mdspan as a range.
template <typename T, typename Extents, typename Layout, typename Accessor>
auto
row_range (std::experimental::mdspan<T, Extents, Layout, Accessor> m,
           std::size_t row_idx)
{
  const auto cols = m.extent (1);
  return std::views::iota (std::size_t{ 0 }, cols)
         | std::views::transform (
             [=] (std::size_t col) -> T & { return m[row_idx, col]; });
}

/// Get an indexed row range (pairs of column index and value reference).
template <typename T, typename Extents, typename Layout, typename Accessor>
auto
indexed_row (std::experimental::mdspan<T, Extents, Layout, Accessor> m,
             std::size_t row_idx)
{
  const auto cols = m.extent (1);
  return std::views::iota (std::size_t{ 0 }, cols)
         | std::views::transform ([=] (std::size_t col) {
             return std::pair<std::size_t, T &>{ col, m[row_idx, col] };
           });
}

/// A cell with its column position for iteration.
struct IndexedCell
{
  width_t col;
  GlyphTable::GlyphId glyph;
  Rgba8 fg;
  Rgba8 bg;

  bool
  operator== (const IndexedCell &other) const
  {
    return glyph == other.glyph && fg == other.fg && bg == other.bg;
  }
};

/// Get a row as indexed cells (col, glyph, fg, bg).
inline auto
indexed_cell_row (const_glyph_view_t glyphs, const_color_view_t fgs,
                  const_color_view_t bgs, std::size_t row_idx)
{
  const auto cols = glyphs.extent (1);
  return std::views::iota (std::size_t{ 0 }, cols)
         | std::views::transform ([=] (std::size_t x) {
             return IndexedCell{ x * ch, glyphs[row_idx, x], fgs[row_idx, x],
                                 bgs[row_idx, x] };
           });
}

// ============================================================================
// RasterView - non-owning view into raster storage
// ============================================================================

/// Cell data for inspection
struct Cell
{
  GlyphTable::GlyphId glyph;
  Rgba8 fg;
  Rgba8 bg;
};

/// Non-owning view into raster storage. This is the primary working type
/// for all rendering operations. Views can create sub-views (subraster)
/// for hierarchical layout.
class RasterView
{
public:
  /// Construct from mdspan views and glyph table
  RasterView (glyph_view_t glyphs, color_view_t fgs, color_view_t bgs,
              GlyphTable &glyph_table) noexcept
      : glyphs_ (glyphs), fgs_ (fgs), bgs_ (bgs), glyph_table_ (&glyph_table)
  {
  }

  /// Dimensions
  [[nodiscard]] width_t
  width () const noexcept
  {
    return glyphs_.extent (1) * ch;
  }
  [[nodiscard]] height_t
  height () const noexcept
  {
    return glyphs_.extent (0) * ln;
  }
  [[nodiscard]] Size
  extent () const noexcept
  {
    return { width (), height () };
  }

  /// Create a sub-view of a rectangular region.
  /// Coordinates are relative to this view.
  [[nodiscard]] RasterView subraster (Pos origin, Size size) const noexcept;

  /// Set glyph at position. Silently ignores out-of-bounds.
  void set_glyph (Pos pos, GlyphTable::GlyphId gid) const noexcept;

  /// Set foreground color at position
  void set_fg (Pos pos, Rgba8 color) const noexcept;

  /// Set background color at position
  void set_bg (Pos pos, Rgba8 color) const noexcept;

  /// Convenience: set ASCII character
  void
  set_char (Pos pos, char c) const noexcept
  {
    set_glyph (pos, static_cast<GlyphTable::GlyphId> (c));
  }

  /// Write UTF-8 text. Returns ending column position.
  col_t write_text (Pos pos, std::string_view text) const noexcept;

  /// Get cell data. Returns nullopt if out of bounds.
  [[nodiscard]] std::optional<Cell> get_cell (Pos pos) const noexcept;

  /// 2D mdspan views for direct access
  [[nodiscard]] glyph_view_t
  glyphs_2d () const noexcept
  {
    return glyphs_;
  }
  [[nodiscard]] color_view_t
  fgs_2d () const noexcept
  {
    return fgs_;
  }
  [[nodiscard]] color_view_t
  bgs_2d () const noexcept
  {
    return bgs_;
  }

  /// Flat ranges for algorithms (row-major order)
  [[nodiscard]] auto
  glyphs () const
  {
    return as_range (glyphs_);
  }
  [[nodiscard]] auto
  fgs () const
  {
    return as_range (fgs_);
  }
  [[nodiscard]] auto
  bgs () const
  {
    return as_range (bgs_);
  }

  /// Access glyph table
  [[nodiscard]] GlyphTable &
  glyph_table () const noexcept
  {
    return *glyph_table_;
  }

private:
  glyph_view_t glyphs_;
  color_view_t fgs_;
  color_view_t bgs_;
  GlyphTable *glyph_table_;
};

// ============================================================================
// Raster - owning storage that produces views
// ============================================================================

/// Owning raster storage. Allocates and manages the underlying arrays.
/// Use view() to get a RasterView for rendering operations.
class Raster
{
public:
  /// Initialize with given dimensions.
  /// All cells default to space (ASCII 32) with DEFAULT_COLOR.
  Raster (std::size_t width, std::size_t height, GlyphTable &glyphs);
  Raster (width_t width, height_t height, GlyphTable &glyphs);
  Raster (Size size, GlyphTable &glyphs);

  /// Get a view of the entire raster
  [[nodiscard]] RasterView view () noexcept;

  /// Implicit conversion to view (convenience)
  operator RasterView () noexcept { return view (); }

  /// Dimensions
  [[nodiscard]] width_t
  width () const noexcept
  {
    return width_;
  }
  [[nodiscard]] height_t
  height () const noexcept
  {
    return height_;
  }
  [[nodiscard]] Size
  extent () const noexcept
  {
    return { width_, height_ };
  }

  /// Clear to spaces with default colors
  void clear ();

  /// Direct access to storage (for diffing)
  [[nodiscard]] std::span<const GlyphTable::GlyphId>
  glyphs () const noexcept
  {
    return glyphs_storage_;
  }
  [[nodiscard]] std::span<const Rgba8>
  fgs () const noexcept
  {
    return fgs_storage_;
  }
  [[nodiscard]] std::span<const Rgba8>
  bgs () const noexcept
  {
    return bgs_storage_;
  }

  /// Get a span of glyphs for a region on a row
  [[nodiscard]] std::span<const GlyphTable::GlyphId>
  glyph_span (height_t y, width_t x, std::size_t len) const noexcept
  {
    const auto cols = width_.numerical_value_in (ch);
    const auto offset = y.numerical_value_in (ln) * cols
                        + x.numerical_value_in (ch);
    return std::span{ glyphs_storage_ }.subspan (offset, len);
  }

  /// 2D views (const, for diffing)
  [[nodiscard]] const_glyph_view_t glyphs_2d () const noexcept;
  [[nodiscard]] const_color_view_t fgs_2d () const noexcept;
  [[nodiscard]] const_color_view_t bgs_2d () const noexcept;

  /// Get a row as indexed cells for iteration
  [[nodiscard]] auto
  row (height_t y) const
  {
    return indexed_cell_row (glyphs_2d (), fgs_2d (), bgs_2d (),
                             y.numerical_value_in (ln));
  }

  /// Iterate rows (just the row ranges)
  [[nodiscard]] auto
  rows () const
  {
    return std::views::iota (std::size_t{ 0 },
                             height_.numerical_value_in (ln))
           | std::views::transform (
               [this] (std::size_t y) { return row (y * ln); });
  }

  /// Iterate rows with their y coordinate: (height_t, row_range)
  [[nodiscard]] auto
  indexed_rows () const
  {
    return std::views::iota (std::size_t{ 0 },
                             height_.numerical_value_in (ln))
           | std::views::transform ([this] (std::size_t yi) {
               const auto y = yi * ln;
               return std::pair{ y, row (y) };
             });
  }

  /// Access glyph table
  [[nodiscard]] GlyphTable &
  glyph_table () const noexcept
  {
    return *glyph_table_;
  }

private:
  width_t width_;
  height_t height_;
  std::vector<GlyphTable::GlyphId> glyphs_storage_;
  std::vector<Rgba8> fgs_storage_;
  std::vector<Rgba8> bgs_storage_;
  GlyphTable *glyph_table_;
};

// ============================================================================
// Row-based iteration helpers
// ============================================================================

/// Zip two rasters' rows together for comparison.
/// Yields (height_t y, zipped_row) where zipped_row pairs corresponding cells.
inline auto
zip_rows (const Raster &front, const Raster &back)
{
  return std::views::iota (std::size_t{ 0 },
                           back.height ().numerical_value_in (ln))
         | std::views::transform ([&] (std::size_t yi) {
             const auto y = yi * ln;
             return std::pair{ y,
                               std::views::zip (front.row (y), back.row (y)) };
           });
}

} // namespace nxb
