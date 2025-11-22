#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace nxb::ui
{

/// Forward declarations
class Dom;
struct Node;

/// Handle to a node in the DOM (stable across updates)
struct NodeId
{
  std::size_t value;

  explicit constexpr NodeId (std::size_t v = 0) : value (v) {}
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
  fixed (std::size_t v)
  {
    return { v, false };
  }
  static constexpr Size
  grow (std::size_t factor = 1)
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

  [[nodiscard]] static Style
  defaults ()
  {
    return {};
  }
};

/// Rectangle (layout result)
struct Rect
{
  std::size_t x = 0, y = 0;
  std::size_t w = 0, h = 0;
};

/// Node types
struct Element
{
  Style style;
  std::vector<NodeId> children;
};

struct Text
{
  std::string content;
};

/// Node data
struct NodeData
{
  NodeId parent = NodeId::null ();
  std::variant<Element, Text> content;
  Rect rect; // Computed by layout
};

/// Persistent DOM tree
/// Nodes are stable (indexed by NodeId), can be updated by coroutines
class Dom
{
public:
  Dom ();

  /// Create element node with style
  [[nodiscard]] NodeId create_element (Style style = Style::defaults ());

  /// Create text node
  [[nodiscard]] NodeId create_text (std::string content);

  /// Add child to parent
  void append_child (NodeId parent, NodeId child);

  /// Update text content (triggers dirty flag)
  void update_text (NodeId node, std::string new_text);

  /// Update style (triggers dirty flag)
  void update_style (NodeId node, Style new_style);

  /// Get node data (const)
  [[nodiscard]] const NodeData &get (NodeId node) const;

  /// Get mutable node data
  [[nodiscard]] NodeData &get_mut (NodeId node);

  /// Check if layout is needed
  [[nodiscard]] bool
  is_dirty () const
  {
    return dirty_;
  }

  /// Clear dirty flag (called after layout)
  void
  mark_clean ()
  {
    dirty_ = false;
  }

  /// Get root node
  [[nodiscard]] NodeId
  root () const
  {
    return root_;
  }

  /// Get all nodes (for traversal)
  [[nodiscard]] const std::vector<NodeData> &
  nodes () const
  {
    return nodes_;
  }

private:
  std::vector<NodeData> nodes_; // Index 0 is unused (null node)
  NodeId root_;                 // Root of the tree
  bool dirty_ = true;           // Needs layout

  friend class LayoutEngine;
};

} // namespace nxb::ui
