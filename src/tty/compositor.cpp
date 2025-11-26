#include "compositor.hpp"
#include "ansi.hpp"
#include "raster-diff.hpp"

#include <iostream>

namespace nxb::ui
{

TerminalCompositor::TerminalCompositor (const nxb::Size size,
                                        GlyphTable &glyphs)
    : front_ (size.w, size.h, glyphs), back_ (size.w, size.h, glyphs),
      glyphs_ (glyphs), hud_height_ (size.h),
      hud_start_row_ (terminal_origin_v + 0 * ln)
{
}

void
TerminalCompositor::resize (nxb::Size size)
{
  // In HUD mode, only resize to HUD height
  auto raster_h = hud_height_ > 0 * ln ? hud_height_ : size.h;
  front_ = Raster (size.w, raster_h, glyphs_);
  back_ = Raster (size.w, raster_h, glyphs_);

  // Clear only the HUD region, preserving cursor position
  fmt::memory_buffer buf;
  ansi::Writer w (buf);
  w.save_cursor ();

  if (hud_height_ > 0 * ln && hud_height_ < size.h)
    {
      // Clear HUD area
      auto end_row = terminal_origin_v + size.h;
      for (auto row = hud_start_row_; row < end_row; row += 1 * ln)
        {
          w.move_to (Pos{ terminal_origin + 0 * ch, row });
          w.clear_line ();
        }
    }
  else
    {
      w.clear_screen ();
    }

  w.restore_cursor ();
  std::cout.write (buf.data (), static_cast<std::streamsize> (buf.size ()));
  std::cout.flush ();
}

void
TerminalCompositor::set_hud_height (height_t hud_height, height_t term_height)
{
  if (hud_height == hud_height_)
    return;

  hud_height_ = hud_height;

  // Calculate where the HUD starts
  // Note: DECSTBM (set scroll region) moves cursor to home, so save/restore
  {
    fmt::memory_buffer buf;
    ansi::Writer wr (buf);
    wr.save_cursor ();

    if (hud_height >= term_height)
      {
        // Full-screen mode
        hud_start_row_ = terminal_origin_v + 0 * ln;
        wr.reset_scroll_region ();
      }
    else
      {
        // HUD mode: scroll region above, HUD at bottom
        // e.g., 24-row term, 2-row HUD: HUD starts at row 22, scroll region is
        // rows 0-21
        hud_start_row_ = terminal_origin_v + (term_height - hud_height);
        auto scroll_top = terminal_origin_v + 0 * ln;
        auto scroll_bottom = hud_start_row_ - 1 * ln;
        wr.set_scroll_region (scroll_top, scroll_bottom);
      }

    wr.restore_cursor ();
    std::cout.write (buf.data (), static_cast<std::streamsize> (buf.size ()));
    std::cout.flush ();
  }

  // Resize rasters to match HUD height
  auto raster_w = front_.width ();
  front_ = Raster (raster_w, hud_height, glyphs_);
  back_ = Raster (raster_w, hud_height, glyphs_);
}

height_t
TerminalCompositor::hud_height () const noexcept
{
  return hud_height_;
}

Raster &
TerminalCompositor::back_buffer () noexcept
{
  return back_;
}

GlyphTable &
TerminalCompositor::glyphs () const noexcept
{
  return glyphs_;
}

nxb::Size
TerminalCompositor::size () const noexcept
{
  return { back_.width (), back_.height () };
}

void
TerminalCompositor::present_frame ()
{
  present_frame (std::cout);
}

void
TerminalCompositor::present_frame (std::ostream &out)
{
  fmt::memory_buffer buf;
  ansi::Writer w (buf);

  // Save cursor so HUD rendering doesn't disturb log output position
  w.save_cursor ();

  // Offset for HUD mode: raster row 0 maps to hud_start_row_ on terminal
  // hud_start_row_ is a row_t (point), subtract origin to get quantity offset
  auto row_offset = hud_start_row_ - terminal_origin_v;

  // Track current colors to re-emit after SGR reset
  std::optional<Rgba8> current_fg;
  std::optional<Rgba8> current_bg;

  // Helper to emit color (palette or true color)
  auto emit_fg = [&w] (const Rgba8 &c) {
    if (c.is_palette ())
      w.fg_palette (c.palette_index ());
    else
      w.fg (c.to_rgb ());
  };

  auto emit_bg = [&w] (const Rgba8 &c) {
    if (c.is_palette ())
      w.bg_palette (c.palette_index ());
    else
      w.bg (c.to_rgb ());
  };

  diff_rasters (front_, back_,
                [&] (const ChangeRun &run)
                  {
                    // Offset position to HUD region
                    auto pos = Pos{ run.origin.x, run.origin.y + row_offset };
                    w.move_to (pos);

                    // Handle emphasis reset first (SGR 0 resets everything)
                    if (run.em_reset)
                      {
                        w.reset ();
                        // Re-emit colors after reset
                        if (current_fg)
                          emit_fg (*current_fg);
                        if (current_bg)
                          emit_bg (*current_bg);
                      }

                    // Update background
                    if (run.bg_reset)
                      {
                        w.bg_default ();
                        current_bg = std::nullopt;
                      }
                    else if (run.bg_change)
                      {
                        emit_bg (*run.bg_change);
                        current_bg = run.bg_change;
                      }

                    // Update foreground
                    if (run.fg_reset)
                      {
                        w.fg_default ();
                        current_fg = std::nullopt;
                      }
                    else if (run.fg_change)
                      {
                        emit_fg (*run.fg_change);
                        current_fg = run.fg_change;
                      }

                    // Apply new emphasis (cast to fmt::emphasis)
                    if (run.em_change)
                      w.style (static_cast<ansi::emphasis> (*run.em_change));

                    for (const auto gid : run.glyphs)
                      {
                        if (auto text = glyphs_.get (gid))
                          w.text (*text);
                      }
                  });

  // Restore cursor to where it was before HUD render
  w.restore_cursor ();

  out.write (buf.data (), static_cast<std::streamsize> (buf.size ()));
  out.flush ();

  std::swap (front_, back_);

  back_ = front_;
}

} // namespace nxb::ui
