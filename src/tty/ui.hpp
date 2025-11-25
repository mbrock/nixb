#pragma once

#include "raster.hpp"

#include <boost/hana.hpp>
#include <concepts>
#include <string>

namespace nxb::ui2
{

namespace hana = boost::hana;

/// Simple size: width, height
struct Size
{
  std::size_t w = 0;
  std::size_t h = 0;
};

/// Rectangle for layout
struct Rect
{
  std::size_t x = 0, y = 0;
  std::size_t w = 0, h = 0;
};

/// Size hint for flex layout
struct SizeHint
{
  std::size_t fixed = 0; // Fixed size (0 = use content size)
  std::size_t grow = 0;  // Flex grow factor (0 = don't grow)

  static constexpr SizeHint
  content ()
  {
    return { 0, 0 };
  }

  static constexpr SizeHint
  fixed_size (std::size_t n)
  {
    return { n, 0 };
  }

  static constexpr SizeHint
  flex (std::size_t factor = 1)
  {
    return { 0, factor };
  }
};

// ============================================================================
// Concepts - the "sender/receiver" of layout
// ============================================================================

/// A widget can report its preferred size
template <typename W>
concept Measurable = requires (const W &w) {
  { w.preferred_size () } -> std::same_as<Size>;
};

/// A widget can be rendered to a raster
template <typename W>
concept Renderable = requires (const W &w, Raster &r) {
  { w.render (r) } -> std::same_as<void>;
};

/// A widget has a computed rect (set by layout)
template <typename W>
concept HasRect = requires (W &w) {
  { w.rect } -> std::convertible_to<Rect &>;
};

/// A widget has width hint for flex layout
template <typename W>
concept HasWidthHint = requires (const W &w) {
  { w.width_hint } -> std::convertible_to<SizeHint>;
};

/// A widget has height hint for flex layout
template <typename W>
concept HasHeightHint = requires (const W &w) {
  { w.height_hint } -> std::convertible_to<SizeHint>;
};

/// A widget can be laid out (has a layout method)
template <typename W>
concept Layoutable = requires (W &w, Rect r) {
  { w.layout (r) } -> std::same_as<void>;
};

/// Full Widget concept - can be measured, laid out, and rendered
template <typename W>
concept Widget = Measurable<W> && Renderable<W> && HasRect<W> && HasWidthHint<W>;

// ============================================================================
// Leaf widgets
// ============================================================================

/// Text widget - just a string
struct Text
{
  std::string content;
  Rgba8 color = Rgba8 (255, 255, 255);
  Rect rect{}; // Computed by layout

  SizeHint width_hint = SizeHint::content ();

  Size
  preferred_size () const
  {
    return { content.length (), 1 };
  }

  void
  render (Raster &raster) const
  {
    for (std::size_t i = 0; i < content.length () && i < rect.w; ++i)
      {
        if (rect.x + i < raster.width () && rect.y < raster.height ())
          {
            raster.set_char (rect.x + i, rect.y, content[i]);
            raster.set_fg (rect.x + i, rect.y, color);
          }
      }
  }
};

/// Box widget - filled rectangle
struct Box
{
  Rgba8 color = Rgba8 (100, 100, 100);
  Rect rect{};

  SizeHint width_hint = SizeHint::flex ();
  SizeHint height_hint = SizeHint::content ();

  Size
  preferred_size () const
  {
    return { 0, 1 }; // No preferred width, 1 row tall
  }

  void
  render (Raster &raster) const
  {
    for (std::size_t y = rect.y; y < rect.y + rect.h && y < raster.height ();
         ++y)
      {
        for (std::size_t x = rect.x;
             x < rect.x + rect.w && x < raster.width (); ++x)
          {
            raster.set_bg (x, y, color);
          }
      }
  }
};

// ============================================================================
// Layout containers
// ============================================================================

/// Row - horizontal flex layout
template <Widget... Children> struct Row
{
  hana::tuple<Children...> children;
  Rect rect{};
  Rgba8 bg_color = Rgba8::transparent ();

  static constexpr std::size_t child_count = sizeof...(Children);

  template <std::size_t I>
  auto &
  get ()
  {
    return hana::at_c<I> (children);
  }

  template <std::size_t I>
  const auto &
  get () const
  {
    return hana::at_c<I> (children);
  }

  Size
  preferred_size () const
  {
    Size total{ 0, 0 };
    hana::for_each (children,
                    [&] (const auto &child)
                      {
                        auto pref = child.preferred_size ();
                        total.w += pref.w;
                        total.h = std::max (total.h, pref.h);
                      });
    return total;
  }

  void
  layout (Rect container)
  {
    rect = container;

    // First pass: calculate fixed sizes and total grow
    std::array<std::size_t, child_count> sizes{};
    std::size_t total_fixed = 0;
    std::size_t total_grow = 0;
    std::size_t idx = 0;

    hana::for_each (children,
                    [&] (auto &child)
                      {
                        if (child.width_hint.fixed > 0)
                          {
                            sizes[idx] = child.width_hint.fixed;
                            total_fixed += sizes[idx];
                          }
                        else if (child.width_hint.grow > 0)
                          {
                            total_grow += child.width_hint.grow;
                          }
                        else
                          {
                            sizes[idx] = child.preferred_size ().w;
                            total_fixed += sizes[idx];
                          }
                        idx++;
                      });

    // Second pass: distribute remaining space to growing children
    std::size_t remaining
        = container.w > total_fixed ? container.w - total_fixed : 0;

    if (total_grow > 0)
      {
        idx = 0;
        hana::for_each (children,
                        [&] (auto &child)
                          {
                            if (child.width_hint.grow > 0)
                              {
                                sizes[idx]
                                    = (remaining * child.width_hint.grow)
                                      / total_grow;
                              }
                            idx++;
                          });
      }

    // Third pass: position children
    std::size_t x = container.x;
    idx = 0;
    hana::for_each (children,
                    [&] (auto &child)
                      {
                        child.rect
                            = { x, container.y, sizes[idx], container.h };

                        if constexpr (requires { child.layout (child.rect); })
                          {
                            child.layout (child.rect);
                          }

                        x += sizes[idx];
                        idx++;
                      });
  }

  void
  render (Raster &raster) const
  {
    // Fill background if not transparent
    if (bg_color != Rgba8::transparent ())
      {
        for (std::size_t y = rect.y;
             y < rect.y + rect.h && y < raster.height (); ++y)
          {
            for (std::size_t x = rect.x;
                 x < rect.x + rect.w && x < raster.width (); ++x)
              {
                raster.set_bg (x, y, bg_color);
              }
          }
      }

    // Render children
    hana::for_each (children,
                    [&] (const auto &child) { child.render (raster); });
  }
};

/// Column - vertical flex layout
template <Widget... Children> struct Column
{
  hana::tuple<Children...> children;
  Rect rect{};
  Rgba8 bg_color = Rgba8::transparent ();

  static constexpr std::size_t child_count = sizeof...(Children);

  template <std::size_t I>
  auto &
  get ()
  {
    return hana::at_c<I> (children);
  }

  template <std::size_t I>
  const auto &
  get () const
  {
    return hana::at_c<I> (children);
  }

  Size
  preferred_size () const
  {
    Size total{ 0, 0 };
    hana::for_each (children,
                    [&] (const auto &child)
                      {
                        auto pref = child.preferred_size ();
                        total.w = std::max (total.w, pref.w);
                        total.h += pref.h;
                      });
    return total;
  }

  void
  layout (Rect container)
  {
    rect = container;

    // First pass: calculate fixed sizes and total grow
    std::array<std::size_t, child_count> sizes{};
    std::size_t total_fixed = 0;
    std::size_t total_grow = 0;
    std::size_t idx = 0;

    hana::for_each (children,
                    [&] (auto &child)
                      {
                        SizeHint hint = SizeHint::content ();
                        if constexpr (requires { child.height_hint; })
                          {
                            hint = child.height_hint;
                          }

                        if (hint.fixed > 0)
                          {
                            sizes[idx] = hint.fixed;
                            total_fixed += sizes[idx];
                          }
                        else if (hint.grow > 0)
                          {
                            total_grow += hint.grow;
                          }
                        else
                          {
                            sizes[idx] = child.preferred_size ().h;
                            total_fixed += sizes[idx];
                          }
                        idx++;
                      });

    // Second pass: distribute remaining space to growing children
    std::size_t remaining
        = container.h > total_fixed ? container.h - total_fixed : 0;

    if (total_grow > 0)
      {
        idx = 0;
        hana::for_each (children,
                        [&] (auto &child)
                          {
                            SizeHint hint = SizeHint::content ();
                            if constexpr (requires { child.height_hint; })
                              {
                                hint = child.height_hint;
                              }

                            if (hint.grow > 0)
                              {
                                sizes[idx]
                                    = (remaining * hint.grow) / total_grow;
                              }
                            idx++;
                          });
      }

    // Third pass: position children
    std::size_t y = container.y;
    idx = 0;
    hana::for_each (children,
                    [&] (auto &child)
                      {
                        child.rect
                            = { container.x, y, container.w, sizes[idx] };

                        if constexpr (requires { child.layout (child.rect); })
                          {
                            child.layout (child.rect);
                          }

                        y += sizes[idx];
                        idx++;
                      });
  }

  void
  render (Raster &raster) const
  {
    // Fill background if not transparent
    if (bg_color != Rgba8::transparent ())
      {
        for (std::size_t y = rect.y;
             y < rect.y + rect.h && y < raster.height (); ++y)
          {
            for (std::size_t x = rect.x;
                 x < rect.x + rect.w && x < raster.width (); ++x)
              {
                raster.set_bg (x, y, bg_color);
              }
          }
      }

    // Render children
    hana::for_each (children,
                    [&] (const auto &child) { child.render (raster); });
  }
};

// ============================================================================
// Convenience constructors
// ============================================================================

template <Widget... Children>
Row<std::decay_t<Children>...>
row (Children &&...children)
{
  return { hana::make_tuple (std::forward<Children> (children)...) };
}

template <Widget... Children>
Column<std::decay_t<Children>...>
column (Children &&...children)
{
  return { hana::make_tuple (std::forward<Children> (children)...) };
}

} // namespace nxb::ui2
