#include "raster.hpp"

#include <algorithm>
#include <cstring>
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

// rgba8 output formatting
std::ostream &
operator<< (std::ostream &os, const Rgba8 &c)
{
  fmt::print (os, "rgba8({:02x},{:02x},{:02x},{:02x})", c.r (), c.g (), c.b (),
              c.a ());
  return os;
}

// Owning constructor
Raster::Raster (const std::size_t width, const std::size_t height,
                GlyphTable &glyphs)
    : Raster (width * ch, height * ln, glyphs)
{
}

Raster::Raster (const width_t width, const height_t height, GlyphTable &glyphs)
    : glyph_view_ (nullptr,
                   mdspan_extents{ rows_from (height), cols_from (width) }),
      fg_view_ (nullptr,
                mdspan_extents{ rows_from (height), cols_from (width) }),
      bg_view_ (nullptr,
                mdspan_extents{ rows_from (height), cols_from (width) }),
      storage_glyphs_ (std::vector<GlyphTable::GlyphId> (
          cols_from (width) * rows_from (height), 32)),
      storage_fgs_ (
          std::vector (cols_from (width) * rows_from (height), DEFAULT_COLOR)),
      storage_bgs_ (
          std::vector (cols_from (width) * rows_from (height), DEFAULT_COLOR)),
      glyphs_ (&glyphs)
{
  // Update views to point to the allocated storage
  const auto rows = rows_from (height);
  const auto cols = cols_from (width);

  glyph_view_
      = glyph_view_t (storage_glyphs_->data (), mdspan_extents{ rows, cols });
  fg_view_
      = color_view_t (storage_fgs_->data (), mdspan_extents{ rows, cols });
  bg_view_
      = color_view_t (storage_bgs_->data (), mdspan_extents{ rows, cols });
}

Raster::Raster (const Size size, GlyphTable &glyphs)
    : Raster (size.w, size.h, glyphs)
{
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
      const auto h = other.rows ();
      const auto w = other.cols ();
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
  if (x >= cols () || y >= rows ())
    return;

  glyph_view_[y, x] = gid;
}

void
Raster::set_fg (const std::size_t x, const std::size_t y,
                const Rgba8 color) const noexcept
{
  if (x >= cols () || y >= rows ())
    return;

  fg_view_[y, x] = color;
}

void
Raster::set_bg (const std::size_t x, const std::size_t y,
                const Rgba8 color) const noexcept
{
  if (x >= cols () || y >= rows ())
    return;

  bg_view_[y, x] = color;
}

void
Raster::set_glyph (const Pos pos, const GlyphTable::GlyphId gid) const noexcept
{
  set_glyph (pos.col (), pos.row (), gid);
}

void
Raster::set_fg (const Pos pos, const Rgba8 color) const noexcept
{
  set_fg (pos.col (), pos.row (), color);
}

void
Raster::set_bg (const Pos pos, const Rgba8 color) const noexcept
{
  set_bg (pos.col (), pos.row (), color);
}

std::size_t
Raster::write_text (const std::size_t x, const std::size_t y,
                    const std::string_view text, GlyphTable &glyphs) noexcept
{
  if (y >= rows ())
    return x;

  const auto glyph_view = glyphs_2d ();

  std::size_t col = x;
  std::size_t i = 0;

  while (i < text.size () && col < cols ())
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

      i += char_len;
      ++col;
    }

  return col;
}

std::size_t
Raster::write_text (const std::size_t x, const std::size_t y,
                    const std::string_view text) noexcept
{
  if (!glyphs_)
    return x; // No glyph table, can't render

  return write_text (x, y, text, *glyphs_);
}

col_t
Raster::write_text (const Pos pos, const std::string_view text,
                    GlyphTable &glyphs) noexcept
{
  auto next_col = write_text (pos.col (), pos.row (), text, glyphs);
  return terminal_origin + next_col * ch;
}

col_t
Raster::write_text (const Pos pos, const std::string_view text) noexcept
{
  if (!glyphs_)
    return pos.x;

  return write_text (pos, text, *glyphs_);
}

void
Raster::fill_rect (const std::size_t x, const std::size_t y,
                   const std::size_t w, const std::size_t h,
                   const GlyphTable::GlyphId gid, const Rgba8 fg_color)
{
  if (w == 0 || h == 0)
    return;

  // Clamp to raster bounds
  const std::size_t x0 = std::min (x, cols ());
  const std::size_t y0 = std::min (y, rows ());
  const std::size_t x1 = std::min (x + w, cols ());
  const std::size_t y1 = std::min (y + h, rows ());

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
Raster::fill_rect (const Pos origin, const Size size,
                   const GlyphTable::GlyphId gid, const Rgba8 fg_color)
{
  fill_rect (origin.col (), origin.row (), size.w.numerical_value_in (ch),
             size.h.numerical_value_in (ln), gid, fg_color);
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
  if (x >= cols () || y >= rows ())
    return std::nullopt;

  const auto glyph_view = glyphs_2d ();
  const auto fg_view = fgs_2d ();
  const auto bg_view = bgs_2d ();

  return Cell{ glyph_view[y, x], fg_view[y, x], bg_view[y, x] };
}

std::optional<Raster::Cell>
Raster::get_cell (const Pos pos) const noexcept
{
  return get_cell (pos.col (), pos.row ());
}

Raster
Raster::subraster (const std::size_t x, const std::size_t y,
                   const std::size_t w, const std::size_t h) const noexcept
{
  using std::submdspan;

  // Clamp to raster bounds
  const std::size_t x0 = std::min (x, cols ());
  const std::size_t y0 = std::min (y, rows ());
  const std::size_t x1 = std::min (x + w, cols ());
  const std::size_t y1 = std::min (y + h, rows ());

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

Raster
Raster::subraster (const Pos origin, const Size size) const noexcept
{
  return subraster (origin.col (), origin.row (),
                    size.w.numerical_value_in (ch),
                    size.h.numerical_value_in (ln));
}

Raster
Raster::subraster (const Size size) const noexcept
{
  return subraster (Pos::origin (), size);
}

} // namespace nxb
