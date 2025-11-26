#include "ansi.hpp"

#include <iterator>
#include <unistd.h>

namespace nxb::ansi
{

bool debug_mode = false;

bool
is_tty ()
{
  return isatty (STDOUT_FILENO) != 0;
}

void
init ()
{
  debug_mode = !is_tty ();
}

namespace
{
constexpr std::string_view CSI = "\x1b[";
constexpr std::string_view CSI_DEBUG = "⟨CSI:";
}

// ============================================================================
// Writer implementation (buffered output)
// ============================================================================

void
Writer::csi (std::string_view params, char final_byte)
{
  if (debug_mode)
    fmt::format_to (std::back_inserter (buf_), "{}{}{}⟩", CSI_DEBUG, params,
                    final_byte);
  else
    fmt::format_to (std::back_inserter (buf_), "{}{}{}", CSI, params,
                    final_byte);
}

Writer &
Writer::move_to (const ansi_row_t row, const ansi_col_t col)
{
  // ansi_origin is at terminal position -1, so the offset from ansi_origin
  // directly gives us the 1-based ANSI coordinate.
  const auto row_num = (row - ansi_origin_v).numerical_value_in (ln);
  const auto col_num = (col - ansi_origin).numerical_value_in (ch);
  csi (fmt::format ("{};{}", row_num, col_num), 'H');
  return *this;
}

Writer &
Writer::move_to (const Pos pos)
{
  return move_to (to_ansi_y (pos), to_ansi_x (pos));
}

Writer &
Writer::move_up (const height_t n)
{
  const auto rows = n.numerical_value_in (ln);
  if (rows > 0)
    csi (fmt::format ("{}", rows), 'A');
  return *this;
}

Writer &
Writer::move_down (const height_t n)
{
  const auto rows = n.numerical_value_in (ln);
  if (rows > 0)
    csi (fmt::format ("{}", rows), 'B');
  return *this;
}

Writer &
Writer::move_right (const width_t n)
{
  const auto cols = n.numerical_value_in (ch);
  if (cols > 0)
    csi (fmt::format ("{}", cols), 'C');
  return *this;
}

Writer &
Writer::move_left (const width_t n)
{
  const auto cols = n.numerical_value_in (ch);
  if (cols > 0)
    csi (fmt::format ("{}", cols), 'D');
  return *this;
}

Writer &
Writer::move (const Size delta)
{
  move_right (delta.w);
  move_down (delta.h);
  return *this;
}

Writer &
Writer::move_to_column (const ansi_col_t col)
{
  const auto col_num = (col - ansi_origin).numerical_value_in (ch);
  csi (fmt::format ("{}", col_num), 'G');
  return *this;
}

Writer &
Writer::clear_screen ()
{
  csi ("2", 'J');
  return *this;
}

Writer &
Writer::clear_screen_from_cursor ()
{
  csi ("0", 'J');
  return *this;
}

Writer &
Writer::clear_screen_to_cursor ()
{
  csi ("1", 'J');
  return *this;
}

Writer &
Writer::clear_line ()
{
  csi ("2", 'K');
  return *this;
}

Writer &
Writer::clear_line_from_cursor ()
{
  csi ("0", 'K');
  return *this;
}

Writer &
Writer::clear_line_to_cursor ()
{
  csi ("1", 'K');
  return *this;
}

Writer &
Writer::set_scroll_region (const row_t top, const row_t bottom)
{
  // Convert terminal row_t to 1-based ANSI row via ansi_origin_v
  const auto top_row = (top - ansi_origin_v).numerical_value_in (ln);
  const auto bottom_row = (bottom - ansi_origin_v).numerical_value_in (ln);
  csi (fmt::format ("{};{}", top_row, bottom_row), 'r');
  return *this;
}

Writer &
Writer::reset_scroll_region ()
{
  csi ("", 'r');
  return *this;
}

Writer &
Writer::scroll_up (const height_t n)
{
  const auto rows = n.numerical_value_in (ln);
  if (rows > 0)
    csi (fmt::format ("{}", rows), 'S');
  return *this;
}

Writer &
Writer::scroll_down (const height_t n)
{
  const auto rows = n.numerical_value_in (ln);
  if (rows > 0)
    csi (fmt::format ("{}", rows), 'T');
  return *this;
}

Writer &
Writer::hide_cursor ()
{
  fmt::format_to (std::back_inserter (buf_), "{}?25l", CSI);
  return *this;
}

Writer &
Writer::show_cursor ()
{
  fmt::format_to (std::back_inserter (buf_), "{}?25h", CSI);
  return *this;
}

Writer &
Writer::save_cursor ()
{
  fmt::format_to (std::back_inserter (buf_), "{}s", CSI);
  return *this;
}

Writer &
Writer::restore_cursor ()
{
  fmt::format_to (std::back_inserter (buf_), "{}u", CSI);
  return *this;
}

Writer &
Writer::fg (const rgb color)
{
  return fg (color.r, color.g, color.b);
}

Writer &
Writer::fg (std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
  csi (fmt::format ("38;2;{};{};{}", r, g, b), 'm');
  return *this;
}

Writer &
Writer::fg (const color c)
{
  return fg (rgb (c));
}

Writer &
Writer::bg (const rgb color)
{
  return bg (color.r, color.g, color.b);
}

Writer &
Writer::bg (std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
  csi (fmt::format ("48;2;{};{};{}", r, g, b), 'm');
  return *this;
}

Writer &
Writer::bg (const color c)
{
  return bg (rgb (c));
}

Writer &
Writer::fg (terminal_color c)
{
  csi (fmt::format ("{}", static_cast<int> (c)), 'm');
  return *this;
}

Writer &
Writer::bg (terminal_color c)
{
  // Background colors are +10 from foreground
  csi (fmt::format ("{}", static_cast<int> (c) + 10), 'm');
  return *this;
}

Writer &
Writer::fg_palette (std::uint8_t index)
{
  csi (fmt::format ("38;5;{}", index), 'm');
  return *this;
}

Writer &
Writer::bg_palette (std::uint8_t index)
{
  csi (fmt::format ("48;5;{}", index), 'm');
  return *this;
}

Writer &
Writer::fg_default ()
{
  csi ("39", 'm');
  return *this;
}

Writer &
Writer::bg_default ()
{
  csi ("49", 'm');
  return *this;
}

Writer &
Writer::style (emphasis e)
{
  // fmt::emphasis is a bitmask, emit all set bits
  if ((static_cast<std::uint8_t> (e)
       & static_cast<std::uint8_t> (emphasis::bold))
      != 0)
    csi ("1", 'm');
  if ((static_cast<std::uint8_t> (e)
       & static_cast<std::uint8_t> (emphasis::faint))
      != 0)
    csi ("2", 'm');
  if ((static_cast<std::uint8_t> (e)
       & static_cast<std::uint8_t> (emphasis::italic))
      != 0)
    csi ("3", 'm');
  if ((static_cast<std::uint8_t> (e)
       & static_cast<std::uint8_t> (emphasis::underline))
      != 0)
    csi ("4", 'm');
  if ((static_cast<std::uint8_t> (e)
       & static_cast<std::uint8_t> (emphasis::blink))
      != 0)
    csi ("5", 'm');
  if ((static_cast<std::uint8_t> (e)
       & static_cast<std::uint8_t> (emphasis::reverse))
      != 0)
    csi ("7", 'm');
  if ((static_cast<std::uint8_t> (e)
       & static_cast<std::uint8_t> (emphasis::conceal))
      != 0)
    csi ("8", 'm');
  if ((static_cast<std::uint8_t> (e)
       & static_cast<std::uint8_t> (emphasis::strikethrough))
      != 0)
    csi ("9", 'm');
  return *this;
}

Writer &
Writer::reset ()
{
  csi ("0", 'm');
  return *this;
}

Writer &
Writer::bold ()
{
  return style (emphasis::bold);
}

Writer &
Writer::dim ()
{
  return style (emphasis::faint);
}

Writer &
Writer::italic ()
{
  return style (emphasis::italic);
}

Writer &
Writer::underline ()
{
  return style (emphasis::underline);
}

Writer &
Writer::reverse ()
{
  return style (emphasis::reverse);
}

Writer &
Writer::text (std::string_view str)
{
  fmt::format_to (std::back_inserter (buf_), "{}", str);
  return *this;
}

// ============================================================================
// Standalone functions (immediate output to stdout)
// ============================================================================

namespace
{
/// Print a CSI sequence, using debug format if debug_mode is set
template <typename... Args>
void
print_csi (fmt::format_string<Args...> fmt_str, Args &&...args)
{
  if (debug_mode)
    {
      fmt::print ("{}", CSI_DEBUG);
      fmt::print (fmt_str, std::forward<Args> (args)...);
      fmt::print ("⟩");
    }
  else
    {
      fmt::print ("{}", CSI);
      fmt::print (fmt_str, std::forward<Args> (args)...);
    }
}
} // namespace

void
move_to (const ansi_row_t row, const ansi_col_t col)
{
  const auto row_num = (row - ansi_origin_v).numerical_value_in (ln);
  const auto col_num = (col - ansi_origin).numerical_value_in (ch);
  print_csi ("{};{}H", row_num, col_num);
}

void
move_to (const Pos pos)
{
  move_to (to_ansi_y (pos), to_ansi_x (pos));
}

void
clear_screen ()
{
  print_csi ("2J");
}

void
clear_line ()
{
  print_csi ("2K");
}

void
hide_cursor ()
{
  print_csi ("?25l");
}

void
show_cursor ()
{
  print_csi ("?25h");
}

void
set_scroll_region (const row_t top, const row_t bottom)
{
  const auto top_row = (top - ansi_origin_v).numerical_value_in (ln);
  const auto bottom_row = (bottom - ansi_origin_v).numerical_value_in (ln);
  print_csi ("{};{}r", top_row, bottom_row);
}

void
reset_scroll_region ()
{
  print_csi ("r");
}

void
scroll_up (const height_t n)
{
  const auto rows = n.numerical_value_in (ln);
  if (rows > 0)
    print_csi ("{}S", rows);
}

void
scroll_down (const height_t n)
{
  const auto rows = n.numerical_value_in (ln);
  if (rows > 0)
    print_csi ("{}T", rows);
}

TerminalGuard::TerminalGuard () { hide_cursor (); }

TerminalGuard::~TerminalGuard ()
{
  reset_scroll_region ();
  show_cursor ();
  clear_screen ();
  move_to (Pos::origin ());
  print_csi ("0m"); // Reset SGR
  std::fflush (stdout);
}

} // namespace nxb::ansi
