#pragma once

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nixb
{

/**
 * Bidirectional dependency graph tracker.
 *
 * Maintains both forward (children) and backward (dependents) edges,
 * automatically keeping them in sync. Also tracks root nodes (nodes with no
 * dependents).
 *
 * Example: If A depends on B (B is a child of A):
 *   - children_[A] contains B
 *   - dependents_[B] contains A
 *   - roots_ won't contain A (since A has dependencies)
 */
template <typename NodeId> class DependencyGraph
{
public:
  DependencyGraph () = default;

  /**
   * Add a dependency edge: parent depends on child.
   * Automatically updates both forward and backward maps.
   */
  void
  add_edge (const NodeId &parent, const NodeId &child)
  {
    children_[parent].push_back (child);
    dependents_[child].push_back (parent);

    // Parent is no longer a root (it has dependencies)
    remove_from_roots (parent);

    // Child might become a root if it has no dependencies
    // (we don't auto-add to roots; use update_roots() for that)
  }

  /**
   * Remove a specific dependency edge.
   */
  void
  remove_edge (const NodeId &parent, const NodeId &child)
  {
    auto &children = children_[parent];
    children.erase (std::remove (children.begin (), children.end (), child),
                    children.end ());

    auto &deps = dependents_[child];
    deps.erase (std::remove (deps.begin (), deps.end (), parent), deps.end ());

    // If parent now has no children, clean up empty vector
    if (children.empty ())
      children_.erase (parent);
    if (deps.empty ())
      dependents_.erase (child);
  }

  /**
   * Remove a node entirely from the graph.
   * Cleans up all edges involving this node.
   */
  void
  remove_node (const NodeId &id)
  {
    // Remove from children map
    children_.erase (id);

    // Remove from dependents map
    dependents_.erase (id);

    // Remove from any parent's children list
    for (auto &[parent_id, children] : children_)
      {
        children.erase (std::remove (children.begin (), children.end (), id),
                        children.end ());
      }

    // Remove from any child's dependents list
    for (auto &[child_id, deps] : dependents_)
      {
        deps.erase (std::remove (deps.begin (), deps.end (), id), deps.end ());
      }

    // Remove from roots
    remove_from_roots (id);
  }

  /**
   * Get children (dependencies) of a node.
   */
  const std::vector<NodeId> &
  get_children (const NodeId &id) const
  {
    static const std::vector<NodeId> empty;
    auto it = children_.find (id);
    return it != children_.end () ? it->second : empty;
  }

  /**
   * Get dependents (reverse dependencies) of a node.
   */
  const std::vector<NodeId> &
  get_dependents (const NodeId &id) const
  {
    static const std::vector<NodeId> empty;
    auto it = dependents_.find (id);
    return it != dependents_.end () ? it->second : empty;
  }

  /**
   * Check if a node has any children (dependencies).
   */
  bool
  has_children (const NodeId &id) const
  {
    auto it = children_.find (id);
    return it != children_.end () && !it->second.empty ();
  }

  /**
   * Check if a node has any dependents.
   */
  bool
  has_dependents (const NodeId &id) const
  {
    auto it = dependents_.find (id);
    return it != dependents_.end () && !it->second.empty ();
  }

  /**
   * Rebuild root nodes list.
   * Roots are nodes that exist but have no dependents (nothing depends on
   * them).
   */
  template <typename NodeContainer>
  void
  update_roots (const NodeContainer &all_nodes)
  {
    roots_.clear ();
    std::unordered_set<NodeId> is_dependency;

    // Collect all nodes that are dependencies of something
    for (const auto &[parent_id, child_ids] : children_)
      {
        is_dependency.insert (child_ids.begin (), child_ids.end ());
      }

    // Roots are nodes that aren't dependencies of anything
    for (const auto &node : all_nodes)
      {
        if (!is_dependency.contains (node))
          {
            roots_.push_back (node);
          }
      }
  }

  /**
   * Get current root nodes.
   */
  const std::vector<NodeId> &
  get_roots () const
  {
    return roots_;
  }

  /**
   * Add a node to roots explicitly.
   */
  void
  add_root (const NodeId &id)
  {
    if (std::find (roots_.begin (), roots_.end (), id) == roots_.end ())
      {
        roots_.push_back (id);
      }
  }

  /**
   * Remove a node from roots.
   */
  void
  remove_from_roots (const NodeId &id)
  {
    roots_.erase (std::remove (roots_.begin (), roots_.end (), id),
                  roots_.end ());
  }

  /**
   * Clear all data.
   */
  void
  clear ()
  {
    children_.clear ();
    dependents_.clear ();
    roots_.clear ();
  }

  /**
   * Get all children map (for iteration).
   */
  const std::unordered_map<NodeId, std::vector<NodeId>> &
  children () const
  {
    return children_;
  }

  /**
   * Get all dependents map (for iteration).
   */
  const std::unordered_map<NodeId, std::vector<NodeId>> &
  dependents () const
  {
    return dependents_;
  }

  /**
   * Mutable access to children for sorting/deduplication.
   */
  std::unordered_map<NodeId, std::vector<NodeId>> &
  children_mut ()
  {
    return children_;
  }

  /**
   * Mutable access to roots for sorting.
   */
  std::vector<NodeId> &
  roots_mut ()
  {
    return roots_;
  }

private:
  std::unordered_map<NodeId, std::vector<NodeId>>
      children_; // parent -> [children]
  std::unordered_map<NodeId, std::vector<NodeId>>
      dependents_;            // child -> [dependents/parents]
  std::vector<NodeId> roots_; // Nodes with no dependents
};

/**
 * Bidirectional ID mapping tracker.
 *
 * Maintains 1:1 mappings between two ID spaces and automatically keeps them in
 * sync.
 */
template <typename IdA, typename IdB> class BidirectionalMap
{
public:
  BidirectionalMap () = default;

  /**
   * Add or update a mapping.
   */
  void
  insert (const IdA &a, const IdB &b)
  {
    a_to_b_[a] = b;
    b_to_a_[b] = a;
  }

  /**
   * Remove mapping by first ID.
   */
  void
  erase_by_a (const IdA &a)
  {
    auto it = a_to_b_.find (a);
    if (it != a_to_b_.end ())
      {
        b_to_a_.erase (it->second);
        a_to_b_.erase (it);
      }
  }

  /**
   * Remove mapping by second ID.
   */
  void
  erase_by_b (const IdB &b)
  {
    auto it = b_to_a_.find (b);
    if (it != b_to_a_.end ())
      {
        a_to_b_.erase (it->second);
        b_to_a_.erase (it);
      }
  }

  /**
   * Lookup B by A.
   */
  std::optional<IdB>
  get_b (const IdA &a) const
  {
    auto it = a_to_b_.find (a);
    return it != a_to_b_.end () ? std::optional{ it->second } : std::nullopt;
  }

  /**
   * Lookup A by B.
   */
  std::optional<IdA>
  get_a (const IdB &b) const
  {
    auto it = b_to_a_.find (b);
    return it != b_to_a_.end () ? std::optional{ it->second } : std::nullopt;
  }

  /**
   * Check if A exists.
   */
  bool
  contains_a (const IdA &a) const
  {
    return a_to_b_.contains (a);
  }

  /**
   * Check if B exists.
   */
  bool
  contains_b (const IdB &b) const
  {
    return b_to_a_.contains (b);
  }

  /**
   * Clear all mappings.
   */
  void
  clear ()
  {
    a_to_b_.clear ();
    b_to_a_.clear ();
  }

private:
  std::unordered_map<IdA, IdB> a_to_b_;
  std::unordered_map<IdB, IdA> b_to_a_;
};

} // namespace nixb
