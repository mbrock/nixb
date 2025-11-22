#pragma once

#include "glyph-table.hpp"

#include <cstdint>
#include <fmt/color.h>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace nxb
{

/// Packed RGBA color (RGBA8888 format: 0xRRGGBBAA in memory)
/// Interoperable with fmt::rgb for seamless integration with fmt's color system.
struct Rgba8
{
  std::uint32_t value;

  /// Construct from RGBA components
  constexpr Rgba8 (std::uint8_t r, std::uint8_t g, std::uint8_t b,
                   std::uint8_t a = 255) noexcept
      : value (r | (g << 8) | (b << 16) | (a << 24))
  {
  }

  /// Construct from fmt::rgb (opaque)
  constexpr Rgba8 (fmt::rgb rgb, std::uint8_t a = 255) noexcept
      : value (rgb.r | (rgb.g << 8) | (rgb.b << 16) | (a << 24))
  {
  }

  /// Construct from fmt::color (opaque)
  constexpr Rgba8 (fmt::color c, std::uint8_t a = 255) noexcept
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
    return (value >> 8) & 0xFF;
  }
  [[nodiscard]] constexpr std::uint8_t
  b () const noexcept
  {
    return (value >> 16) & 0xFF;
  }
  [[nodiscard]] constexpr std::uint8_t
  a () const noexcept
  {
    return (value >> 24) & 0xFF;
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
class Raster
{
public:
  /// Default color for terminal cells (transparent = use terminal default)
  static constexpr Rgba8 DEFAULT_COLOR = Rgba8::transparent ();

  /// Initialize raster with given dimensions.
  /// All cells default to space (ASCII 32) with DEFAULT_COLOR.
  Raster (std::size_t width, std::size_t height);

  /// Dimensions
  [[nodiscard]] std::size_t
  width () const noexcept
  {
    return width_;
  }
  [[nodiscard]] std::size_t
  height () const noexcept
  {
    return height_;
  }
  [[nodiscard]] std::size_t
  size () const noexcept
  {
    return width_ * height_;
  }

  /// Set glyph at (x, y). Silently ignores out-of-bounds coordinates.
  void set_glyph (std::size_t x, std::size_t y,
                  GlyphTable::GlyphId gid) noexcept;

  /// Set foreground color at (x, y)
  void set_fg (std::size_t x, std::size_t y, Rgba8 color) noexcept;

  /// Set background color at (x, y)
  void set_bg (std::size_t x, std::size_t y, Rgba8 color) noexcept;

  /// Convenience: set ASCII character at (x, y)
  void
  set_char (std::size_t x, std::size_t y, char ch) noexcept
  {
    set_glyph (x, y, static_cast<GlyphTable::GlyphId> (ch));
  }

  /// Write UTF-8 text starting at (x, y) with given colors.
  /// Returns the ending column position (for chaining writes).
  /// Multi-byte UTF-8 sequences are interned via the glyph table.
  std::size_t write_text (std::size_t x, std::size_t y, std::string_view text,
                          GlyphTable &glyphs,
                          Rgba8 fg = DEFAULT_COLOR,
                          Rgba8 bg = DEFAULT_COLOR) noexcept;

  /// Fill a rectangle with a single glyph and foreground color.
  /// The background color is left unchanged.
  void fill_rect (std::size_t x, std::size_t y, std::size_t w, std::size_t h,
                  GlyphTable::GlyphId gid, Rgba8 fg_color);

  /// Clear entire raster to spaces with default colors
  void clear ();

  /// Direct access to SOA arrays (for diffing, inspection, etc.)
  [[nodiscard]] std::span<const GlyphTable::GlyphId>
  glyphs () const noexcept
  {
    return glyphs_;
  }
  [[nodiscard]] std::span<const Rgba8>
  fgs () const noexcept
  {
    return fgs_;
  }
  [[nodiscard]] std::span<const Rgba8>
  bgs () const noexcept
  {
    return bgs_;
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

private:
  std::size_t width_;
  std::size_t height_;

  /// SOA layout: separate vectors for each cell component
  std::vector<GlyphTable::GlyphId> glyphs_;
  std::vector<Rgba8> fgs_;
  std::vector<Rgba8> bgs_;
};

} // namespace nxb

