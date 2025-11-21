#pragma once

namespace nixb::ansi
{

// Move cursor to specific row and column (1-based).
void move_cursor (int row, int col);

// Clear the current line.
void clear_line ();

// Set the scrolling region to be from top_row to bottom_row (inclusive,
// 1-based).
void set_scroll_region (int top_row, int bottom_row);

// Reset scrolling region to full screen.
void reset_scroll_region ();

// Scroll the current region up (positive count).
void scroll_up (int count);

// Scroll the current region down (positive count).
void scroll_down (int count);

// Hide the cursor.
void hide_cursor ();

// Show the cursor.
void show_cursor ();

} // namespace nixb::ansi
