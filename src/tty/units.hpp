#pragma once

/// Terminal grid units using mp-units for type-safe 2D layout.
///
/// This header defines the foundational unit system for terminal coordinates:
/// - Dimensions: horizontal (X) and vertical (Y)
/// - Units: ch (character width) and ln (line height)
/// - Quantities: width_t, height_t for displacements
/// - Points: col_t, row_t for absolute positions (affine space)
/// - Composites: Size (2D extent) and Pos (2D point)

#include <mp-units/framework.h>

namespace nxb
{

using namespace mp_units;

// ============================================================================
// Base Dimensions
// ============================================================================

inline constexpr struct dim_horizontal final : base_dimension<"X">
{
} dim_horizontal;

inline constexpr struct dim_vertical final : base_dimension<"Y">
{
} dim_vertical;

// ============================================================================
// Quantity Specifications
// ============================================================================

QUANTITY_SPEC (horizontal_extent, dim_horizontal);
QUANTITY_SPEC (vertical_extent, dim_vertical);

// ============================================================================
// Units
// ============================================================================

/// Character width unit (horizontal grid cell)
inline constexpr struct ch final : named_unit<"ch", kind_of<horizontal_extent>>
{
} ch;

/// Line height unit (vertical grid cell)
inline constexpr struct ln final : named_unit<"ln", kind_of<vertical_extent>>
{
} ln;

// ============================================================================
// Displacement Types (Vectors)
// ============================================================================

/// Horizontal displacement (number of characters)
using width_t = quantity<ch, std::size_t>;

/// Vertical displacement (number of lines)
using height_t = quantity<ln, std::size_t>;

// ============================================================================
// Dimensionless Quantities
// ============================================================================

/// Ratio for flex factors (0.0 to 1.0)
using ratio_t = quantity<one, double>;

/// Percentage (0% to 100%)
using percent_t = quantity<percent, double>;

// ============================================================================
// Affine Space (Points vs Vectors)
// ============================================================================

/// Absolute origin for horizontal terminal coordinates
inline constexpr struct terminal_origin final
    : absolute_point_origin<horizontal_extent>
{
} terminal_origin;

/// Absolute origin for vertical terminal coordinates
inline constexpr struct terminal_origin_v final
    : absolute_point_origin<vertical_extent>
{
} terminal_origin_v;

/// Column position (absolute X coordinate in terminal)
using col_t = quantity_point<ch, terminal_origin>;

/// Row position (absolute Y coordinate in terminal)
using row_t = quantity_point<ln, terminal_origin_v>;

// ============================================================================
// Composite Types
// ============================================================================

/// 2D extent/displacement (width x height)
struct Size
{
  width_t w{ 0 * ch };
  height_t h{ 0 * ln };
};

/// 2D point in terminal space (column, row)
struct Pos
{
  col_t x = terminal_origin + 0 * ch;
  row_t y = terminal_origin_v + 0 * ln;

  /// Create position at the origin (0, 0)
  static constexpr Pos
  origin ()
  {
    return {};
  }

  /// Create position from displacement offsets
  static constexpr Pos
  at (width_t dx, height_t dy)
  {
    return { terminal_origin + dx, terminal_origin_v + dy };
  }

  /// Offset by horizontal displacement (point + vector = point)
  constexpr Pos
  operator+ (width_t dx) const
  {
    return { x + dx, y };
  }

  /// Offset by vertical displacement (point + vector = point)
  constexpr Pos
  operator+ (height_t dy) const
  {
    return { x, y + dy };
  }

  /// Offset by 2D displacement (point + vector = point)
  constexpr Pos
  operator+ (Size delta) const
  {
    return { x + delta.w, y + delta.h };
  }

  /// Difference of positions gives displacement (point - point = vector)
  friend constexpr Size
  operator- (Pos a, Pos b)
  {
    auto dx = (a.x - b.x).numerical_value_in (ch);
    auto dy = (a.y - b.y).numerical_value_in (ln);
    return { static_cast<std::size_t> (dx) * ch,
             static_cast<std::size_t> (dy) * ln };
  }

  /// Get displacement from terminal origin
  constexpr Size
  from_origin () const
  {
    return *this - Pos::origin ();
  }

  /// Extract raw column index (for legacy API interop)
  [[nodiscard]] constexpr std::size_t
  col () const
  {
    return static_cast<std::size_t> (
        (x - terminal_origin).numerical_value_in (ch));
  }

  /// Extract raw row index (for legacy API interop)
  [[nodiscard]] constexpr std::size_t
  row () const
  {
    return static_cast<std::size_t> (
        (y - terminal_origin_v).numerical_value_in (ln));
  }

  /// Equality comparison
  friend constexpr bool
  operator== (Pos a, Pos b)
  {
    return a.x == b.x && a.y == b.y;
  }
};

} // namespace nxb
