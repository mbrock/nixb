#include "Ansi.hpp"

#include <fmt/core.h>

namespace nixb::ansi
{

namespace
{
constexpr const char *CSI = "\x1b[";
}

void
move_cursor (int row, int col)
{
  fmt::print ("{}{};{}H", CSI, row, col);
}

void
clear_line ()
{
  fmt::print ("{}2K", CSI);
}

void
set_scroll_region (int top_row, int bottom_row)
{
  fmt::print ("{}{};{}r", CSI, top_row, bottom_row);
}

void
reset_scroll_region ()
{
  fmt::print ("{}r", CSI);
}

void
scroll_up (int count)
{
  if (count > 0)
    fmt::print ("{}{}S", CSI, count);
}

void
scroll_down (int count)
{
  if (count > 0)
    fmt::print ("{}{}T", CSI, count);
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

} // namespace nixb::ansi
