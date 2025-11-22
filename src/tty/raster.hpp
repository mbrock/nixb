#pragma once

#include "glyph-table.hpp"

#include <experimental/mdspan>
#include <fmt/color.h>
#include <optional>
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

  constexpr auto operator<=> (const Rgba8 &) const = default;
};

/// Terminal raster using Structure-of-Arrays layout for cache efficiency.
/// Each cell contains a glyph ID (maps to UTF-8 via GlyphTable), foreground
/// color, and background color.
///
/// Uses std::mdspan for efficient 2D indexing with zero overhead.
/// Can be either an owning raster (with storage) or a non-owning view
/// (subraster).
class Raster
{
public:
  /// Type aliases for mdspan views
  /// Using dynamic extents for both dimensions (rows, cols)
  using mdspan_extents
      = std::experimental::extents<std::size_t, std::dynamic_extent,
                                   std::dynamic_extent>;
  using glyph_view_t
      = std::experimental::mdspan<GlyphTable::GlyphId, mdspan_extents>;
  using const_glyph_view_t
      = std::experimental::mdspan<const GlyphTable::GlyphId, mdspan_extents>;
  using color_view_t = std::experimental::mdspan<Rgba8, mdspan_extents>;
  using const_color_view_t
      = std::experimental::mdspan<const Rgba8, mdspan_extents>;

  /// Default color for terminal cells (transparent = use terminal default)
  static constexpr Rgba8 DEFAULT_COLOR = Rgba8::transparent ();

  /// Initialize owning raster with given dimensions.
  /// All cells default to space (ASCII 32) with DEFAULT_COLOR.
  Raster (std::size_t width, std::size_t height);

  /// Create a non-owning view from existing mdspan views.
  /// Used internally for subraster views.
  Raster (const glyph_view_t &glyphs, const color_view_t &fgs,
          const color_view_t &bgs);

  /// Dimensions
  [[nodiscard]] std::size_t
  width () const noexcept
  {
    return glyph_view_.extent (1);
  }
  [[nodiscard]] std::size_t
  height () const noexcept
  {
    return glyph_view_.extent (0);
  }
  [[nodiscard]] std::size_t
  size () const noexcept
  {
    return width () * height ();
  }

  /// Create a non-owning subraster view of a rectangular region.
  /// The subraster behaves just like a full raster but references the parent's
  /// storage. Coordinates are relative to this raster (works recursively for
  /// nested views).
  [[nodiscard]] Raster subraster (std::size_t x, std::size_t y, std::size_t w,
                                  std::size_t h) const noexcept;

  /// Set glyph at (x, y). Silently ignores out-of-bounds coordinates.
  void set_glyph (std::size_t x, std::size_t y,
                  GlyphTable::GlyphId gid) const noexcept;

  /// Set foreground color at (x, y)
  void set_fg (std::size_t x, std::size_t y, Rgba8 color) const noexcept;

  /// Set background color at (x, y)
  void set_bg (std::size_t x, std::size_t y, Rgba8 color) const noexcept;

  /// Convenience: set ASCII character at (x, y)
  void
  set_char (const std::size_t x, const std::size_t y, const char ch) noexcept
  {
    set_glyph (x, y, static_cast<GlyphTable::GlyphId> (ch));
  }

  /// Write UTF-8 text starting at (x, y) with given colors.
  /// Returns the ending column position (for chaining writes).
  /// Multi-byte UTF-8 sequences are interned via the glyph table.
  std::size_t write_text (std::size_t x, std::size_t y, std::string_view text,
                          GlyphTable &glyphs, Rgba8 fg = DEFAULT_COLOR,
                          Rgba8 bg = DEFAULT_COLOR) noexcept;

  /// Fill a rectangle with a single glyph and foreground color.
  /// The background color is left unchanged.
  void fill_rect (std::size_t x, std::size_t y, std::size_t w, std::size_t h,
                  GlyphTable::GlyphId gid, Rgba8 fg_color);

  /// Clear entire raster to spaces with default colors
  void clear ();

  /// 2D mdspan views for natural (row, col) indexing
  [[nodiscard]] glyph_view_t
  glyphs_2d () noexcept
  {
    return glyph_view_;
  }
  [[nodiscard]] const_glyph_view_t
  glyphs_2d () const noexcept
  {
    return const_glyph_view_t{ glyph_view_.data_handle (),
                               mdspan_extents{ height (), width () } };
  }

  [[nodiscard]] color_view_t
  fgs_2d () noexcept
  {
    return fg_view_;
  }
  [[nodiscard]] const_color_view_t
  fgs_2d () const noexcept
  {
    return const_color_view_t{ fg_view_.data_handle (),
                               mdspan_extents{ height (), width () } };
  }

  [[nodiscard]] color_view_t
  bgs_2d () noexcept
  {
    return bg_view_;
  }
  [[nodiscard]] const_color_view_t
  bgs_2d () const noexcept
  {
    return const_color_view_t{ bg_view_.data_handle (),
                               mdspan_extents{ height (), width () } };
  }

  /// Direct access to SOA arrays (for diffing, inspection, etc.)
  /// Only available for owning rasters (not views).
  /// Returns empty span for non-owning views.
  [[nodiscard]] std::span<const GlyphTable::GlyphId>
  glyphs () const noexcept
  {
    return storage_glyphs_ ? std::span (*storage_glyphs_)
                           : std::span<const GlyphTable::GlyphId> ();
  }
  [[nodiscard]] std::span<const Rgba8>
  fgs () const noexcept
  {
    return storage_fgs_ ? std::span (*storage_fgs_)
                        : std::span<const Rgba8> ();
  }
  [[nodiscard]] std::span<const Rgba8>
  bgs () const noexcept
  {
    return storage_bgs_ ? std::span (*storage_bgs_)
                        : std::span<const Rgba8> ();
  }

  /// Get cell data at (x, y).
  /// Returns nullopt if out of bounds.
  struct Cell
  {
    GlyphTable::GlyphId glyph;
    Rgba8 fg;
    Rgba8 bg;
  };
  [[nodiscard]] std::optional<Cell> get_cell (std::size_t x,
                                              std::size_t y) const noexcept;

  /// Helper: count cells in a region that match a predicate
  template <typename Pred>
  [[nodiscard]] std::size_t
  count_if (const std::size_t x0, const std::size_t y0, std::size_t x1,
            std::size_t y1, Pred &&pred) const
  {
    const auto view = glyphs_2d ();
    std::size_t count = 0;

    // Clamp to raster bounds
    x1 = std::min (x1, width ());
    y1 = std::min (y1, height ());

    for (std::size_t y = y0; y < y1; ++y)
      {
        for (std::size_t x = x0; x < x1; ++x)
          {
            if (pred (view[y, x]))
              ++count;
          }
      }
    return count;
  }

private:
  /// mdspan views - always present, define the view into the data
  glyph_view_t glyph_view_;
  color_view_t fg_view_;
  color_view_t bg_view_;

  /// Storage - only present for owning rasters
  /// For non-owning views (subrasters), these are empty
  std::optional<std::vector<GlyphTable::GlyphId>> storage_glyphs_;
  std::optional<std::vector<Rgba8>> storage_fgs_;
  std::optional<std::vector<Rgba8>> storage_bgs_;
};

} // namespace nxb
