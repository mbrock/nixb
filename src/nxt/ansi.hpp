#pragma once

#include <fmt/base.h>
#include <fmt/color.h>

#include <optional>
#include <string_view>

#include "nxt/units.hpp"

namespace nxb::ansi {

/// ANSI output modes
enum class Mode {
    disabled,  // No ANSI output at all (default for non-TTY)
    debug,     // Readable debug format like ⟨CSI:0m⟩
    enabled    // Real ANSI escape sequences (default for TTY)
};

/// Current ANSI output mode
extern Mode mode;

/// Initialize the ANSI module. Sets mode based on TTY detection.
/// Call this early in main().
void init();

/// Check if stdout is connected to a real TTY
[[nodiscard]] bool is_tty();

// Re-export fmt's types for convenience
using rgb = fmt::rgb;
using color = fmt::color;
using terminal_color = fmt::terminal_color;
using emphasis = fmt::emphasis;

/// ANSI escape sequence builder that writes to a fmt::memory_buffer
class Writer
{
public:
    explicit Writer(fmt::memory_buffer & buf)
        : buf_(buf)
    {
    }

    /// Move cursor to terminal position (1-based coordinates)
    Writer & move_to(ansi_row_t y, ansi_col_t x);
    Writer & move_to(Pos pos);

    /// Move cursor relatively (positive deltas)
    Writer & move_up(height_t n = 1 * ln);
    Writer & move_down(height_t n = 1 * ln);
    Writer & move_right(width_t n = 1 * ch);
    Writer & move_left(width_t n = 1 * ch);
    Writer & move(Size delta); // right and down only

    /// Move to column (1-based)
    Writer & move_to_column(ansi_col_t col);

    /// Clear operations
    Writer & clear_screen();
    Writer & clear_screen_from_cursor();
    Writer & clear_screen_to_cursor();
    Writer & clear_line();
    Writer & clear_line_from_cursor();
    Writer & clear_line_to_cursor();

    /// Scroll region (INCLUSIVE bounds [top, bottom])
    Writer & set_scroll_region(row_t top, row_t bottom);
    Writer & reset_scroll_region();
    Writer & scroll_up(height_t n = 1 * ln);
    Writer & scroll_down(height_t n = 1 * ln);

    /// Cursor visibility
    Writer & hide_cursor();
    Writer & show_cursor();

    /// Save/restore cursor position
    Writer & save_cursor();
    Writer & restore_cursor();

    /// Request cursor position report (DSR 6). Response comes via stdin.
    Writer & request_cursor_position();

    /// Colors (24-bit RGB)
    Writer & fg(rgb color);
    Writer & fg(std::uint8_t r, std::uint8_t g, std::uint8_t b);
    Writer & fg(color c); // Named color (e.g., fmt::color::red)
    Writer & bg(rgb color);
    Writer & bg(std::uint8_t r, std::uint8_t g, std::uint8_t b);
    Writer & bg(color c); // Named color

    /// Terminal colors (16-color palette)
    Writer & fg(terminal_color c);
    Writer & bg(terminal_color c);

    /// 256-color palette
    Writer & fg_palette(std::uint8_t index);
    Writer & bg_palette(std::uint8_t index);

    /// Reset colors
    Writer & fg_default();
    Writer & bg_default();

    /// Text emphasis (uses fmt's emphasis enum)
    Writer & style(emphasis e);
    Writer & reset();
    Writer & bold();
    Writer & dim();
    Writer & italic();
    Writer & underline();
    Writer & reverse();

    /// Write raw text (no escaping)
    Writer & text(std::string_view str);

    /// Write a single character
    Writer & text(const char ch)
    {
        buf_.push_back(ch);
        return *this;
    }

    /// Get the underlying buffer
    [[nodiscard]] fmt::memory_buffer & buffer()
    {
        return buf_;
    }

    [[nodiscard]] const fmt::memory_buffer & buffer() const
    {
        return buf_;
    }

private:
    fmt::memory_buffer & buf_;

    /// Helper: write CSI sequence
    void csi(std::string_view params, char final_byte);
};

/// Standalone functions for immediate output (writes directly to
/// stdout)

void move_to(ansi_row_t row, ansi_col_t col);
void move_to(Pos pos);
void clear_screen();
void clear_line();
void hide_cursor();
void show_cursor();
void set_scroll_region(row_t top, row_t bottom);
void reset_scroll_region();
void scroll_up(height_t n = 1 * ln);
void scroll_down(height_t n = 1 * ln);

/// Query current cursor position (blocking).
/// Sends DSR 6 and reads CPR response from stdin.
/// Returns nullopt if query fails (not a TTY, timeout, parse error).
/// Requires terminal to be in raw mode for reliable response reading.
[[nodiscard]] std::optional<Pos> query_cursor_position();

/// RAII guard that hides cursor on construction, restores terminal
/// state on destruction. Resets scroll region, shows cursor, clears
/// screen.
struct TerminalGuard
{
    TerminalGuard();
    ~TerminalGuard();

    TerminalGuard(const TerminalGuard &) = delete;
    TerminalGuard & operator=(const TerminalGuard &) = delete;
};

} // namespace nxb::ansi
