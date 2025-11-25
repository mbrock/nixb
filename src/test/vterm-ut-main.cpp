#include "../tty/ansi.hpp"
#include "../tty/app.hpp"
#include "../tty/dom.hpp"
#include "../tty/layout.hpp"
#include "../tty/paint.hpp"
#include "../vterm-wrapper.hpp"
#include <boost/ut.hpp>

#include <iostream>
#include <sstream>

namespace nxb::test
{

using namespace boost::ut;

// Helper to capture stdout/stderr from a function
template <typename Func>
std::string
capture_output (Func &&func)
{
  const std::ostringstream capture;
  std::streambuf *old_cout = std::cout.rdbuf (capture.rdbuf ());
  std::streambuf *old_cerr = std::cerr.rdbuf (capture.rdbuf ());

  func ();

  std::cout.rdbuf (old_cout);
  std::cerr.rdbuf (old_cerr);

  return capture.str ();
}

// Helper to dump raster state for debugging
inline std::string
dump_raster (const nxb::Raster &raster, const nxb::GlyphTable &glyphs)
{
  std::ostringstream out;
  out << "Raster " << raster.cols () << "x" << raster.rows () << ":\n";

  for (std::size_t y = 0; y < raster.rows (); ++y)
    {
      out << "Row " << y << ": ";
      for (std::size_t x = 0; x < raster.cols (); ++x)
        {
          auto cell = raster.get_cell (x, y);
          if (cell.has_value ())
            out << glyphs.get (cell->glyph).value ();
          else
            out << ' ';
        }
      out << "\n";
    }
  return out.str ();
}

// Helper to dump terminal state
inline std::string
dump_terminal (const nxb::vterm::Terminal &term)
{
  std::ostringstream out;
  auto [rows, cols] = term.get_size ();
  out << "Terminal " << cols << "x" << rows << ":\n";

  for (int r = 0; r < rows; ++r)
    {
      out << "Row " << r << ": '" << term.get_row_text (r) << "'\n";
    }
  return out.str ();
}

} // namespace nxb::test

// boost-ut test suite (must be outside namespace)
boost::ut::suite vterm_tests = []
  {
    using namespace nxb::vterm;
    using namespace nxb;
    using namespace nxb::test;
    using namespace boost::ut;

    "basic-ansi: ANSI escape sequences render correctly"_test = []
      {
        Terminal term (4, 8);

        // Run the test - simulate what basic_ansi test outputs
        auto output = capture_output (
            [] ()
              {
                std::cout << "\x1b[2J"; // Clear screen
                std::cout << "\x1b[H";  // Move to home
                std::cout << "HELLO\r\n";
                std::cout << "WORLD!!";
                std::cout << "\x1b[4;8H"; // Move cursor to row 4, column 8
                std::cout.flush ();
              });

        term.write (output);

        // Verify display matches expected (trimming trailing spaces for
        // comparison)
        auto trim_end = [] (std::string s)
          {
            s.erase (std::find_if (s.rbegin (), s.rend (),
                                   [] (const unsigned char ch)
                                     { return !std::isspace (ch); })
                         .base (),
                     s.end ());
            return s;
          };

        expect (trim_end (term.get_row_text (0)) == std::string ("HELLO"));
        expect (trim_end (term.get_row_text (1)) == std::string ("WORLD!!"));
        expect (trim_end (term.get_row_text (2)) == std::string (""));
        expect (trim_end (term.get_row_text (3)) == std::string (""));
      };

    "vterm: colors work correctly"_test = []
      {
        Terminal term (5, 20);

        // Test RGB colors
        term.write ("\x1b[38;2;255;0;0m"); // Red FG
        term.write ("\x1b[48;2;0;255;0m"); // Green BG
        term.write ("RGB");
        term.write ("\x1b[0m");

        auto cell = term.get_cell (0, 0);
        expect (cell.has_value ());
        expect (cell->fg.is_rgb ());
        expect (cell->bg.is_rgb ());

        // Test indexed colors
        term.write (" ");
        term.write ("\x1b[33m"); // Yellow FG
        term.write ("IDX");

        auto idx_cell = term.get_cell (0, 4);
        expect (idx_cell.has_value ());
        expect (idx_cell->fg.is_indexed ());
      };

    "vterm: text attributes"_test = []
      {
        Terminal term (5, 40);

        term.write ("\x1b[1mBold\x1b[0m ");
        term.write ("\x1b[3mItalic\x1b[0m ");
        term.write ("\x1b[4mUnderline\x1b[0m ");
        term.write ("\x1b[9mStrike\x1b[0m");

        auto bold_cell = term.get_cell (0, 0);
        expect (bold_cell.has_value ());
        expect (bold_cell->bold);
        expect (!bold_cell->italic);

        auto italic_cell = term.get_cell (0, 5);
        expect (italic_cell.has_value ());
        expect (italic_cell->italic);
        expect (!italic_cell->bold);

        auto underline_cell = term.get_cell (0, 12);
        expect (underline_cell.has_value ());
        expect (underline_cell->underline);

        auto strike_cell = term.get_cell (0, 22);
        expect (strike_cell.has_value ());
        expect (strike_cell->strike);
      };

    "vterm: wide characters (CJK)"_test = []
      {
        Terminal term (5, 20);

        term.write ("Hello 世界!");

        // Use snapshot to check for wide characters
        auto snap = term.snapshot ();

        // Should have at least one wide character
        expect (
            snap.any_of ([] (const Cell &cell) { return cell.width == 2; }));

        // Count wide characters
        int wide_count = snap.count_if ([] (const Cell &cell)
                                          { return cell.width == 2; });
        expect (ge (wide_count, 2)); // At least 2 wide chars in "世界"

        std::string text = term.get_row_text (0);
        expect (neq (text.find ("Hello"), std::string::npos));
        expect (neq (text.find ("世界"), std::string::npos));
      };

    "vterm: cursor movement"_test = []
      {
        Terminal term (10, 40);

        // Test various cursor movements
        term.write ("\x1b[H"); // Home
        term.write ("A");
        term.write ("\x1b[5;10H"); // Move to row 5, col 10
        term.write ("B");
        term.write ("\x1b[1;1H"); // Back to top-left
        term.write ("C");         // Should overwrite A

        expect (term.get_row_text (0)[0] == 'C');
        expect (term.get_row_text (4)[9] == 'B');
      };

    "vterm: screen clearing"_test = []
      {
        Terminal term (5, 20);

        // Fill with text
        term.write ("Line 1\nLine 2\nLine 3");

        // Clear entire screen
        term.write ("\x1b[2J");

        // All cells should be empty (space with no attributes)
        const auto snap = term.snapshot ();
        expect (snap.all_of (
            [] (const Cell &cell)
              {
                return cell.chars.empty ()
                       || (cell.chars.size () == 1 && cell.chars[0] == U' ');
              }));
      };

    "vterm: get screen text"_test = []
      {
        Terminal term (3, 10);

        term.write ("\x1b[H");
        term.write ("AAA");
        term.write ("\x1b[2;1H"); // Move to row 2, col 1
        term.write ("BBB");
        term.write ("\x1b[3;1H"); // Move to row 3, col 1
        term.write ("CCC");

        // Get individual rows
        expect (term.get_row_text (0).substr (0, 3) == std::string ("AAA"));
        expect (term.get_row_text (1).substr (0, 3) == std::string ("BBB"));
        expect (term.get_row_text (2).substr (0, 3) == std::string ("CCC"));

        // Get entire screen
        std::string screen = term.get_screen_text ();
        expect (neq (screen.find ("AAA"), std::string::npos));
        expect (neq (screen.find ("BBB"), std::string::npos));
        expect (neq (screen.find ("CCC"), std::string::npos));
      };

    "vterm: reset clears state"_test = []
      {
        Terminal term (5, 20);

        term.write ("\x1b[1;31mRed Bold Text\x1b[0m");

        auto cell = term.get_cell (0, 0);
        expect (cell.has_value ());
        expect (cell->bold);

        // Hard reset
        term.reset (true);

        // All cells should be cleared
        auto snap = term.snapshot ();
        expect (snap.all_of (
            [] (const Cell &cell)
              {
                return cell.chars.empty ()
                       || (cell.chars.size () == 1 && cell.chars[0] == U' ');
              }));
      };

    "compositor: basic rendering with DOM, layout, and paint"_test = []
      {
        using namespace nxb;

        GlyphTable glyphs;
        ui::TerminalCompositor compositor (20, 6, glyphs);
        ui::LayoutEngine layout;
        ui::Painter painter;
        ui::Dom dom;

        // Build a simple DOM tree with a title and a box
        ui::Style container_style = ui::Style::defaults ();
        container_style.flex_dir = ui::FlexDir::Column;
        container_style.width = ui::Size::fixed (20);
        container_style.height = ui::Size::fixed (6);
        container_style.justify = ui::Justify::Start;
        container_style.align = ui::Align::Start;
        auto container = dom.create_element (container_style);
        dom.append_child (dom.root (), container);

        // Box with # characters (3 rows, 10 cols)
        ui::Style box_style = ui::Style::defaults ();
        box_style.width = ui::Size::fixed (10);
        box_style.height = ui::Size::fixed (3);
        box_style.bg_glyph = '#';
        box_style.bg_color = fmt::color::blue;
        auto box = dom.create_element (box_style);
        dom.append_child (container, box);

        // Layout and paint
        layout.compute (dom, 20, 6);
        auto &back_buffer = compositor.back_buffer ();
        back_buffer.clear ();
        painter.paint (dom, back_buffer, compositor.glyphs ());

        // Write text directly to buffer
        back_buffer.write_text (0, 0, "Comp", glyphs);
        back_buffer.write_text (0, 4, "OK", glyphs);

        // Verify the rendered output
        // Row 0 should have "Comp"
        auto cell00 = back_buffer.get_cell (0, 0);
        expect (cell00.has_value ()) << "Cell (0,0) should have value";
        expect (static_cast<char> (cell00->glyph) == 'C')
            << "Cell (0,0) should be 'C'";

        // Row 4 should have "OK"
        auto cell04 = back_buffer.get_cell (0, 4);
        expect (cell04.has_value ()) << "Cell (0,4) should have value";
        expect (static_cast<char> (cell04->glyph) == 'O')
            << "Cell (0,4) should be 'O'";

        // The box should be filled with '#' (checking the layout worked)
        int box_cells_filled = back_buffer.count_if (
            Pos::at (0 * ch, 1 * ln), Size{ 10 * ch, 3 * ln },
            [] (const GlyphTable::GlyphId gid) { return gid == '#'; });

        // Should have at least some '#' characters from the box
        expect (ge (box_cells_filled, 10))
            << "Box should have at least 10 '#' cells";

        // Test present_frame with a stringstream
        std::ostringstream out;
        compositor.present_frame (out);
        std::string ansi_output = out.str ();

        // Should have generated ANSI output
        expect (gt (ansi_output.size (), 0u)) << "Should generate ANSI output";
        // Should contain at least some positioning or content
        expect (neq (ansi_output.find ("Comp"), std::string::npos))
            << "ANSI output should contain 'Comp'";
      };

    "ansi::Writer: generates correct ANSI codes"_test = []
      {
        using namespace nxb;
        using namespace nxb::vterm;

        Terminal term (24, 80);

        // Use ansi::Writer to generate ANSI sequences
        fmt::memory_buffer buf;
        ansi::Writer writer (buf);

        writer
            .move_to (1, 1) // Move to top-left (1-based)
            .fg (fmt::color::red)
            .bold ()
            .text ("RED BOLD TEXT")
            .reset ()
            .move_to (2, 1)
            .fg (0, 255, 0) // RGB green
            .text ("Green text")
            .reset ();

        std::string ansi_output (buf.data (), buf.size ());

        // Write the generated ANSI to vterm
        term.write (ansi_output);

        // Verify text content
        auto row0_text = term.get_row_text (0);
        auto row1_text = term.get_row_text (1);

        expect (neq (row0_text.find ("RED BOLD TEXT"), std::string::npos));
        expect (neq (row1_text.find ("Green text"), std::string::npos));

        // Check attributes
        auto cell_bold = term.get_cell (0, 0);
        expect (cell_bold.has_value ());
        expect (cell_bold->bold);
        // Color could be either indexed or RGB depending on vterm's color
        // handling
        expect (cell_bold->fg.is_indexed () || cell_bold->fg.is_rgb ());

        // Check that row 1 is not bold
        auto cell_normal = term.get_cell (1, 0);
        expect (cell_normal.has_value ());
        expect (!cell_normal->bold);
      };

    "ansi::Writer: screen comparison with expected output"_test = []
      {
        using namespace nxb;
        using namespace nxb::vterm;

        Terminal expected (5, 30);
        Terminal actual (5, 30);

        // Generate expected output manually
        expected.write ("\x1b[2J\x1b[H"); // Clear and home
        expected.write ("\x1b[1;1HLine 1");
        expected.write ("\x1b[2;1HLine 2");
        expected.write ("\x1b[3;1HLine 3");

        // Generate actual output using ansi::Writer
        fmt::memory_buffer buf;
        ansi::Writer writer (buf);

        writer.clear_screen ()
            .move_to (1, 1)
            .text ("Line 1")
            .move_to (2, 1)
            .text ("Line 2")
            .move_to (3, 1)
            .text ("Line 3");

        actual.write (std::string_view (buf.data (), buf.size ()));

        // Compare line by line
        for (int row = 0; row < 5; row++)
          {
            std::string expected_text = expected.get_row_text (row);
            std::string actual_text = actual.get_row_text (row);

            expect (expected_text == actual_text)
                << "Row " << row << " should match";
          }
      };
  };

int
main ()
{
}
