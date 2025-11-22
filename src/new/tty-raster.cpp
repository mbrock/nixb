#include "tty-raster.hpp"

#include <algorithm>
#include <cstring>

namespace nxb
{

Raster::Raster (std::size_t width, std::size_t height)
    : width_ (width), height_ (height)
{
  const std::size_t n = width * height;

  // Initialize all cells to space (ASCII 32) with default colors
  glyphs_.resize (n, 32); // Space character
  fgs_.resize (n, DEFAULT_COLOR);
  bgs_.resize (n, DEFAULT_COLOR);
}

void
Raster::set_glyph (std::size_t x, std::size_t y,
                   GlyphTable::GlyphId gid) noexcept
{
  if (x >= width_ || y >= height_)
    return;

  const std::size_t idx = y * width_ + x;
  glyphs_[idx] = gid;
}

void
Raster::set_fg (std::size_t x, std::size_t y, Rgba8 color) noexcept
{
  if (x >= width_ || y >= height_)
    return;

  const std::size_t idx = y * width_ + x;
  fgs_[idx] = color;
}

void
Raster::set_bg (std::size_t x, std::size_t y, Rgba8 color) noexcept
{
  if (x >= width_ || y >= height_)
    return;

  const std::size_t idx = y * width_ + x;
  bgs_[idx] = color;
}

std::size_t
Raster::write_text (std::size_t x, std::size_t y, std::string_view text,
                    GlyphTable &glyphs, Rgba8 fg, Rgba8 bg) noexcept
{
  if (y >= height_)
    return x;

  std::size_t col = x;
  std::size_t i = 0;

  while (i < text.size () && col < width_)
    {
      // Determine UTF-8 character byte length
      const auto byte = static_cast<unsigned char> (text[i]);
      std::size_t char_len = 1;

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

      // Clamp to available data
      if (i + char_len > text.size ())
        char_len = text.size () - i;

      // Intern the UTF-8 sequence and get glyph ID
      std::string_view glyph_bytes = text.substr (i, char_len);
      GlyphTable::GlyphId gid = glyphs.intern (glyph_bytes);

      // Write to raster
      const std::size_t idx = y * width_ + col;
      glyphs_[idx] = gid;
      fgs_[idx] = fg;
      bgs_[idx] = bg;

      i += char_len;
      ++col;
    }

  return col;
}

void
Raster::fill_rect (std::size_t x, std::size_t y, std::size_t w, std::size_t h,
                   GlyphTable::GlyphId gid, Rgba8 fg_color)
{
  if (w == 0 || h == 0)
    return;

  // Clamp to raster bounds
  const std::size_t x0 = std::min (x, width_);
  const std::size_t y0 = std::min (y, height_);
  const std::size_t x1 = std::min (x + w, width_);
  const std::size_t y1 = std::min (y + h, height_);

  if (x0 >= x1 || y0 >= y1)
    return;

  // Fill row by row using memset for better performance
  for (std::size_t yy = y0; yy < y1; ++yy)
    {
      const std::size_t row_start = yy * width_ + x0;
      const std::size_t row_end = yy * width_ + x1;

      std::fill (glyphs_.begin () + row_start, glyphs_.begin () + row_end,
                 gid);
      std::fill (fgs_.begin () + row_start, fgs_.begin () + row_end,
                 fg_color);
    }
}

void
Raster::clear ()
{
  std::fill (glyphs_.begin (), glyphs_.end (), 32); // Space
  std::fill (fgs_.begin (), fgs_.end (), DEFAULT_COLOR);
  std::fill (bgs_.begin (), bgs_.end (), DEFAULT_COLOR);
}

std::optional<Raster::Cell>
Raster::get_cell (std::size_t x, std::size_t y) const noexcept
{
  if (x >= width_ || y >= height_)
    return std::nullopt;

  const std::size_t idx = y * width_ + x;
  return Cell{ glyphs_[idx], fgs_[idx], bgs_[idx] };
}

} // namespace nxb

