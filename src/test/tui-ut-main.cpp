#include "../tty/raster-diff.hpp"
#include "../tty/tui.hpp"
#include "../tty/units.hpp"

#include <boost/ut.hpp>

namespace nxb::test
{

using namespace boost::ut;
namespace tui = nxb::tui;
using nxb::ChangeRun;
using nxb::GlyphTable;
using nxb::Raster;
using nxb::RasterView;
using nxb::Rgba8;

suite tui_tests = []
  {
    "progress_bar paints background and glyphs"_test = []
      {
        GlyphTable glyphs;
        auto raster_size = nxb::Size{ static_cast<std::size_t> (8) * nxb::ch,
                                      static_cast<std::size_t> (1) * nxb::ln };
        Raster raster (raster_size, glyphs);
        RasterView view = raster.view ();

        const Rgba8 fg (200, 200, 255);
        const Rgba8 bg (20, 30, 40);
        auto bar = tui::progress_bar (50.0 * percent, fg, bg);

        const auto layout_size
            = nxb::Size{ static_cast<std::size_t> (8) * nxb::ch,
                         static_cast<std::size_t> (1) * nxb::ln };
        bar.render (view, layout_size);

        bool has_bar_glyph = false;
        for (auto x = 0 * ch; x < view.width (); ++x)
          {
            auto cell = view.get_cell (Pos::at (x, 0 * ln));
            expect (cell.has_value ()) << "cell exists";
            expect (cell->bg == bg) << "bg at x=" << x.numerical_value_in (ch)
                                    << " (" << cell->bg << " == " << bg << ")";
            if (cell->glyph != ' ')
              has_bar_glyph = true;
          }

        expect (has_bar_glyph) << "progress glyphs written";
      };

    "column layout renders children at successive rows"_test = []
      {
        GlyphTable glyphs;
        auto raster_size = nxb::Size{ static_cast<std::size_t> (10) * nxb::ch,
                                      static_cast<std::size_t> (3) * nxb::ln };
        Raster raster (raster_size, glyphs);
        RasterView view = raster.view ();

        // Three text items stacked vertically
        auto layout = tui::column (tui::text ("AAA"), tui::text ("BBB"),
                                   tui::text ("CCC"));

        layout.render (view, raster_size);

        // Check row 0: should have "AAA"
        auto r0c0 = view.get_cell (Pos::at (0 * ch, 0 * ln));
        auto r0c1 = view.get_cell (Pos::at (1 * ch, 0 * ln));
        auto r0c2 = view.get_cell (Pos::at (2 * ch, 0 * ln));
        expect (r0c0 && r0c1 && r0c2) << "row 0 cells exist";
        expect (static_cast<char> (r0c0->glyph) == 'A') << "row 0 col 0 = A";
        expect (static_cast<char> (r0c1->glyph) == 'A') << "row 0 col 1 = A";
        expect (static_cast<char> (r0c2->glyph) == 'A') << "row 0 col 2 = A";

        // Check row 1: should have "BBB"
        auto r1c0 = view.get_cell (Pos::at (0 * ch, 1 * ln));
        auto r1c1 = view.get_cell (Pos::at (1 * ch, 1 * ln));
        auto r1c2 = view.get_cell (Pos::at (2 * ch, 1 * ln));
        expect (r1c0 && r1c1 && r1c2) << "row 1 cells exist";
        expect (static_cast<char> (r1c0->glyph) == 'B') << "row 1 col 0 = B";
        expect (static_cast<char> (r1c1->glyph) == 'B') << "row 1 col 1 = B";
        expect (static_cast<char> (r1c2->glyph) == 'B') << "row 1 col 2 = B";

        // Check row 2: should have "CCC"
        auto r2c0 = view.get_cell (Pos::at (0 * ch, 2 * ln));
        auto r2c1 = view.get_cell (Pos::at (1 * ch, 2 * ln));
        auto r2c2 = view.get_cell (Pos::at (2 * ch, 2 * ln));
        expect (r2c0 && r2c1 && r2c2) << "row 2 cells exist";
        expect (static_cast<char> (r2c0->glyph) == 'C') << "row 2 col 0 = C";
        expect (static_cast<char> (r2c1->glyph) == 'C') << "row 2 col 1 = C";
        expect (static_cast<char> (r2c2->glyph) == 'C') << "row 2 col 2 = C";
      };

    "row layout distributes fixed and flex children"_test = []
      {
        GlyphTable glyphs;
        auto raster_size = nxb::Size{ static_cast<std::size_t> (10) * nxb::ch,
                                      static_cast<std::size_t> (1) * nxb::ln };
        Raster raster (raster_size, glyphs);
        RasterView view = raster.view ();

        const Rgba8 fill_bg (5, 15, 25);
        auto layout = tui::row (tui::text ("abc"), tui::fill (fill_bg),
                                tui::text ("xy"));

        const auto layout_size
            = nxb::Size{ static_cast<std::size_t> (10) * nxb::ch,
                         static_cast<std::size_t> (1) * nxb::ln };
        layout.render (view, layout_size);

        // Text appears at expected positions
        auto c0 = view.get_cell (Pos::at (0 * ch, 0 * ln));
        auto c1 = view.get_cell (Pos::at (1 * ch, 0 * ln));
        auto c2 = view.get_cell (Pos::at (2 * ch, 0 * ln));
        auto c8 = view.get_cell (Pos::at (8 * ch, 0 * ln));
        auto c9 = view.get_cell (Pos::at (9 * ch, 0 * ln));

        expect (c0 && c1 && c2 && c8 && c9) << "cells exist";
        expect (static_cast<char> (c0->glyph) == 'a');
        expect (static_cast<char> (c1->glyph) == 'b');
        expect (static_cast<char> (c2->glyph) == 'c');
        expect (static_cast<char> (c8->glyph) == 'x');
        expect (static_cast<char> (c9->glyph) == 'y');

        // Flex fill gets remaining width (5 cols) and paints background
        for (auto x = 3 * ch; x < 8 * ch; ++x)
          {
            auto cell = view.get_cell (Pos::at (x, 0 * ln));
            expect (cell.has_value ());
            expect (cell->bg == fill_bg)
                << "fill bg at x=" << x.numerical_value_in (ch);
          }
      };
  };

suite diff_tests = []
  {
    "diff_rasters emits nothing for identical rasters"_test = []
      {
        GlyphTable glyphs;
        Raster front (4 * ch, 2 * ln, glyphs);
        Raster back (4 * ch, 2 * ln, glyphs);

        // Both start cleared to spaces - should be identical
        std::vector<ChangeRun> runs;
        diff_rasters (front, back,
                      [&] (const ChangeRun &r) { runs.push_back (r); });

        expect (runs.empty ()) << "no changes for identical rasters";
      };

    "diff_rasters emits one run for single changed cell"_test = []
      {
        GlyphTable glyphs;
        Raster front (4 * ch, 1 * ln, glyphs);
        Raster back (4 * ch, 1 * ln, glyphs);

        // Change one cell in the back buffer
        back.view ().set_char (Pos::at (1 * ch, 0 * ln), 'X');

        std::vector<ChangeRun> runs;
        diff_rasters (front, back,
                      [&] (const ChangeRun &r) { runs.push_back (r); });

        expect (runs.size () == 1_ul) << "one run for one changed cell";
        expect (runs[0].origin == Pos::at (1 * ch, 0 * ln))
            << "correct position";
        expect (runs[0].glyphs.size () == 1_ul) << "one glyph in run";
        expect (runs[0].glyphs[0] == 'X') << "correct glyph";
      };

    "diff_rasters batches consecutive changed cells with same colors"_test = []
      {
        GlyphTable glyphs;
        Raster front (8 * ch, 1 * ln, glyphs);
        Raster back (8 * ch, 1 * ln, glyphs);

        // Change cells 2,3,4 to "ABC"
        back.view ().write_text (Pos::at (2 * ch, 0 * ln), "ABC");

        std::vector<ChangeRun> runs;
        diff_rasters (front, back,
                      [&] (const ChangeRun &r) { runs.push_back (r); });

        expect (runs.size () == 1_ul)
            << "consecutive changes batched into one run";
        expect (runs[0].origin == Pos::at (2 * ch, 0 * ln))
            << "starts at first change";
        expect (runs[0].glyphs.size () == 3_ul) << "three glyphs in run";
      };

    "diff_rasters splits runs at color boundaries"_test = []
      {
        GlyphTable glyphs;
        Raster front (6 * ch, 1 * ln, glyphs);
        Raster back (6 * ch, 1 * ln, glyphs);

        const Rgba8 red (255, 0, 0);
        const Rgba8 blue (0, 0, 255);

        // Write "AB" in red, then "CD" in blue
        auto view = back.view ();
        view.write_text (Pos::at (1 * ch, 0 * ln), "AB");
        view.set_fg (Pos::at (1 * ch, 0 * ln), red);
        view.set_fg (Pos::at (2 * ch, 0 * ln), red);
        view.write_text (Pos::at (3 * ch, 0 * ln), "CD");
        view.set_fg (Pos::at (3 * ch, 0 * ln), blue);
        view.set_fg (Pos::at (4 * ch, 0 * ln), blue);

        std::vector<ChangeRun> runs;
        diff_rasters (front, back,
                      [&] (const ChangeRun &r) { runs.push_back (r); });

        expect (runs.size () == 2_ul) << "split at color boundary";
        expect (runs[0].glyphs.size () == 2_ul) << "first run has 2 glyphs";
        expect (runs[1].glyphs.size () == 2_ul) << "second run has 2 glyphs";
        expect (runs[0].fg_change == red) << "first run sets red";
        expect (runs[1].fg_change == blue) << "second run sets blue";
      };

    "diff_rasters tracks color state across runs"_test = []
      {
        GlyphTable glyphs;
        Raster front (6 * ch, 1 * ln, glyphs);
        Raster back (6 * ch, 1 * ln, glyphs);

        const Rgba8 red (255, 0, 0);

        // Two separate red regions with unchanged gap between
        auto view = back.view ();
        view.set_char (Pos::at (0 * ch, 0 * ln), 'A');
        view.set_fg (Pos::at (0 * ch, 0 * ln), red);
        view.set_char (Pos::at (3 * ch, 0 * ln), 'B');
        view.set_fg (Pos::at (3 * ch, 0 * ln), red);

        std::vector<ChangeRun> runs;
        diff_rasters (front, back,
                      [&] (const ChangeRun &r) { runs.push_back (r); });

        expect (runs.size () == 2_ul) << "two separate runs";
        expect (runs[0].fg_change == red) << "first run sets red";
        expect (!runs[1].fg_change.has_value ())
            << "second run omits redundant color";
      };

    "diff_rasters handles fg_reset for default color"_test = []
      {
        GlyphTable glyphs;
        Raster front (4 * ch, 1 * ln, glyphs);
        Raster back (4 * ch, 1 * ln, glyphs);

        const Rgba8 red (255, 0, 0);

        // First cell red, second cell default
        auto view = back.view ();
        view.set_char (Pos::at (0 * ch, 0 * ln), 'A');
        view.set_fg (Pos::at (0 * ch, 0 * ln), red);
        view.set_char (Pos::at (1 * ch, 0 * ln), 'B');
        // B keeps DEFAULT_COLOR

        std::vector<ChangeRun> runs;
        diff_rasters (front, back,
                      [&] (const ChangeRun &r) { runs.push_back (r); });

        expect (runs.size () == 2_ul) << "two runs due to color change";
        expect (runs[0].fg_change == red) << "first sets red";
        expect (runs[1].fg_reset == true) << "second resets to default";
      };

    "diff_rasters handles multiple rows"_test = []
      {
        GlyphTable glyphs;
        Raster front (4 * ch, 3 * ln, glyphs);
        Raster back (4 * ch, 3 * ln, glyphs);

        // Change one cell per row
        auto view = back.view ();
        view.set_char (Pos::at (0 * ch, 0 * ln), 'A');
        view.set_char (Pos::at (2 * ch, 1 * ln), 'B');
        view.set_char (Pos::at (3 * ch, 2 * ln), 'C');

        std::vector<ChangeRun> runs;
        diff_rasters (front, back,
                      [&] (const ChangeRun &r) { runs.push_back (r); });

        expect (runs.size () == 3_ul) << "one run per row";
        expect (runs[0].origin == Pos::at (0 * ch, 0 * ln));
        expect (runs[1].origin == Pos::at (2 * ch, 1 * ln));
        expect (runs[2].origin == Pos::at (3 * ch, 2 * ln));
      };
  };

} // namespace nxb::test

int
main ()
{
  return boost::ut::cfg<>.run ();
}
