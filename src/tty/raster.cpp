#include "raster.hpp"

#include <algorithm>
#include <cstring>
#include <experimental/mdspan>

namespace nxb
{

// Owning constructor
Raster::Raster (const std::size_t width, const std::size_t height,
                GlyphTable &glyphs)
    : glyph_view_ (nullptr, mdspan_extents{ height, width }),
      fg_view_ (nullptr, mdspan_extents{ height, width }),
      bg_view_ (nullptr, mdspan_extents{ height, width }),
      storage_glyphs_ (std::vector<GlyphTable::GlyphId> (width * height, 32)),
      storage_fgs_ (std::vector (width * height, DEFAULT_COLOR)),
      storage_bgs_ (std::vector (width * height, DEFAULT_COLOR)),
      glyphs_ (&glyphs)
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
Raster::Raster (const glyph_view_t &glyphs, const color_view_t &fgs,
                const color_view_t &bgs)
    : glyph_view_ (glyphs), fg_view_ (fgs), bg_view_ (bgs), glyphs_ (nullptr)
{
  // Note: glyphs_ will be set when creating subrasters via subraster()
}

// Copy assignment operator
Raster &
Raster::operator= (const Raster &other)
{
  if (this == &other)
    return *this;

  // Copy storage (deep copy if present)
  storage_glyphs_ = other.storage_glyphs_;
  storage_fgs_ = other.storage_fgs_;
  storage_bgs_ = other.storage_bgs_;

  // Copy glyph table pointer
  glyphs_ = other.glyphs_;

  // Reconstruct mdspan views to point at our own storage
  if (storage_glyphs_)
    {
      const auto h = other.height ();
      const auto w = other.width ();
      glyph_view_
          = glyph_view_t (storage_glyphs_->data (), mdspan_extents{ h, w });
      fg_view_ = color_view_t (storage_fgs_->data (), mdspan_extents{ h, w });
      bg_view_ = color_view_t (storage_bgs_->data (), mdspan_extents{ h, w });
    }
  else
    {
      // Non-owning view: just copy the views
      glyph_view_ = other.glyph_view_;
      fg_view_ = other.fg_view_;
      bg_view_ = other.bg_view_;
    }

  return *this;
}

void
Raster::set_glyph (const std::size_t x, const std::size_t y,
                   const GlyphTable::GlyphId gid) const noexcept
{
  if (x >= width () || y >= height ())
    return;

  glyph_view_[y, x] = gid;
}

void
Raster::set_fg (const std::size_t x, const std::size_t y,
                const Rgba8 color) const noexcept
{
  if (x >= width () || y >= height ())
    return;

  fg_view_[y, x] = color;
}

void
Raster::set_bg (const std::size_t x, const std::size_t y,
                const Rgba8 color) const noexcept
{
  if (x >= width () || y >= height ())
    return;

  bg_view_[y, x] = color;
}

std::size_t
Raster::write_text (const std::size_t x, const std::size_t y,
                    const std::string_view text, GlyphTable &glyphs,
                    const Rgba8 fg, const Rgba8 bg) noexcept
{
  if (y >= height ())
    return x;

  const auto glyph_view = glyphs_2d ();
  const auto fg_view = fgs_2d ();
  const auto bg_view = bgs_2d ();

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
      const std::string_view glyph_bytes = text.substr (i, char_len);
      const GlyphTable::GlyphId gid = glyphs.intern (glyph_bytes);

      // Write to raster using mdspan 2D indexing
      glyph_view[y, col] = gid;
      fg_view[y, col] = fg;
      bg_view[y, col] = bg;

      i += char_len;
      ++col;
    }

  return col;
}

std::size_t
Raster::write_text (const std::size_t x, const std::size_t y,
                    const std::string_view text, const Rgba8 fg,
                    const Rgba8 bg) noexcept
{
  if (!glyphs_)
    return x; // No glyph table, can't render

  return write_text (x, y, text, *glyphs_, fg, bg);
}

void
Raster::fill_rect (const std::size_t x, const std::size_t y,
                   const std::size_t w, const std::size_t h,
                   const GlyphTable::GlyphId gid, const Rgba8 fg_color)
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
  const auto glyph_view = glyphs_2d ();
  const auto fg_view = fgs_2d ();

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
      std::ranges::fill (*storage_glyphs_,
                         32); // Space
      std::ranges::fill (*storage_fgs_, DEFAULT_COLOR);
      std::ranges::fill (*storage_bgs_, DEFAULT_COLOR);
    }
}

std::optional<Raster::Cell>
Raster::get_cell (const std::size_t x, const std::size_t y) const noexcept
{
  if (x >= width () || y >= height ())
    return std::nullopt;

  const auto glyph_view = glyphs_2d ();
  const auto fg_view = fgs_2d ();
  const auto bg_view = bgs_2d ();

  return Cell{ glyph_view[y, x], fg_view[y, x], bg_view[y, x] };
}

Raster
Raster::subraster (const std::size_t x, const std::size_t y,
                   const std::size_t w, const std::size_t h) const noexcept
{
  using std::submdspan;

  // Clamp to raster bounds
  const std::size_t x0 = std::min (x, width ());
  const std::size_t y0 = std::min (y, height ());
  const std::size_t x1 = std::min (x + w, width ());
  const std::size_t y1 = std::min (y + h, height ());

  // Create subviews using submdspan
  const auto glyph_sub
      = submdspan (glyph_view_, std::pair{ y0, y1 }, std::pair{ x0, x1 });
  const auto fg_sub
      = submdspan (fg_view_, std::pair{ y0, y1 }, std::pair{ x0, x1 });
  const auto bg_sub
      = submdspan (bg_view_, std::pair{ y0, y1 }, std::pair{ x0, x1 });

  // Return a non-owning Raster view with inherited glyph table
  Raster sub (glyph_sub, fg_sub, bg_sub);
  sub.glyphs_ = glyphs_;
  return sub;
}

} // namespace nxb
