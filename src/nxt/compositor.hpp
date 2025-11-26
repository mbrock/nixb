#pragma once

#include "nxt/glyph-table.hpp"
#include "nxt/raster.hpp"
#include "nxt/units.hpp"

#include <iosfwd>

namespace nxb::ui
{

  /// Double-buffered terminal compositor with HUD/scroll region support.
  class TerminalCompositor
  {
  public:
    TerminalCompositor (nxb::Size size, GlyphTable &glyphs);
    void resize (nxb::Size size);

    Raster &back_buffer () noexcept;
    GlyphTable &glyphs () const noexcept;
    nxb::Size size () const noexcept;

    /// Set HUD height. Raster covers only the bottom hud_height rows.
    /// The scroll region is set to rows above the HUD.
    /// If hud_height >= terminal height, no scroll region (full-screen
    /// mode).
    void set_hud_height (height_t hud_height, height_t term_height);
    [[nodiscard]] height_t hud_height () const noexcept;

    // Public for testing the rendering pipeline without async runtime
    void present_frame ();
    void present_frame (std::ostream &out);

  private:
    Raster front_;
    Raster back_;
    GlyphTable &glyphs_;
    height_t hud_height_{ 0 * ln }; // 0 = full-screen mode
    row_t hud_start_row_{
      terminal_origin_v + 0 * ln
    }; // row where HUD starts
  };

} // namespace nxb::ui
