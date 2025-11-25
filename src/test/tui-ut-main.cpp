#include "../tty/tui.hpp"
#include "../tty/units.hpp"

#include <boost/ut.hpp>

namespace nxb::test
{

using namespace boost::ut;
namespace tui = nxb::tui;
using nxb::GlyphTable;
using nxb::Raster;
using nxb::Rgba8;

suite tui_tests = [] {
  "progress_bar paints background and glyphs"_test = [] {
    GlyphTable glyphs;
    auto raster_size = nxb::Size{ static_cast<std::size_t> (8) * nxb::ch,
                                  static_cast<std::size_t> (1) * nxb::ln };
    Raster raster (raster_size, glyphs);

    const Rgba8 fg (200, 200, 255);
    const Rgba8 bg (20, 30, 40);
    auto bar = tui::progress_bar (50.0 * percent, fg, bg);

    const auto layout_size
        = nxb::Size{ static_cast<std::size_t> (8) * nxb::ch,
                     static_cast<std::size_t> (1) * nxb::ln };
    bar.render (raster, layout_size);

    bool has_bar_glyph = false;
    for (std::size_t x = 0; x < raster.cols (); ++x)
      {
        auto cell = raster.get_cell (x, 0);
        expect (cell.has_value ()) << "cell exists";
        expect (cell->bg == bg)
            << "bg at x=" << x << " (" << cell->bg << " == " << bg << ")";
        if (cell->glyph != ' ')
          has_bar_glyph = true;
      }

    expect (has_bar_glyph) << "progress glyphs written";
  };

  "row layout distributes fixed and flex children"_test = [] {
    GlyphTable glyphs;
    auto raster_size = nxb::Size{ static_cast<std::size_t> (10) * nxb::ch,
                                  static_cast<std::size_t> (1) * nxb::ln };
    Raster raster (raster_size, glyphs);

    const Rgba8 fill_bg (5, 15, 25);
    auto layout
        = tui::row (tui::text ("abc"), tui::fill (fill_bg), tui::text ("xy"));

    const auto layout_size
        = nxb::Size{ static_cast<std::size_t> (10) * nxb::ch,
                     static_cast<std::size_t> (1) * nxb::ln };
    layout.render (raster, layout_size);

    // Text appears at expected positions
    auto c0 = raster.get_cell (0, 0);
    auto c1 = raster.get_cell (1, 0);
    auto c2 = raster.get_cell (2, 0);
    auto c8 = raster.get_cell (8, 0);
    auto c9 = raster.get_cell (9, 0);

    expect (c0 && c1 && c2 && c8 && c9) << "cells exist";
    expect (static_cast<char> (c0->glyph) == 'a');
    expect (static_cast<char> (c1->glyph) == 'b');
    expect (static_cast<char> (c2->glyph) == 'c');
    expect (static_cast<char> (c8->glyph) == 'x');
    expect (static_cast<char> (c9->glyph) == 'y');

    // Flex fill gets remaining width (5 cols) and paints background
    for (std::size_t x = 3; x < 8; ++x)
      {
        auto cell = raster.get_cell (x, 0);
        expect (cell.has_value ());
        expect (cell->bg == fill_bg) << "fill bg at x=" << x;
      }
  };
};

} // namespace nxb::test

int
main ()
{
  return boost::ut::cfg<>.run ();
}
