#pragma once

#include <cstddef>

namespace nxb
{

/// Simple interval algebra for terminal row/column ranges.
/// Makes it explicit whether you're counting from top/bottom/left/right,
/// and whether bounds are 0-based (raster) or 1-based (ANSI terminal).

/// Row interval (ANSI 1-based coordinates, INCLUSIVE bounds)
struct RowSpan
{
  std::size_t first; // 1-based, included
  std::size_t last;  // 1-based, included

  [[nodiscard]] constexpr std::size_t
  height () const
  {
    if (last >= first)
      return last - first + 1;
    else
      return 0;
  }

  [[nodiscard]] constexpr bool
  contains (const std::size_t row_1based) const
  {
    return row_1based >= first && row_1based <= last;
  }
};

/// Column interval (ANSI 1-based coordinates, INCLUSIVE bounds)
struct ColSpan
{
  std::size_t first; // 1-based, included
  std::size_t last;  // 1-based, included

  [[nodiscard]] constexpr std::size_t
  width () const
  {
    if (last >= first)
      return last - first + 1;
    else
      return 0;
  }

  [[nodiscard]] constexpr bool
  contains (const std::size_t col_1based) const
  {
    return col_1based >= first && col_1based <= last;
  }
};

/// Builder for row spans starting from top
struct from_top_t
{
  std::size_t total_rows;

  /// Take N rows from top, return span [1, N]
  [[nodiscard]] constexpr RowSpan
  taking (const std::size_t n) const
  {
    const std::size_t last = n < total_rows ? n : total_rows;
    return { 1, last };
  }

  /// Take all but N rows from bottom, return span [1, total_rows - N]
  [[nodiscard]] constexpr RowSpan
  leaving (const std::size_t n) const
  {
    const std::size_t last = n < total_rows ? (total_rows - n) : 1;
    return { 1, last };
  }
};

/// Builder for row spans starting from bottom
struct from_bottom_t
{
  std::size_t total_rows;

  /// Take N rows from bottom, return span [total_rows - N + 1, total_rows]
  [[nodiscard]] constexpr RowSpan
  taking (const std::size_t n) const
  {
    const std::size_t first = n < total_rows ? total_rows - n + 1 : 1;
    return { first, total_rows };
  }

  /// Skip N rows from bottom, return span [N + 1, total_rows]
  [[nodiscard]] constexpr RowSpan
  skipping (const std::size_t n) const
  {
    const std::size_t first = n + 1;
    return { first, total_rows };
  }
};

/// Entry point: specify total number of rows
struct rows_t
{
  std::size_t total;

  [[nodiscard]] constexpr from_top_t
  from_top () const
  {
    return { total };
  }

  [[nodiscard]] constexpr from_bottom_t
  from_bottom () const
  {
    return { total };
  }

  /// Full span [1, total]
  [[nodiscard]] constexpr RowSpan
  all () const
  {
    return { 1, total };
  }
};

/// Entry point: specify total number of columns
struct cols_t
{
  std::size_t total;

  /// Full span [1, total]
  [[nodiscard]] constexpr ColSpan
  all () const
  {
    return { 1, total };
  }
};

/// Convenience: convert 0-based raster index to 1-based terminal coordinate
constexpr std::size_t
to_ansi (const std::size_t zero_based)
{
  return zero_based + 1;
}

/// Convenience: convert 1-based terminal coordinate to 0-based raster index
constexpr std::size_t
from_ansi (const std::size_t one_based)
{
  return one_based > 0 ? one_based - 1 : 0;
}

} // namespace nxb
