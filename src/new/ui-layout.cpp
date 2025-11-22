#include "ui-layout.hpp"

#include <algorithm>
#include <numeric>

namespace nxb::ui
{

void
LayoutEngine::compute (Dom &dom, std::size_t container_width,
                       std::size_t container_height)
{
  // Layout from root
  Rect root_rect{ 0, 0, container_width, container_height };
  layout_element (dom, dom.root (), root_rect);
  dom.mark_clean ();
}

void
LayoutEngine::layout_element (Dom &dom, NodeId node_id, Rect container_rect)
{
  auto &node = dom.get_mut (node_id);
  auto *elem = std::get_if<Element> (&node.content);
  if (!elem)
    {
      // Text node - just set rect to container
      node.rect = container_rect;
      return;
    }

  // Set this node's rect
  node.rect = container_rect;

  if (elem->children.empty ())
    return;

  const auto &style = elem->style;

  // Measure all children
  struct ChildInfo
  {
    NodeId id;
    std::size_t intrinsic_width;
    std::size_t intrinsic_height;
    std::size_t grow_factor;
  };

  std::vector<ChildInfo> children;
  for (auto child_id : elem->children)
    {
      auto [w, h] = measure (dom, child_id, container_rect.w, container_rect.h);

      // Check child's size specs
      auto &child_node = dom.get (child_id);
      std::size_t grow = 0;
      if (auto *child_elem = std::get_if<Element> (&child_node.content))
        {
          const bool is_main_axis_row = (style.flex_dir == FlexDir::Row);
          const auto &child_size
              = is_main_axis_row ? child_elem->style.width : child_elem->style.height;

          if (child_size.is_grow)
            grow = child_size.value;

          // Override with fixed size if specified
          if (!is_main_axis_row && child_elem->style.width.value > 0
              && !child_elem->style.width.is_grow)
            w = child_elem->style.width.value;
          if (is_main_axis_row && child_elem->style.height.value > 0
              && !child_elem->style.height.is_grow)
            h = child_elem->style.height.value;
        }

      children.push_back ({ child_id, w, h, grow });
    }

  // Compute flex layout
  const bool is_row = (style.flex_dir == FlexDir::Row);

  // Sum up intrinsic sizes on main axis
  std::size_t total_intrinsic = 0;
  std::size_t total_grow = 0;
  for (const auto &child : children)
    {
      total_intrinsic
          += is_row ? child.intrinsic_width : child.intrinsic_height;
      total_grow += child.grow_factor;
    }

  // Available space on main axis
  const std::size_t main_size
      = is_row ? container_rect.w : container_rect.h;
  const std::size_t cross_size
      = is_row ? container_rect.h : container_rect.w;

  // Distribute extra space to growing children
  std::size_t extra_space
      = (main_size > total_intrinsic) ? (main_size - total_intrinsic) : 0;
  std::size_t space_per_grow = (total_grow > 0) ? (extra_space / total_grow) : 0;

  // Compute spacing for justify-content
  std::size_t spacing = 0;
  std::size_t initial_offset = 0;

  if (total_grow == 0 && extra_space > 0)
    {
      switch (style.justify)
        {
        case Justify::Start:
          break;
        case Justify::Center:
          initial_offset = extra_space / 2;
          break;
        case Justify::End:
          initial_offset = extra_space;
          break;
        case Justify::SpaceBetween:
          if (children.size () > 1)
            spacing = extra_space / (children.size () - 1);
          break;
        case Justify::SpaceAround:
          spacing = extra_space / children.size ();
          initial_offset = spacing / 2;
          break;
        }
    }

  // Position children
  std::size_t main_pos = initial_offset;

  for (const auto &child : children)
    {
      std::size_t child_main_size
          = is_row ? child.intrinsic_width : child.intrinsic_height;
      std::size_t child_cross_size
          = is_row ? child.intrinsic_height : child.intrinsic_width;

      // Add flex-grow space
      if (child.grow_factor > 0)
        child_main_size += space_per_grow * child.grow_factor;

      // Cross-axis alignment
      std::size_t cross_pos = 0;
      switch (style.align)
        {
        case Align::Start:
          cross_pos = 0;
          break;
        case Align::Center:
          cross_pos = (cross_size > child_cross_size)
                          ? (cross_size - child_cross_size) / 2
                          : 0;
          break;
        case Align::End:
          cross_pos = (cross_size > child_cross_size)
                          ? (cross_size - child_cross_size)
                          : 0;
          break;
        case Align::Stretch:
          child_cross_size = cross_size;
          break;
        }

      // Compute child rect
      Rect child_rect;
      if (is_row)
        {
          child_rect = { container_rect.x + main_pos,
                         container_rect.y + cross_pos, child_main_size,
                         child_cross_size };
        }
      else
        {
          child_rect = { container_rect.x + cross_pos,
                         container_rect.y + main_pos, child_cross_size,
                         child_main_size };
        }

      // Recursively layout child
      layout_element (dom, child.id, child_rect);

      main_pos += child_main_size + spacing;
    }
}

std::pair<std::size_t, std::size_t>
LayoutEngine::measure (Dom &dom, NodeId node_id, std::size_t max_width,
                       std::size_t max_height)
{
  const auto &node = dom.get (node_id);

  if (auto *text = std::get_if<Text> (&node.content))
    {
      // Text: width = length, height = line count
      auto width = std::min (measure_text_width (text->content), max_width);
      auto height = std::min (count_lines (text->content), max_height);
      return { width, height };
    }

  if (auto *elem = std::get_if<Element> (&node.content))
    {
      // Element: measure children
      if (elem->children.empty ())
        return { 0, 0 };

      const bool is_row = (elem->style.flex_dir == FlexDir::Row);

      std::size_t total_main = 0;
      std::size_t max_cross = 0;

      for (auto child_id : elem->children)
        {
          auto [w, h] = measure (dom, child_id, max_width, max_height);

          if (is_row)
            {
              total_main += w;
              max_cross = std::max (max_cross, h);
            }
          else
            {
              total_main += h;
              max_cross = std::max (max_cross, w);
            }
        }

      return is_row ? std::pair{ total_main, max_cross }
                    : std::pair{ max_cross, total_main };
    }

  return { 0, 0 };
}

std::size_t
LayoutEngine::count_lines (const std::string &text)
{
  return std::count (text.begin (), text.end (), '\n') + 1;
}

std::size_t
LayoutEngine::measure_text_width (const std::string &text)
{
  // Naive: just find longest line
  std::size_t max_width = 0;
  std::size_t current_width = 0;

  for (char c : text)
    {
      if (c == '\n')
        {
          max_width = std::max (max_width, current_width);
          current_width = 0;
        }
      else
        {
          current_width++;
        }
    }

  return std::max (max_width, current_width);
}

} // namespace nxb::ui

