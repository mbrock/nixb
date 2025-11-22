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
                          fmt::color fg, fmt::color bg)
{
  // Fill with glyph ID (just use ASCII for now)
  GlyphTable::GlyphId gid = static_cast<GlyphTable::GlyphId> (bg_glyph);
  Rgba8 fg_rgba (fg);
  Rgba8 bg_rgba (bg);

  for (std::size_t y = rect.y; y < rect.y + rect.h && y < raster.height ();
       ++y)
    {
      for (std::size_t x = rect.x; x < rect.x + rect.w && x < raster.width ();
           ++x)
        {
          raster.set_glyph (x, y, gid);
          raster.set_fg (x, y, fg_rgba);
          if (bg != fmt::color::black)
            raster.set_bg (x, y, bg_rgba);
        }
    }
}

void
Painter::draw_text (Raster &raster, GlyphTable &glyphs, const Rect &rect,
                    const std::string &text, fmt::color color)
{
  std::size_t x = rect.x;
  std::size_t y = rect.y;
  Rgba8 fg_rgba (color);

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
          raster.set_fg (x, y, fg_rgba);
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
