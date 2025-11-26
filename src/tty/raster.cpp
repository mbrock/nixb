#include "raster.hpp"

#include <algorithm>
#include <experimental/mdspan>
#include <fmt/core.h>
#include <fmt/ostream.h>

namespace nxb
{

namespace
{

inline std::size_t
cols_from (width_t w)
{
  return w.numerical_value_in (ch);
}

inline std::size_t
rows_from (height_t h)
{
  return h.numerical_value_in (ln);
}

} // namespace

// Rgba8 output formatting
std::ostream &
operator<< (std::ostream &os, const Rgba8 &c)
{
  fmt::print (os, "rgba8({:02x},{:02x},{:02x},{:02x})", c.r (), c.g (), c.b (),
              c.a ());
  return os;
}

// ============================================================================
// RasterView implementation
// ============================================================================

void
RasterView::set_glyph (const Pos pos, const GlyphTable::GlyphId gid) const
    noexcept
{
  const auto x = pos.col ();
  const auto y = pos.row ();
  if (x >= glyphs_.extent (1) || y >= glyphs_.extent (0))
    return;

  glyphs_[y, x] = gid;
}

void
RasterView::set_fg (const Pos pos, const Rgba8 color) const noexcept
{
  const auto x = pos.col ();
  const auto y = pos.row ();
  if (x >= glyphs_.extent (1) || y >= glyphs_.extent (0))
    return;

  fgs_[y, x] = color;
}

void
RasterView::set_bg (const Pos pos, const Rgba8 color) const noexcept
{
  const auto x = pos.col ();
  const auto y = pos.row ();
  if (x >= glyphs_.extent (1) || y >= glyphs_.extent (0))
    return;

  bgs_[y, x] = color;
}

col_t
RasterView::write_text (const Pos pos, const std::string_view text) const
    noexcept
{
  const auto rows = glyphs_.extent (0);
  const auto cols = glyphs_.extent (1);
  const auto y = pos.row ();

  if (y >= rows)
    return pos.x;

  std::size_t col = pos.col ();
  std::size_t i = 0;

  while (i < text.size () && col < cols)
    {
      // Determine UTF-8 character byte length
      const auto byte = static_cast<unsigned char> (text[i]);
      std::size_t char_len = 1;

      if ((byte & 0x80) == 0)
        char_len = 1;
      else if ((byte & 0xE0) == 0xC0)
        char_len = 2;
      else if ((byte & 0xF0) == 0xE0)
        char_len = 3;
      else if ((byte & 0xF8) == 0xF0)
        char_len = 4;

      if (i + char_len > text.size ())
        char_len = text.size () - i;

      const std::string_view glyph_bytes = text.substr (i, char_len);
      const GlyphTable::GlyphId gid = glyph_table_->intern (glyph_bytes);

      glyphs_[y, col] = gid;

      i += char_len;
      ++col;
    }

  return terminal_origin + col * ch;
}

std::optional<Cell>
RasterView::get_cell (const Pos pos) const noexcept
{
  const auto x = pos.col ();
  const auto y = pos.row ();
  if (x >= glyphs_.extent (1) || y >= glyphs_.extent (0))
    return std::nullopt;

  return Cell{ glyphs_[y, x], fgs_[y, x], bgs_[y, x] };
}

RasterView
RasterView::subraster (const Pos origin, const Size size) const noexcept
{
  using std::submdspan;

  const std::size_t x = origin.col ();
  const std::size_t y = origin.row ();
  const std::size_t w = cols_from (size.w);
  const std::size_t h = rows_from (size.h);

  const std::size_t x0 = std::min (x, glyphs_.extent (1));
  const std::size_t y0 = std::min (y, glyphs_.extent (0));
  const std::size_t x1 = std::min (x + w, glyphs_.extent (1));
  const std::size_t y1 = std::min (y + h, glyphs_.extent (0));

  const auto glyph_sub
      = submdspan (glyphs_, std::pair{ y0, y1 }, std::pair{ x0, x1 });
  const auto fg_sub
      = submdspan (fgs_, std::pair{ y0, y1 }, std::pair{ x0, x1 });
  const auto bg_sub
      = submdspan (bgs_, std::pair{ y0, y1 }, std::pair{ x0, x1 });

  return RasterView (glyph_sub, fg_sub, bg_sub, *glyph_table_);
}

// ============================================================================
// Raster implementation
// ============================================================================

Raster::Raster (const std::size_t width, const std::size_t height,
                GlyphTable &glyphs)
    : Raster (width * ch, height * ln, glyphs)
{
}

Raster::Raster (const width_t width, const height_t height, GlyphTable &glyphs)
    : width_ (width),
      height_ (height),
      glyphs_storage_ (cols_from (width) * rows_from (height), 32),
      fgs_storage_ (cols_from (width) * rows_from (height), DEFAULT_COLOR),
      bgs_storage_ (cols_from (width) * rows_from (height), DEFAULT_COLOR),
      glyph_table_ (&glyphs)
{
}

Raster::Raster (const Size size, GlyphTable &glyphs)
    : Raster (size.w, size.h, glyphs)
{
}

RasterView
Raster::view () noexcept
{
  const auto rows = rows_from (height_);
  const auto cols = cols_from (width_);

  glyph_view_t glyphs (glyphs_storage_.data (), mdspan_extents{ rows, cols });
  color_view_t fgs (fgs_storage_.data (), mdspan_extents{ rows, cols });
  color_view_t bgs (bgs_storage_.data (), mdspan_extents{ rows, cols });

  return RasterView (glyphs, fgs, bgs, *glyph_table_);
}

void
Raster::clear ()
{
  std::ranges::fill (glyphs_storage_, 32);
  std::ranges::fill (fgs_storage_, DEFAULT_COLOR);
  std::ranges::fill (bgs_storage_, DEFAULT_COLOR);
}

const_glyph_view_t
Raster::glyphs_2d () const noexcept
{
  const auto rows = rows_from (height_);
  const auto cols = cols_from (width_);
  return const_glyph_view_t (glyphs_storage_.data (),
                             mdspan_extents{ rows, cols });
}

const_color_view_t
Raster::fgs_2d () const noexcept
{
  const auto rows = rows_from (height_);
  const auto cols = cols_from (width_);
  return const_color_view_t (fgs_storage_.data (),
                             mdspan_extents{ rows, cols });
}

const_color_view_t
Raster::bgs_2d () const noexcept
{
  const auto rows = rows_from (height_);
  const auto cols = cols_from (width_);
  return const_color_view_t (bgs_storage_.data (),
                             mdspan_extents{ rows, cols });
}

} // namespace nxb
