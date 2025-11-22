#pragma once

#include <cstddef>
#include <fmt/color.h>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "raster.hpp"

namespace nxb::ui
{
using nxb::Rgba8;

/// Forward declarations
class Dom;
struct Node;

/// Handle to a node in the DOM (stable across updates)
struct NodeId
{
  std::size_t value;

  explicit constexpr NodeId (const std::size_t v = 0) : value (v) {}
  constexpr auto operator<=> (const NodeId &) const = default;

  [[nodiscard]] constexpr bool
  is_valid () const
  {
    return value != 0;
  }

  static constexpr NodeId
  null ()
  {
    return NodeId{ 0 };
  }
};

/// Flex layout direction
enum class FlexDir : std::uint8_t
{
  Row,
  Column
};

/// Justify content (main axis)
enum class Justify : std::uint8_t
{
  Start,
  Center,
  End,
  SpaceBetween,
  SpaceAround
};

/// Align items (cross axis)
enum class Align : std::uint8_t
{
  Start,
  Center,
  End,
  Stretch
};

/// Size specification
struct Size
{
  std::size_t value = 0;
  bool is_grow = false; // If true, value is flex-grow factor

  static constexpr Size
  fixed (const std::size_t v)
  {
    return { v, false };
  }
  static constexpr Size
  grow (const std::size_t factor = 1)
  {
    return { factor, true };
  }
};
/// Simple style (no string parsing needed)
struct Style
{
  FlexDir flex_dir = FlexDir::Row;
  Justify justify = Justify::Start;
  Align align = Align::Stretch;

  Size width{};
  Size height{};

  char bg_glyph = ' '; // Background fill character
  Rgba8 fg_color = Rgba8::transparent ();
  Rgba8 bg_color = Rgba8::transparent ();

  [[nodiscard]] static Style
  defaults ()
  {
    return {};
  }
};

struct Text
{
  std::string content;
  Rgba8 color = Rgba8::transparent ();
};

struct Rect
{
  std::size_t x = 0, y = 0;
  std::size_t w = 0, h = 0;
};

struct Element
{
  Style style;
  std::vector<NodeId> children;
};

struct NodeData
{
  NodeId parent = NodeId::null ();
  std::variant<Element, Text> content;
  Rect rect; // Computed by layout
};

class Dom
{
public:
  Dom ();

  [[nodiscard]] NodeId create_element (const Style &style
                                       = Style::defaults ());

  [[nodiscard]] NodeId create_text (std::string content,
                                    fmt::color color = fmt::color::white);

  void append_child (NodeId parent, NodeId child);

  void update_text (NodeId node, std::string new_text,
                    std::optional<fmt::color> color = std::nullopt);

  void update_style (NodeId node, const Style &new_style);

  [[nodiscard]] const NodeData &get (NodeId node) const;

  [[nodiscard]] NodeData &get_mut (NodeId node);

  [[nodiscard]] bool
  is_dirty () const
  {
    return dirty_;
  }

  void
  mark_clean ()
  {
    dirty_ = false;
  }

  [[nodiscard]] NodeId
  root () const
  {
    return root_;
  }

  [[nodiscard]] const std::vector<NodeData> &
  nodes () const
  {
    return nodes_;
  }

private:
  std::vector<NodeData> nodes_;
  NodeId root_;
  bool dirty_ = true;

  friend class LayoutEngine;
};

} // namespace nxb::ui
