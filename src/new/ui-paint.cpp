#include "ui-paint.hpp"

namespace nxb::ui
{

void
Painter::paint (const Dom &dom, Raster &raster, GlyphTable &glyphs)
{
  // Clear raster first
  raster.clear ();

  // Paint from root
  const auto &root_node = dom.get (dom.root ());
  paint_node (dom, root_node, raster, glyphs);
}

void
Painter::paint_node (const Dom &dom, const NodeData &node, Raster &raster,
                     GlyphTable &glyphs)
{
  // Handle based on node type
  if (auto *elem = std::get_if<Element> (&node.content))
    {
      // Fill background
      if (elem->style.bg_glyph != ' ')
        {
          fill_background (raster, node.rect, elem->style.bg_glyph,
                           elem->style.fg_color, elem->style.bg_color);
        }

      // Paint children
      for (auto child_id : elem->children)
        {
          const auto &child = dom.get (child_id);
          paint_node (dom, child, raster, glyphs);
        }
    }
  else if (auto *text = std::get_if<Text> (&node.content))
    {
      // Draw text
      draw_text (raster, glyphs, node.rect, text->content, text->color);
    }
}

void
Painter::fill_background (Raster &raster, const Rect &rect, char bg_glyph,
                          Rgba8 fg_color, Rgba8 bg_color)
{
  // Fill with glyph ID (just use ASCII for now)
  GlyphTable::GlyphId gid = static_cast<GlyphTable::GlyphId> (bg_glyph);

  // Get a subraster view of just the rectangle we want to fill
  auto sub = raster.subraster (rect.x, rect.y, rect.w, rect.h);

  // Fill the subraster - coordinates are now local (0,0) based!
  for (std::size_t row = 0; row < sub.height (); ++row)
    {
      for (std::size_t col = 0; col < sub.width (); ++col)
        {
          sub.set_glyph (col, row, gid);
          sub.set_fg (col, row, fg_color);
          if (bg_color != Rgba8::transparent ())
            sub.set_bg (col, row, bg_color);
        }
    }
}

void
Painter::draw_text (Raster &raster, GlyphTable &glyphs, const Rect &rect,
                    const std::string &text, Rgba8 color)
{
  std::size_t x = rect.x;
  std::size_t y = rect.y;

  for (char c : text)
    {
      if (c == '\n')
        {
          // Move to next line
          x = rect.x;
          ++y;
          if (y >= rect.y + rect.h)
            break; // Out of bounds
          continue;
        }

      // Write character
      if (x < rect.x + rect.w && x < raster.width () && y < raster.height ())
        {
          raster.set_char (x, y, c);
          raster.set_fg (x, y, color);
          ++x;
        }
      else
        {
          // Hit right edge, wrap to next line
          x = rect.x;
          ++y;
          if (y >= rect.y + rect.h)
            break;
        }
    }
}

} // namespace nxb::ui
