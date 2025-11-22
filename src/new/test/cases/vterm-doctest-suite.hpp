#pragma once
// doctest-based vterm tests - replaces old YAML+emacs system.
// Run: make vterm-test  or  ./build/src/new/nxb-vterm-tests

#include "../../ansi.hpp"
#include "../../ui-app.hpp"
#include "../../ui-dom.hpp"
#include "../../ui-layout.hpp"
#include "../../ui-paint.hpp"
#include "../../vterm-wrapper.hpp"
#include "../doctest.h"

#include <iostream>
#include <sstream>

namespace nxb::test
{

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
  out << "Raster " << raster.width () << "x" << raster.height () << ":\n";

  for (std::size_t y = 0; y < raster.height (); ++y)
    {
      out << "Row " << y << ": ";
      for (std::size_t x = 0; x < raster.width (); ++x)
        {
          auto cell = raster.get_cell (x, y);
          if (cell.has_value ())
            {
              const char ch = (cell->glyph >= 32 && cell->glyph < 127)
                            ? static_cast<char> (cell->glyph)
                            : '?';
              out << ch;
            }
          else
            {
              out << ' ';
            }
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

TEST_CASE ("basic-ansi: ANSI escape sequences render correctly")
{
  using namespace nxb::vterm;

  Terminal term (4, 8);

  // Run the test - simulate what basic_ansi test outputs
  auto output = capture_output ([] () {
    std::cout << "\x1b[2J"; // Clear screen
    std::cout << "\x1b[H";  // Move to home
    std::cout << "HELLO\r\n";
    std::cout << "WORLD!!";
    std::cout << "\x1b[4;8H"; // Move cursor to row 4, column 8
    std::cout.flush ();
  });

  term.write (output);

  // Verify display matches expected (trimming trailing spaces for comparison)
  auto trim_end = [] (std::string s) {
    s.erase (
        std::find_if (s.rbegin (), s.rend (),
                      [] (const unsigned char ch) { return !std::isspace (ch); })
            .base (),
        s.end ());
    return s;
  };

  CHECK (trim_end (term.get_row_text (0)) == "HELLO");
  CHECK (trim_end (term.get_row_text (1)) == "WORLD!!");
  CHECK (trim_end (term.get_row_text (2)) == "");
  CHECK (trim_end (term.get_row_text (3)) == "");

  // Cursor position would be at [4, 8] (1-based) = [3, 7] (0-based)
  // Note: vterm doesn't expose cursor position in our wrapper yet,
  // but we can add it if needed
}

TEST_CASE ("vterm: colors work correctly")
{
  using namespace nxb::vterm;

  Terminal term (5, 20);

  // Test RGB colors
  term.write ("\x1b[38;2;255;0;0m"); // Red FG
  term.write ("\x1b[48;2;0;255;0m"); // Green BG
  term.write ("RGB");
  term.write ("\x1b[0m");

  auto cell = term.get_cell (0, 0);
  REQUIRE (cell.has_value ());
  CHECK (cell->fg.is_rgb ());
  CHECK (cell->bg.is_rgb ());

  // Test indexed colors
  term.write (" ");
  term.write ("\x1b[33m"); // Yellow FG
  term.write ("IDX");

  auto idx_cell = term.get_cell (0, 4);
  REQUIRE (idx_cell.has_value ());
  CHECK (idx_cell->fg.is_indexed ());
}

TEST_CASE ("vterm: text attributes")
{
  using namespace nxb::vterm;

  Terminal term (5, 40);

  term.write ("\x1b[1mBold\x1b[0m ");
  term.write ("\x1b[3mItalic\x1b[0m ");
  term.write ("\x1b[4mUnderline\x1b[0m ");
  term.write ("\x1b[9mStrike\x1b[0m");

  auto bold_cell = term.get_cell (0, 0);
  REQUIRE (bold_cell.has_value ());
  CHECK (bold_cell->bold);
  CHECK_FALSE (bold_cell->italic);

  auto italic_cell = term.get_cell (0, 5);
  REQUIRE (italic_cell.has_value ());
  CHECK (italic_cell->italic);
  CHECK_FALSE (italic_cell->bold);

  auto underline_cell = term.get_cell (0, 12);
  REQUIRE (underline_cell.has_value ());
  CHECK (underline_cell->underline);

  auto strike_cell = term.get_cell (0, 22);
  REQUIRE (strike_cell.has_value ());
  CHECK (strike_cell->strike);
}

TEST_CASE ("vterm: wide characters (CJK)")
{
  using namespace nxb::vterm;

  Terminal term (5, 20);

  term.write ("Hello 世界!");

  // Use snapshot to check for wide characters
  auto snap = term.snapshot ();

  // Should have at least one wide character
  CHECK (snap.any_of ([] (const Cell &cell) { return cell.width == 2; }));

  // Count wide characters
  int wide_count
      = snap.count_if ([] (const Cell &cell) { return cell.width == 2; });
  CHECK (wide_count >= 2); // At least 2 wide chars in "世界"

  std::string text = term.get_row_text (0);
  CHECK (text.find ("Hello") != std::string::npos);
  CHECK (text.find ("世界") != std::string::npos);
}

TEST_CASE ("vterm: cursor movement")
{
  using namespace nxb::vterm;

  Terminal term (10, 40);

  // Test various cursor movements
  term.write ("\x1b[H"); // Home
  term.write ("A");
  term.write ("\x1b[5;10H"); // Move to row 5, col 10
  term.write ("B");
  term.write ("\x1b[1;1H"); // Back to top-left
  term.write ("C");         // Should overwrite A

  CHECK (term.get_row_text (0)[0] == 'C');
  CHECK (term.get_row_text (4)[9] == 'B');
}

TEST_CASE ("vterm: screen clearing")
{
  using namespace nxb::vterm;

  Terminal term (5, 20);

  // Fill with text
  term.write ("Line 1\nLine 2\nLine 3");

  // Clear entire screen
  term.write ("\x1b[2J");

  // All cells should be empty (space with no attributes)
  const auto snap = term.snapshot ();
  CHECK (snap.all_of ([] (const Cell &cell) {
    return cell.chars.empty ()
           || (cell.chars.size () == 1 && cell.chars[0] == U' ');
  }));
}

TEST_CASE ("vterm: get screen text")
{
  using namespace nxb::vterm;

  Terminal term (3, 10);

  term.write ("\x1b[H");
  term.write ("AAA");
  term.write ("\x1b[2;1H"); // Move to row 2, col 1
  term.write ("BBB");
  term.write ("\x1b[3;1H"); // Move to row 3, col 1
  term.write ("CCC");

  // Get individual rows
  CHECK (term.get_row_text (0).substr (0, 3) == "AAA");
  CHECK (term.get_row_text (1).substr (0, 3) == "BBB");
  CHECK (term.get_row_text (2).substr (0, 3) == "CCC");

  // Get entire screen
  std::string screen = term.get_screen_text ();
  CHECK (screen.find ("AAA") != std::string::npos);
  CHECK (screen.find ("BBB") != std::string::npos);
  CHECK (screen.find ("CCC") != std::string::npos);
}

TEST_CASE ("vterm: reset clears state")
{
  using namespace nxb::vterm;

  Terminal term (5, 20);

  term.write ("\x1b[1;31mRed Bold Text\x1b[0m");

  auto cell = term.get_cell (0, 0);
  REQUIRE (cell.has_value ());
  CHECK (cell->bold);

  // Hard reset
  term.reset (true);

  // All cells should be cleared
  auto snap = term.snapshot ();
  CHECK (snap.all_of ([] (const Cell &cell) {
    return cell.chars.empty ()
           || (cell.chars.size () == 1 && cell.chars[0] == U' ');
  }));
}

TEST_CASE ("compositor: basic rendering with DOM, layout, and paint")
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

  // Dump raster state for debugging
  INFO ("Back buffer state:\n", dump_raster (back_buffer, glyphs));

  // Verify the rendered output
  // Row 0 should have "Comp"
  auto cell00 = back_buffer.get_cell (0, 0);
  REQUIRE (cell00.has_value ());
  INFO ("Cell at (0,0): glyph=", cell00->glyph, " (char='",
        (char)cell00->glyph, "')");
  CHECK (cell00->glyph == 'C');

  // Row 4 should have "OK"
  auto cell04 = back_buffer.get_cell (0, 4);
  REQUIRE (cell04.has_value ());
  INFO ("Cell at (0,4): glyph=", cell04->glyph, " (char='",
        (char)cell04->glyph, "')");
  CHECK (cell04->glyph == 'O');

  // The box should be filled with '#' (checking the layout worked)
  // Use Raster's count_if helper with mdspan underneath
  int box_cells_filled = back_buffer.count_if (
      0, 1, 10, 4, [] (const GlyphTable::GlyphId gid) { return gid == '#'; });

  INFO ("Box cells filled with '#': ", box_cells_filled, " / 30");
  // Should have at least some '#' characters from the box
  CHECK (box_cells_filled >= 10);

  // Test present_frame with a stringstream
  std::ostringstream out;
  compositor.present_frame (out);
  std::string ansi_output = out.str ();

  INFO ("ANSI output length: ", ansi_output.size (), " bytes");
  INFO ("ANSI output (first 200 chars): '",
        ansi_output.substr (0, std::min<size_t> (200, ansi_output.size ())),
        "'");

  // Should have generated ANSI output
  CHECK (ansi_output.size () > 0);
  // Should contain at least some positioning or content
  CHECK (ansi_output.find ("Comp") != std::string::npos);
}

TEST_CASE ("ansi::Writer: generates correct ANSI codes")
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
  INFO ("Generated ANSI (", ansi_output.size (), " bytes): '", ansi_output,
        "'");

  // Write the generated ANSI to vterm
  term.write (ansi_output);

  INFO ("Terminal state after ANSI:\n", dump_terminal (term));

  // Verify text content
  auto row0_text = term.get_row_text (0);
  auto row1_text = term.get_row_text (1);

  INFO ("Row 0 text: '", row0_text, "'");
  INFO ("Row 1 text: '", row1_text, "'");

  CHECK (row0_text.find ("RED BOLD TEXT") != std::string::npos);
  CHECK (row1_text.find ("Green text") != std::string::npos);

  // Check attributes
  auto cell_bold = term.get_cell (0, 0);
  REQUIRE (cell_bold.has_value ());
  INFO ("Cell (0,0): bold=", cell_bold->bold,
        " fg.is_rgb=", cell_bold->fg.is_rgb (),
        " fg.is_indexed=", cell_bold->fg.is_indexed ());
  CHECK (cell_bold->bold);
  // Color could be either indexed or RGB depending on vterm's color handling
  CHECK ((cell_bold->fg.is_indexed () || cell_bold->fg.is_rgb ()));

  // Check that row 1 is not bold
  auto cell_normal = term.get_cell (1, 0);
  REQUIRE (cell_normal.has_value ());
  INFO ("Cell (1,0): bold=", cell_normal->bold);
  CHECK_FALSE (cell_normal->bold);
}

TEST_CASE ("ansi::Writer: screen comparison with expected output")
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

      INFO ("Row ", row, " comparison");
      CHECK (expected_text == actual_text);
    }
}

} // namespace nxb::test
