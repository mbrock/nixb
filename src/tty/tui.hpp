#pragma once

#include "raster.hpp"

#include <concepts>
#include <span>
#include <string>
#include <string_view>

namespace nxb::tui
{

// ============================================================================
// Core Types
// ============================================================================

struct Size
{
  std::size_t w = 0;
  std::size_t h = 0;
};

struct SizeHint
{
  std::size_t min = 0;  // Minimum size needed
  std::size_t flex = 0; // Flex grow factor (0 = don't grow)

  static constexpr SizeHint
  fixed (std::size_t n)
  {
    return { n, 0 };
  }

  static constexpr SizeHint
  grow (std::size_t factor = 1)
  {
    return { 0, factor };
  }
};

// ============================================================================
// Layout Concept
// ============================================================================

template <typename L>
concept Layout = requires (const L &layout, Raster &raster, Size size) {
  { layout.width_hint () } -> std::convertible_to<SizeHint>;
  { layout.height_hint () } -> std::convertible_to<SizeHint>;
  { layout.render (raster, size) } -> std::same_as<void>;
};

// ============================================================================
// Leaf: generic single-row layout from a render function
// ============================================================================

template <typename RenderFn>
struct Leaf
{
  SizeHint w_hint;
  SizeHint h_hint;
  RenderFn render_fn;

  constexpr SizeHint
  width_hint () const
  {
    return w_hint;
  }

  constexpr SizeHint
  height_hint () const
  {
    return h_hint;
  }

  void
  render (Raster &raster, Size size) const
  {
    render_fn (raster, size);
  }
};

template <typename F>
auto
leaf (SizeHint w, SizeHint h, F &&f)
{
  return Leaf<std::decay_t<F>>{ w, h, std::forward<F> (f) };
}

// ============================================================================
// Styled span: text + colors (building block for leaf content)
// ============================================================================

struct Span
{
  std::string text;
  Rgba8 fg = Rgba8::white ();
  Rgba8 bg = Rgba8::transparent ();
};

// Render a span to a raster at position 0,0
inline void
render_span (Raster &r, const Span &s)
{
  r.write_text (0, 0, s.text, s.fg, s.bg);
}

// ============================================================================
// String utilities
// ============================================================================

// Repeat a single-char string n times
inline std::string
repeat (std::string_view ch, std::size_t n)
{
  std::string result;
  result.reserve (ch.size () * n);
  for (std::size_t i = 0; i < n; ++i)
    result += ch;
  return result;
}

// Count UTF-8 code points (approximate display width)
inline std::size_t
utf8_width (std::string_view s)
{
  std::size_t width = 0;
  for (std::size_t i = 0; i < s.size ();)
    {
      unsigned char c = static_cast<unsigned char> (s[i]);
      if ((c & 0x80) == 0)
        i += 1;
      else if ((c & 0xE0) == 0xC0)
        i += 2;
      else if ((c & 0xF0) == 0xE0)
        i += 3;
      else
        i += 4;
      ++width;
    }
  return width;
}

// ============================================================================
// Primitives
// ============================================================================

// Text: fixed-width string
inline auto
text (std::string s, Rgba8 fg = Rgba8::white (), Rgba8 bg = Rgba8::transparent ())
{
  auto w = utf8_width (s);
  return leaf (SizeHint::fixed (w), SizeHint::fixed (1),
               [=] (Raster &r, Size) { r.write_text (0, 0, s, fg, bg); });
}

// Fill: solid color rectangle (grows in both dimensions)
inline auto
fill (Rgba8 color = Rgba8 (60, 60, 60))
{
  return leaf (SizeHint::grow (), SizeHint::grow (),
               [=] (Raster &r, Size size)
               {
                 for (std::size_t y = 0; y < size.h; ++y)
                   for (std::size_t x = 0; x < size.w; ++x)
                     r.set_bg (x, y, color);
               });
}

// Horizontal rule: box drawing character repeated
inline auto
hrule (Rgba8 color = Rgba8 (80, 80, 100))
{
  return leaf (SizeHint::grow (), SizeHint::fixed (1),
               [=] (Raster &r, Size size)
               { r.write_text (0, 0, repeat ("─", size.w), color); });
}

// Bar string: pure function (fraction, width) → string of filled blocks
inline std::string
bar_string (float fraction, std::size_t width)
{
  static constexpr std::array<std::string_view, 9> partials
      = { "", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█" };

  double fill = std::clamp (fraction, 0.0f, 1.0f) * width;
  std::size_t full = static_cast<std::size_t> (fill);
  std::size_t partial = static_cast<std::size_t> ((fill - full) * 8 + 0.5);

  if (partial >= 8)
    {
      full++;
      partial = 0;
    }

  return repeat ("█", std::min (full, width)) + std::string (partials[partial]);
}

// Progress bar: bg color + fg bar string
inline auto
progress_bar (float fraction, Rgba8 fg = Rgba8 (100, 180, 255),
              Rgba8 bg = Rgba8 (50, 50, 50))
{
  return leaf (SizeHint::grow (), SizeHint::fixed (1),
               [=] (Raster &r, Size size)
               {
                 for (std::size_t x = 0; x < size.w; ++x)
                   r.set_bg (x, 0, bg);
                 r.write_text (0, 0, bar_string (fraction, size.w), fg);
               });
}

// ============================================================================
// Row (horizontal flex layout)
// ============================================================================

template <Layout... Children>
struct Row
{
  std::tuple<Children...> children;

  constexpr SizeHint
  width_hint () const
  {
    std::size_t total_min = 0;
    std::size_t total_flex = 0;
    std::apply (
        [&] (const auto &...c)
        {
          ((total_min += c.width_hint ().min, total_flex += c.width_hint ().flex),
           ...);
        },
        children);
    return { total_min, total_flex };
  }

  constexpr SizeHint
  height_hint () const
  {
    std::size_t max_min = 0;
    std::apply (
        [&] (const auto &...c)
        { ((max_min = std::max (max_min, c.height_hint ().min)), ...); },
        children);
    return SizeHint::fixed (max_min > 0 ? max_min : 1);
  }

  void
  render (Raster &raster, Size size) const
  {
    constexpr std::size_t N = sizeof...(Children);
    if constexpr (N == 0)
      return;

    // Collect hints
    std::array<SizeHint, N> hints;
    std::size_t i = 0;
    std::apply ([&] (const auto &...c) { ((hints[i++] = c.width_hint ()), ...); },
                children);

    // Calculate widths
    auto widths = flex_distribute (size.w, hints);

    // Render children
    std::size_t x = 0;
    i = 0;
    std::apply (
        [&] (const auto &...c)
        {
          (
              [&]
              {
                if (widths[i] > 0)
                  {
                    auto sub = raster.subraster (x, 0, widths[i], size.h);
                    c.render (sub, Size{ widths[i], size.h });
                    x += widths[i];
                  }
                ++i;
              }(),
              ...);
        },
        children);
  }

private:
  template <std::size_t N>
  static std::array<std::size_t, N>
  flex_distribute (std::size_t total, const std::array<SizeHint, N> &hints)
  {
    std::array<std::size_t, N> result{};

    std::size_t used = 0;
    std::size_t total_flex = 0;
    for (std::size_t i = 0; i < N; ++i)
      {
        result[i] = hints[i].min;
        used += hints[i].min;
        total_flex += hints[i].flex;
      }

    if (total_flex > 0 && total > used)
      {
        std::size_t remaining = total - used;
        for (std::size_t i = 0; i < N; ++i)
          {
            if (hints[i].flex > 0)
              result[i] += (remaining * hints[i].flex) / total_flex;
          }
      }

    return result;
  }
};

template <Layout... Children>
Row<std::decay_t<Children>...>
row (Children &&...children)
{
  return { std::tuple{ std::forward<Children> (children)... } };
}

// ============================================================================
// Column (vertical flex layout)
// ============================================================================

template <Layout... Children>
struct Column
{
  std::tuple<Children...> children;

  constexpr SizeHint
  width_hint () const
  {
    std::size_t max_min = 0;
    std::apply (
        [&] (const auto &...c)
        { ((max_min = std::max (max_min, c.width_hint ().min)), ...); },
        children);
    return { max_min, 1 };
  }

  constexpr SizeHint
  height_hint () const
  {
    std::size_t total_min = 0;
    std::size_t total_flex = 0;
    std::apply (
        [&] (const auto &...c)
        {
          ((total_min += c.height_hint ().min, total_flex += c.height_hint ().flex),
           ...);
        },
        children);
    return { total_min, total_flex };
  }

  void
  render (Raster &raster, Size size) const
  {
    constexpr std::size_t N = sizeof...(Children);
    if constexpr (N == 0)
      return;

    std::array<SizeHint, N> hints;
    std::size_t i = 0;
    std::apply ([&] (const auto &...c) { ((hints[i++] = c.height_hint ()), ...); },
                children);

    auto heights = flex_distribute (size.h, hints);

    std::size_t y = 0;
    i = 0;
    std::apply (
        [&] (const auto &...c)
        {
          (
              [&]
              {
                if (heights[i] > 0)
                  {
                    auto sub = raster.subraster (0, y, size.w, heights[i]);
                    c.render (sub, Size{ size.w, heights[i] });
                    y += heights[i];
                  }
                ++i;
              }(),
              ...);
        },
        children);
  }

private:
  template <std::size_t N>
  static std::array<std::size_t, N>
  flex_distribute (std::size_t total, const std::array<SizeHint, N> &hints)
  {
    std::array<std::size_t, N> result{};

    std::size_t used = 0;
    std::size_t total_flex = 0;
    for (std::size_t i = 0; i < N; ++i)
      {
        result[i] = hints[i].min;
        used += hints[i].min;
        total_flex += hints[i].flex;
      }

    if (total_flex > 0 && total > used)
      {
        std::size_t remaining = total - used;
        for (std::size_t i = 0; i < N; ++i)
          {
            if (hints[i].flex > 0)
              result[i] += (remaining * hints[i].flex) / total_flex;
          }
      }

    return result;
  }
};

template <Layout... Children>
Column<std::decay_t<Children>...>
column (Children &&...children)
{
  return { std::tuple{ std::forward<Children> (children)... } };
}

// ============================================================================
// List (dynamic collection of uniform items)
// ============================================================================

template <typename T, typename ViewFn>
struct List
{
  std::span<const T> items;
  ViewFn view;

  constexpr SizeHint
  width_hint () const
  {
    return SizeHint::grow ();
  }

  constexpr SizeHint
  height_hint () const
  {
    return SizeHint::fixed (items.size ());
  }

  void
  render (Raster &raster, Size size) const
  {
    std::size_t y = 0;
    for (const auto &item : items)
      {
        if (y >= size.h)
          break;
        auto child = view (item);
        auto sub = raster.subraster (0, y, size.w, 1);
        child.render (sub, Size{ size.w, 1 });
        ++y;
      }
  }
};

template <typename T, typename ViewFn>
List<T, ViewFn>
list (std::span<const T> items, ViewFn &&view)
{
  return { items, std::forward<ViewFn> (view) };
}

template <typename T, typename ViewFn>
auto
list (const std::vector<T> &items, ViewFn &&view)
{
  return list (std::span<const T> (items), std::forward<ViewFn> (view));
}

} // namespace nxb::tui
