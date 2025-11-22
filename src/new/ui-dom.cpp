#include "ui-dom.hpp"

#include <stdexcept>

namespace nxb::ui
{

Dom::Dom ()
{
  // Reserve index 0 as null
  nodes_.push_back (NodeData{});

  // Create root element
  root_ = create_element (Style::defaults ());
}

NodeId
Dom::create_element (Style style)
{
  NodeId id{ nodes_.size () };
  nodes_.push_back (NodeData{
      .parent = NodeId::null (),
      .content = Element{ .style = style, .children = {} },
      .rect = {},
  });
  dirty_ = true;
  return id;
}

NodeId
Dom::create_text (std::string content, fmt::color color)
{
  NodeId id{ nodes_.size () };
  nodes_.push_back (NodeData{
      .parent = NodeId::null (),
      .content = Text{ .content = std::move (content), .color = color },
      .rect = {},
  });
  dirty_ = true;
  return id;
}

void
Dom::append_child (NodeId parent, NodeId child)
{
  if (!parent.is_valid () || !child.is_valid ())
    return;

  auto &parent_data = nodes_[parent.value];
  auto &child_data = nodes_[child.value];

  // Only elements can have children
  if (auto *elem = std::get_if<Element> (&parent_data.content))
    {
      elem->children.push_back (child);
      child_data.parent = parent;
      dirty_ = true;
    }
}

void
Dom::update_text (NodeId node, std::string new_text,
                  std::optional<fmt::color> color)
{
  if (!node.is_valid ())
    return;

  auto &data = nodes_[node.value];
  if (auto *text = std::get_if<Text> (&data.content))
    {
      text->content = std::move (new_text);
      if (color)
        text->color = *color;
      dirty_ = true;
    }
}

void
Dom::update_style (NodeId node, Style new_style)
{
  if (!node.is_valid ())
    return;

  auto &data = nodes_[node.value];
  if (auto *elem = std::get_if<Element> (&data.content))
    {
      elem->style = new_style;
      dirty_ = true;
    }
}

const NodeData &
Dom::get (NodeId node) const
{
  if (!node.is_valid () || node.value >= nodes_.size ())
    throw std::out_of_range ("Invalid NodeId");
  return nodes_[node.value];
}

NodeData &
Dom::get_mut (NodeId node)
{
  if (!node.is_valid () || node.value >= nodes_.size ())
    throw std::out_of_range ("Invalid NodeId");
  return nodes_[node.value];
}

} // namespace nxb::ui

