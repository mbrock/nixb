#pragma once

#include "tty-raster.hpp"
#include "ui-dom.hpp"

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
  void fill_background (Raster &raster, const Rect &rect, char bg_glyph,
                        fmt::color fg, fmt::color bg);

  /// Draw text within rect
  void draw_text (Raster &raster, GlyphTable &glyphs, const Rect &rect,
                  const std::string &text, fmt::color color);
};

} // namespace nxb::ui
