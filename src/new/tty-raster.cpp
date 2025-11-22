#include "tty-raster.hpp"

#include <algorithm>
#include <cstring>
#include <experimental/mdspan>

namespace nxb
{

// Owning constructor
Raster::Raster (std::size_t width, std::size_t height)
    : glyph_view_ (nullptr, mdspan_extents{ height, width }),
      fg_view_ (nullptr, mdspan_extents{ height, width }),
      bg_view_ (nullptr, mdspan_extents{ height, width }),
      storage_glyphs_ (std::vector<GlyphTable::GlyphId> (width * height, 32)),
      storage_fgs_ (std::vector<Rgba8> (width * height, DEFAULT_COLOR)),
      storage_bgs_ (std::vector<Rgba8> (width * height, DEFAULT_COLOR))
{
  // Update views to point to the allocated storage
  glyph_view_ = glyph_view_t (storage_glyphs_->data (),
                              mdspan_extents{ height, width });
  fg_view_
      = color_view_t (storage_fgs_->data (), mdspan_extents{ height, width });
  bg_view_
      = color_view_t (storage_bgs_->data (), mdspan_extents{ height, width });
}

// Non-owning view constructor
Raster::Raster (glyph_view_t glyphs, color_view_t fgs, color_view_t bgs)
    : glyph_view_ (glyphs), fg_view_ (fgs), bg_view_ (bgs)
{
}

void
Raster::set_glyph (std::size_t x, std::size_t y,
                   GlyphTable::GlyphId gid) noexcept
{
  if (x >= width () || y >= height ())
    return;

  glyph_view_[y, x] = gid;
}

void
Raster::set_fg (std::size_t x, std::size_t y, Rgba8 color) noexcept
{
  if (x >= width () || y >= height ())
    return;

  fg_view_[y, x] = color;
}

void
Raster::set_bg (std::size_t x, std::size_t y, Rgba8 color) noexcept
{
  if (x >= width () || y >= height ())
    return;

  bg_view_[y, x] = color;
}

std::size_t
Raster::write_text (std::size_t x, std::size_t y, std::string_view text,
                    GlyphTable &glyphs, Rgba8 fg, Rgba8 bg) noexcept
{
  if (y >= height ())
    return x;

  auto glyph_view = glyphs_2d ();
  auto fg_view = fgs_2d ();
  auto bg_view = bgs_2d ();

  std::size_t col = x;
  std::size_t i = 0;

  while (i < text.size () && col < width ())
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

      // Write to raster using mdspan 2D indexing
      glyph_view[y, col] = gid;
      fg_view[y, col] = fg;
      bg_view[y, col] = bg;

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
  const std::size_t x0 = std::min (x, width ());
  const std::size_t y0 = std::min (y, height ());
  const std::size_t x1 = std::min (x + w, width ());
  const std::size_t y1 = std::min (y + h, height ());

  if (x0 >= x1 || y0 >= y1)
    return;

  // Get 2D views
  auto glyph_view = glyphs_2d ();
  auto fg_view = fgs_2d ();

  // Fill using natural 2D iteration
  for (std::size_t row = y0; row < y1; ++row)
    {
      for (std::size_t col = x0; col < x1; ++col)
        {
          glyph_view[row, col] = gid;
          fg_view[row, col] = fg_color;
        }
    }
}

void
Raster::clear ()
{
  // Clear only works for owning rasters
  if (storage_glyphs_ && storage_fgs_ && storage_bgs_)
    {
      std::fill (storage_glyphs_->begin (), storage_glyphs_->end (),
                 32); // Space
      std::fill (storage_fgs_->begin (), storage_fgs_->end (), DEFAULT_COLOR);
      std::fill (storage_bgs_->begin (), storage_bgs_->end (), DEFAULT_COLOR);
    }
}

std::optional<Raster::Cell>
Raster::get_cell (std::size_t x, std::size_t y) const noexcept
{
  if (x >= width () || y >= height ())
    return std::nullopt;

  auto glyph_view = glyphs_2d ();
  auto fg_view = fgs_2d ();
  auto bg_view = bgs_2d ();

  return Cell{ glyph_view[y, x], fg_view[y, x], bg_view[y, x] };
}

Raster
Raster::subraster (std::size_t x, std::size_t y, std::size_t w,
                   std::size_t h) noexcept
{
  using std::submdspan;

  // Clamp to raster bounds
  const std::size_t x0 = std::min (x, width ());
  const std::size_t y0 = std::min (y, height ());
  const std::size_t x1 = std::min (x + w, width ());
  const std::size_t y1 = std::min (y + h, height ());

  // Create subviews using submdspan
  auto glyph_sub
      = submdspan (glyph_view_, std::pair{ y0, y1 }, std::pair{ x0, x1 });
  auto fg_sub = submdspan (fg_view_, std::pair{ y0, y1 }, std::pair{ x0, x1 });
  auto bg_sub = submdspan (bg_view_, std::pair{ y0, y1 }, std::pair{ x0, x1 });

  // Return a non-owning Raster view
  return Raster (glyph_sub, fg_sub, bg_sub);
}

} // namespace nxb
