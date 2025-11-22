#pragma once

#include "dom.hpp"
#include "raster.hpp"

namespace nxb::ui
{

/// Paint DOM to raster
/// Traverses DOM tree and draws each node using its computed rect
class Painter
{
public:
  /// Paint entire DOM to raster
  void paint (const Dom &dom, Raster &raster, GlyphTable &glyphs);

private:
  /// Paint a single node and its children
  void paint_node (const Dom &dom, const NodeData &node, Raster &raster,
                   GlyphTable &glyphs);

  /// Fill rect with background glyph
  static void fill_background (Raster &raster, const Rect &rect, char bg_glyph,
                               Rgba8 fg_color, Rgba8 bg_color);

  /// Draw text within rect
  static void draw_text (Raster &raster, GlyphTable &glyphs, const Rect &rect,
                         const std::string &text, Rgba8 color);
};

} // namespace nxb::ui
