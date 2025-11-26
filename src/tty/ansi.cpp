#include "ansi.hpp"

#include <iterator>

namespace nxb::ansi
{

namespace
{
constexpr std::string_view CSI = "\x1b[";
}

// ============================================================================
// Writer implementation (buffered output)
// ============================================================================

void
Writer::csi (std::string_view params, char final_byte)
{
  fmt::format_to (std::back_inserter (buf_), "{}{}{}", CSI, params,
                  final_byte);
}

Writer &
Writer::move_to (const ansi_row_t row, const ansi_col_t col)
{
  // ANSI coordinates are 1-based. ansi_row_t/ansi_col_t use ansi_origin
  // which is offset by 1 from terminal_origin.
  // ansi_origin = terminal_origin + 1, so to get 1-based number:
  // terminal position 0 -> ansi position 1
  // (pos - terminal_origin) + 1 = ansi 1-based number
  const auto row_num
      = (row.quantity_from (terminal_origin_v)).numerical_value_in (ln) + 1;
  const auto col_num
      = (col.quantity_from (terminal_origin)).numerical_value_in (ch) + 1;
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
  const auto col_num = static_cast<std::size_t> (
      (col - terminal_origin).numerical_value_in (ch));
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
  const auto top_row = static_cast<std::size_t> (
      (to_ansi (top) - terminal_origin_v).numerical_value_in (ln));
  const auto bottom_row = static_cast<std::size_t> (
      (to_ansi (bottom) - terminal_origin_v).numerical_value_in (ln));
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

void
move_to (const ansi_row_t row, const ansi_col_t col)
{
  const auto row_num
      = (row.quantity_from (terminal_origin_v)).numerical_value_in (ln) + 1;
  const auto col_num
      = (col.quantity_from (terminal_origin)).numerical_value_in (ch) + 1;
  fmt::print ("{}{};{}H", CSI, row_num, col_num);
}

void
move_to (const Pos pos)
{
  move_to (to_ansi_y (pos), to_ansi_x (pos));
}

void
clear_screen ()
{
  fmt::print ("{}2J", CSI);
}

void
clear_line ()
{
  fmt::print ("{}2K", CSI);
}

void
hide_cursor ()
{
  fmt::print ("{}?25l", CSI);
}

void
show_cursor ()
{
  fmt::print ("{}?25h", CSI);
}

void
set_scroll_region (const row_t top, const row_t bottom)
{
  const auto top_row = static_cast<std::size_t> (
      (to_ansi (top) - terminal_origin_v).numerical_value_in (ln));
  const auto bottom_row = static_cast<std::size_t> (
      (to_ansi (bottom) - terminal_origin_v).numerical_value_in (ln));
  fmt::print ("{}{};{}r", CSI, top_row, bottom_row);
}

void
reset_scroll_region ()
{
  fmt::print ("{}r", CSI);
}

void
scroll_up (const height_t n)
{
  const auto rows = n.numerical_value_in (ln);
  if (rows > 0)
    fmt::print ("{}{}S", CSI, rows);
}

void
scroll_down (const height_t n)
{
  const auto rows = n.numerical_value_in (ln);
  if (rows > 0)
    fmt::print ("{}{}T", CSI, rows);
}

} // namespace nxb::ansi
