#pragma once

#include "ui-dom.hpp"

namespace nxb::ui
{

/// Simple flexbox layout engine
/// Computes rect positions for all nodes in the DOM
class LayoutEngine
{
public:
  /// Compute layout for entire DOM
  /// Sets rect on all nodes
  void compute (Dom &dom, std::size_t container_width,
                std::size_t container_height);

private:
  /// Layout a single element and its children
  void layout_element (Dom &dom, NodeId node_id, Rect container_rect);

  /// Measure intrinsic size of a node
  std::pair<std::size_t, std::size_t> measure (Dom &dom, NodeId node_id,
                                               std::size_t max_width,
                                               std::size_t max_height);

  /// Count lines in text
  std::size_t count_lines (const std::string &text);

  /// Measure text width (naive: just length, no Unicode width yet)
  std::size_t measure_text_width (const std::string &text);
};

} // namespace nxb::ui

