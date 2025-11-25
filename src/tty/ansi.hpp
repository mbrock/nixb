#pragma once

#include <fmt/color.h>
#include <fmt/format.h>
#include <string_view>

namespace nxb::ansi
{

// Re-export fmt's types for convenience
using rgb = fmt::rgb;
using color = fmt::color;
using terminal_color = fmt::terminal_color;
using emphasis = fmt::emphasis;

/// ANSI escape sequence builder that writes to a fmt::memory_buffer
class Writer
{
public:
  explicit Writer (fmt::memory_buffer &buf) : buf_ (buf) {}

  /// Move cursor to terminal position (1-based coordinates)
  Writer &move_to (std::size_t row, std::size_t col);

  /// Move cursor relatively
  Writer &move_up (std::size_t n = 1);
  Writer &move_down (std::size_t n = 1);
  Writer &move_right (std::size_t n = 1);
  Writer &move_left (std::size_t n = 1);

  /// Move to column (1-based)
  Writer &move_to_column (std::size_t col);

  /// Clear operations
  Writer &clear_screen ();
  Writer &clear_screen_from_cursor ();
  Writer &clear_screen_to_cursor ();
  Writer &clear_line ();
  Writer &clear_line_from_cursor ();
  Writer &clear_line_to_cursor ();

  /// Scroll region (INCLUSIVE bounds [top, bottom])
  Writer &set_scroll_region (std::size_t top, std::size_t bottom);
  Writer &reset_scroll_region ();
  Writer &scroll_up (std::size_t n = 1);
  Writer &scroll_down (std::size_t n = 1);

  /// Cursor visibility
  Writer &hide_cursor ();
  Writer &show_cursor ();

  /// Save/restore cursor position
  Writer &save_cursor ();
  Writer &restore_cursor ();

  /// Colors (24-bit RGB)
  Writer &fg (rgb color);
  Writer &fg (std::uint8_t r, std::uint8_t g, std::uint8_t b);
  Writer &fg (color c); // Named color (e.g., fmt::color::red)
  Writer &bg (rgb color);
  Writer &bg (std::uint8_t r, std::uint8_t g, std::uint8_t b);
  Writer &bg (color c); // Named color

  /// Terminal colors (16-color palette)
  Writer &fg (terminal_color c);
  Writer &bg (terminal_color c);

  /// Reset colors
  Writer &fg_default ();
  Writer &bg_default ();

  /// Text emphasis (uses fmt's emphasis enum)
  Writer &style (emphasis e);
  Writer &reset ();
  Writer &bold ();
  Writer &dim ();
  Writer &italic ();
  Writer &underline ();
  Writer &reverse ();

  /// Write raw text (no escaping)
  Writer &text (std::string_view str);

  /// Write a single character
  Writer &
  text (const char ch)
  {
    buf_.push_back (ch);
    return *this;
  }

  /// Get the underlying buffer
  [[nodiscard]] fmt::memory_buffer &
  buffer ()
  {
    return buf_;
  }
  [[nodiscard]] const fmt::memory_buffer &
  buffer () const
  {
    return buf_;
  }

private:
  fmt::memory_buffer &buf_;

  /// Helper: write CSI sequence
  void csi (std::string_view params, char final_byte);
};

/// Standalone functions for immediate output (writes directly to stdout)

void move_to (std::size_t row, std::size_t col);
void clear_screen ();
void clear_line ();
void hide_cursor ();
void show_cursor ();
void set_scroll_region (std::size_t top, std::size_t bottom);
void reset_scroll_region ();
void scroll_up (std::size_t n = 1);
void scroll_down (std::size_t n = 1);

} // namespace nxb::ansi
