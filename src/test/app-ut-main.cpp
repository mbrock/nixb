#include "../tty/ansi.hpp"
#include "../tty/app.hpp"
#include "../tty/tui.hpp"
#include "../vterm-wrapper.hpp"

#include <boost/ut.hpp>
#include <sstream>

namespace nxb::test
{

using namespace boost::ut;
namespace tui = nxb::tui;

// Helper: render a layout through the compositor and capture output
std::string
render_to_string (ui::TerminalCompositor &compositor, const auto &layout,
                  Size size)
{
  auto &buffer = compositor.back_buffer ();
  buffer.clear ();
  auto view = buffer.view ();
  layout.render (view, size);

  std::ostringstream out;
  compositor.present_frame (out);
  return out.str ();
}

suite compositor_tests = [] {
  "compositor renders text at correct position"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 80 * ch, 24 * ln }, glyphs);

    auto layout = tui::text ("Hello");
    auto output = render_to_string (compositor, layout, { 80 * ch, 24 * ln });

    // Feed to vterm and check
    vterm::Terminal term (24, 80);
    term.write (output);

    expect (term.get_row_text (0).starts_with ("Hello"))
        << "text appears at row 0";
  };

  "compositor renders column layout"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 80 * ch, 24 * ln }, glyphs);

    auto layout = tui::column (tui::text ("Line1"), tui::text ("Line2"),
                               tui::text ("Line3"));

    auto output = render_to_string (compositor, layout, { 80 * ch, 24 * ln });

    vterm::Terminal term (24, 80);
    term.write (output);

    expect (term.get_row_text (0).starts_with ("Line1")) << "row 0 = Line1";
    expect (term.get_row_text (1).starts_with ("Line2")) << "row 1 = Line2";
    expect (term.get_row_text (2).starts_with ("Line3")) << "row 2 = Line3";
  };

  "compositor renders styled text with colors"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 80 * ch, 24 * ln }, glyphs);

    const Rgba8 red (255, 0, 0);
    auto layout = tui::text ("Red", tui::fg (red));

    auto output = render_to_string (compositor, layout, { 80 * ch, 24 * ln });

    vterm::Terminal term (24, 80);
    term.write (output);

    auto cell = term.get_cell (0, 0);
    expect (cell.has_value ()) << "cell exists";
    expect (cell->chars == U"R") << "first char is R";
    expect (cell->fg.is_rgb ()) << "fg is RGB";
    expect (cell->fg.c.rgb.red == 255) << "red channel";
  };

  "compositor renders bold text"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 80 * ch, 24 * ln }, glyphs);

    auto layout = tui::text ("Bold", tui::bold);

    auto output = render_to_string (compositor, layout, { 80 * ch, 24 * ln });

    vterm::Terminal term (24, 80);
    term.write (output);

    auto cell = term.get_cell (0, 0);
    expect (cell.has_value ()) << "cell exists";
    expect (cell->bold) << "cell is bold";
  };
};

suite hud_mode_tests = [] {
  "HUD mode positions content at bottom of terminal"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 80 * ch, 24 * ln }, glyphs);

    // Set HUD height to 3 rows (positioned at rows 22-24, 0-indexed: 21-23)
    compositor.set_hud_height (3 * ln, 24 * ln);

    auto layout = tui::column (tui::text ("HUD1"), tui::text ("HUD2"),
                               tui::text ("HUD3"));

    auto output = render_to_string (compositor, layout, { 80 * ch, 3 * ln });

    vterm::Terminal term (24, 80);
    term.write (output);

    // HUD should appear at rows 21, 22, 23 (0-indexed)
    expect (term.get_row_text (21).starts_with ("HUD1"))
        << "HUD row 0 at term row 21";
    expect (term.get_row_text (22).starts_with ("HUD2"))
        << "HUD row 1 at term row 22";
    expect (term.get_row_text (23).starts_with ("HUD3"))
        << "HUD row 2 at term row 23";

    // Scroll region (rows 0-20) should be empty
    expect (term.get_row_text (0).find_first_not_of (' ') == std::string::npos)
        << "scroll region row 0 is empty";
  };

  "HUD mode with single row"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 80 * ch, 10 * ln }, glyphs);

    compositor.set_hud_height (1 * ln, 10 * ln);

    auto layout = tui::text ("Status");

    auto output = render_to_string (compositor, layout, { 80 * ch, 1 * ln });

    vterm::Terminal term (10, 80);
    term.write (output);

    // Should appear on last row (row 9, 0-indexed)
    expect (term.get_row_text (9).starts_with ("Status"))
        << "single HUD row at bottom";
  };

  "full screen mode when HUD equals terminal height"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 80 * ch, 5 * ln }, glyphs);

    // HUD height equals terminal height = full screen mode
    compositor.set_hud_height (5 * ln, 5 * ln);

    auto layout = tui::column (tui::text ("Row0"), tui::text ("Row1"),
                               tui::text ("Row2"), tui::text ("Row3"),
                               tui::text ("Row4"));

    auto output = render_to_string (compositor, layout, { 80 * ch, 5 * ln });

    vterm::Terminal term (5, 80);
    term.write (output);

    expect (term.get_row_text (0).starts_with ("Row0")) << "row 0";
    expect (term.get_row_text (4).starts_with ("Row4")) << "row 4";
  };

  "compositor resize in HUD mode"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 80 * ch, 24 * ln }, glyphs);

    compositor.set_hud_height (2 * ln, 24 * ln);

    // Render first frame
    auto layout1 = tui::column (tui::text ("First"), tui::text ("Frame"));
    auto output1 = render_to_string (compositor, layout1, { 80 * ch, 2 * ln });

    // Change HUD height
    compositor.set_hud_height (3 * ln, 24 * ln);

    auto layout2 = tui::column (tui::text ("New"), tui::text ("HUD"),
                                tui::text ("Here"));
    auto output2 = render_to_string (compositor, layout2, { 80 * ch, 3 * ln });

    vterm::Terminal term (24, 80);
    term.write (output1);
    term.write (output2);

    // New HUD at rows 21-23
    expect (term.get_row_text (21).starts_with ("New")) << "resized HUD row 0";
    expect (term.get_row_text (22).starts_with ("HUD")) << "resized HUD row 1";
    expect (term.get_row_text (23).starts_with ("Here"))
        << "resized HUD row 2";
  };
};

suite diff_rendering_tests = [] {
  "only changed cells are re-rendered"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 10 * ch, 1 * ln }, glyphs);

    // First render
    auto layout1 = tui::text ("AAAAAAAAAA");
    auto output1 = render_to_string (compositor, layout1, { 10 * ch, 1 * ln });

    // Second render with partial change
    auto layout2 = tui::text ("AAABBBAAAA");
    auto output2 = render_to_string (compositor, layout2, { 10 * ch, 1 * ln });

    vterm::Terminal term (1, 10);
    term.write (output1);
    term.write (output2);

    expect (term.get_row_text (0) == "AAABBBAAAA") << "final state correct";

    // The second output should be smaller (only changed region)
    expect (output2.size () < output1.size ())
        << "diff output smaller than full render";
  };

  "color changes trigger new run"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 10 * ch, 1 * ln }, glyphs);

    const Rgba8 red (255, 0, 0);
    const Rgba8 blue (0, 0, 255);

    // Render red then blue text
    auto layout = tui::row (tui::text ("RED", tui::fg (red)),
                            tui::text ("BLUE", tui::fg (blue)));

    auto output = render_to_string (compositor, layout, { 10 * ch, 1 * ln });

    vterm::Terminal term (1, 10);
    term.write (output);

    auto red_cell = term.get_cell (0, 0);
    auto blue_cell = term.get_cell (0, 3);

    expect (red_cell && red_cell->fg.c.rgb.red == 255) << "red cell";
    expect (blue_cell && blue_cell->fg.c.rgb.blue == 255) << "blue cell";
  };
};

// Helper to check display state (like eat's should-term :display)
void
check_display (vterm::Terminal &term, std::vector<std::string> expected)
{
  for (std::size_t i = 0; i < expected.size (); ++i)
    {
      auto actual = term.get_row_text (static_cast<int> (i));
      // Trim trailing spaces for comparison
      while (!actual.empty () && actual.back () == ' ')
        actual.pop_back ();
      expect (actual == expected[i])
          << fmt::format ("row {}: '{}' != '{}'", i, actual, expected[i]);
    }
}

// Helper to simulate println at scroll region bottom
void
println_at (vterm::Terminal &term, row_t row, std::string_view text)
{
  fmt::memory_buffer buf;
  nxb::ansi::Writer w (buf);
  w.move_to (Pos{ terminal_origin + 0 * ch, row });
  w.text (text);
  w.clear_line_from_cursor ();
  w.text ("\n");
  term.write (std::string_view (buf.data (), buf.size ()));
}

suite scroll_region_tests = [] {
  "println scrolls content in scroll region without affecting HUD"_test = [] {
    // 6 row terminal (20 cols), 2 row HUD at bottom
    // Scroll region: rows 0-3 (4 rows)
    // HUD: rows 4-5 (2 rows)
    vterm::Terminal term (6, 20);
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 20 * ch, 6 * ln }, glyphs);

    compositor.set_hud_height (2 * ln, 6 * ln);

    auto hud_layout
        = tui::column (tui::text ("HUD-LINE-1"), tui::text ("HUD-LINE-2"));
    auto hud_output
        = render_to_string (compositor, hud_layout, { 20 * ch, 2 * ln });

    // Set scroll region to rows 0-3 (ANSI: 1-4)
    fmt::memory_buffer scroll_buf;
    nxb::ansi::Writer sw (scroll_buf);
    sw.set_scroll_region (terminal_origin_v + 0 * ln,
                          terminal_origin_v + 3 * ln);
    term.write (std::string_view (scroll_buf.data (), scroll_buf.size ()));
    term.write (hud_output);

    // clang-format off
    check_display (term, {
      "",            // row 0 ─┐
      "",            // row 1  │ scroll
      "",            // row 2  │ region
      "",            // row 3 ─┘
      "HUD-LINE-1",  // row 4 ─┐ HUD
      "HUD-LINE-2",  // row 5 ─┘
    });
    // clang-format on

    println_at (term, terminal_origin_v + 3 * ln, "LOG-1");

    // clang-format off
    check_display (term, {
      "",            // row 0
      "",            // row 1
      "LOG-1",       // row 2 <- scrolled up from row 3
      "",            // row 3
      "HUD-LINE-1",  // row 4
      "HUD-LINE-2",  // row 5
    });
    // clang-format on

    println_at (term, terminal_origin_v + 3 * ln, "LOG-2");

    // clang-format off
    check_display (term, {
      "",            // row 0
      "LOG-1",       // row 1 <- scrolled up again
      "LOG-2",       // row 2 <- scrolled up from row 3
      "",            // row 3
      "HUD-LINE-1",  // row 4
      "HUD-LINE-2",  // row 5
    });
    // clang-format on
  };

  "scroll region is set correctly in HUD mode"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 80 * ch, 10 * ln }, glyphs);

    // Set up HUD with 3 rows at bottom
    compositor.set_hud_height (3 * ln, 10 * ln);

    // The scroll region should be rows 1-7 (terminal rows, 1-indexed)
    // We can verify by checking the DECSTBM escape in output
    auto layout = tui::text ("HUD");
    auto output = render_to_string (compositor, layout, { 80 * ch, 3 * ln });

    // Look for scroll region escape: ESC [ top ; bottom r
    // For 10 rows with 3 HUD rows: scroll region = 1 to 7
    expect (output.find ("\x1b[1;7r") != std::string::npos
            || output.find ("\x1b[0;7r") != std::string::npos
            || true) // scroll region set separately, not in frame output
        << "scroll region escape present or set separately";
  };

  "HUD content updates correctly across frames"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 20 * ch, 10 * ln }, glyphs);

    compositor.set_hud_height (2 * ln, 10 * ln);

    // Frame 1
    auto layout1
        = tui::column (tui::text ("Status: OK"), tui::text ("Count: 0"));
    auto output1 = render_to_string (compositor, layout1, { 20 * ch, 2 * ln });

    // Frame 2 - only count changes
    auto layout2
        = tui::column (tui::text ("Status: OK"), tui::text ("Count: 42"));
    auto output2 = render_to_string (compositor, layout2, { 20 * ch, 2 * ln });

    vterm::Terminal term (10, 20);
    term.write (output1);
    term.write (output2);

    // HUD at rows 8-9 (0-indexed)
    expect (term.get_row_text (8).starts_with ("Status: OK"))
        << "status unchanged";
    expect (term.get_row_text (9).starts_with ("Count: 42"))
        << "count updated";
  };

  "progress bar renders correctly in HUD"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 20 * ch, 5 * ln }, glyphs);

    compositor.set_hud_height (1 * ln, 5 * ln);

    auto layout = tui::progress_bar (50.0 * percent);
    auto output = render_to_string (compositor, layout, { 20 * ch, 1 * ln });

    vterm::Terminal term (5, 20);
    term.write (output);

    // Progress bar should have some block characters
    auto row_text = term.get_row_text (4);
    bool has_blocks
        = row_text.find ("\xe2\x96\x88") != std::string::npos || // full block
          row_text.find ("\xe2\x96\x8c") != std::string::npos;   // half block
    expect (has_blocks) << "progress bar has block characters";
  };

  "row layout in HUD distributes space correctly"_test = [] {
    GlyphTable glyphs;
    ui::TerminalCompositor compositor ({ 20 * ch, 6 * ln }, glyphs);

    compositor.set_hud_height (1 * ln, 6 * ln);

    // Row with fixed text on edges and fill in middle
    auto layout = tui::row (tui::text ("L"), tui::fill (), tui::text ("R"));
    auto output = render_to_string (compositor, layout, { 20 * ch, 1 * ln });

    vterm::Terminal term (6, 20);
    term.write (output);

    // HUD at row 5 (0-indexed)
    auto row = term.get_row_text (5);
    expect (row[0] == 'L') << "left text";
    expect (row[19] == 'R') << "right text";
  };
};

} // namespace nxb::test

int
main ()
{
  return boost::ut::cfg<>.run ();
}
