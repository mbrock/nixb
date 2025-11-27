#pragma once

// Derivation dependency graph construction and traversal
//
// Example usage:
//
//   nxb::NixContext ctx;
//   auto roots = nxb::resolve_installable(ctx, "nixpkgs#hello");
//   auto graph = nxb::drv::build_graph(ctx, roots);
//
//   fmt::print("Total derivations: {}\n", graph.nodes.size());
//   fmt::print("Build order:\n");
//   for (auto& node : graph.topological_order()) {
//       fmt::print("  {} ({} inputs)\n", node->name, node->inputs.size());
//   }

#include <algorithm>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include <nix/store/derivations.hh>
#include <nix/store/store-api.hh>
#include <nix/util/types.hh>

#include "nix-api.hpp"

namespace nxb::drv {

// ============================================================================
// Graph node representing a single derivation
// ============================================================================

struct Node
{
    // Identity
    nix::StorePath drv_path;
    std::string name;
    std::string system;

    // Relationships (indices into Graph::nodes)
    std::vector<std::size_t> inputs;  // Derivations this depends on
    std::vector<std::size_t> outputs; // Derivations that depend on this

    // Derivation details
    std::string builder;
    nix::StringPairs env;
    nix::Strings args;
    std::vector<std::string> output_names;

    // Graph metrics (computed after graph is built)
    std::size_t depth = 0;        // Distance from root (0 = root)
    std::size_t height = 0;       // Distance to furthest leaf
    std::size_t subtree_size = 0; // Number of transitive dependencies

    Node(nix::StorePath path)
        : drv_path(std::move(path))
    {
    }
};

// ============================================================================
// The complete derivation graph
// ============================================================================

struct Graph
{
    // All nodes, in insertion order
    std::vector<std::unique_ptr<Node>> nodes;

    // Index from store path to node index
    std::map<nix::StorePath, std::size_t> path_to_index;

    // Root nodes (the ones we started from)
    std::vector<std::size_t> roots;

    // Leaf nodes (no inputs, e.g. fetchurl, source paths)
    std::vector<std::size_t> leaves;

    // Statistics
    std::size_t total_derivations() const { return nodes.size(); }

    std::size_t max_depth() const
    {
        std::size_t max = 0;
        for (const auto& node : nodes)
            max = std::max(max, node->depth);
        return max;
    }

    // Get node by index
    Node* get(std::size_t idx)
    {
        return idx < nodes.size() ? nodes[idx].get() : nullptr;
    }

    const Node* get(std::size_t idx) const
    {
        return idx < nodes.size() ? nodes[idx].get() : nullptr;
    }

    // Get node by path
    Node* get(const nix::StorePath& path)
    {
        auto it = path_to_index.find(path);
        return it != path_to_index.end() ? get(it->second) : nullptr;
    }

    const Node* get(const nix::StorePath& path) const
    {
        auto it = path_to_index.find(path);
        return it != path_to_index.end() ? get(it->second) : nullptr;
    }

    // Topological sort (dependencies before dependents)
    // This gives a valid build order
    std::vector<Node*> topological_order()
    {
        std::vector<Node*> result;
        result.reserve(nodes.size());

        // Kahn's algorithm
        std::vector<std::size_t> in_degree(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i)
            in_degree[i] = nodes[i]->inputs.size();

        std::queue<std::size_t> ready;
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (in_degree[i] == 0)
                ready.push(i);

        while (!ready.empty()) {
            auto idx = ready.front();
            ready.pop();
            result.push_back(nodes[idx].get());

            for (auto out_idx : nodes[idx]->outputs) {
                if (--in_degree[out_idx] == 0)
                    ready.push(out_idx);
            }
        }

        return result;
    }

    // Reverse topological sort (dependents before dependencies)
    // Useful for propagating information from roots to leaves
    std::vector<Node*> reverse_topological_order()
    {
        auto order = topological_order();
        std::reverse(order.begin(), order.end());
        return order;
    }

    // Get all nodes at a given depth
    std::vector<Node*> at_depth(std::size_t depth)
    {
        std::vector<Node*> result;
        for (auto& node : nodes)
            if (node->depth == depth)
                result.push_back(node.get());
        return result;
    }

    // Find path from root to a node (for debugging/display)
    std::vector<Node*> path_to(const Node* target) const
    {
        if (!target)
            return {};

        // BFS from roots
        std::map<std::size_t, std::size_t> parent;
        std::queue<std::size_t> q;

        for (auto root_idx : roots) {
            q.push(root_idx);
            parent[root_idx] = root_idx; // sentinel
        }

        auto target_idx = path_to_index.at(target->drv_path);

        while (!q.empty()) {
            auto idx = q.front();
            q.pop();

            if (idx == target_idx) {
                // Reconstruct path
                std::vector<Node*> path;
                auto curr = idx;
                while (parent[curr] != curr) {
                    path.push_back(nodes[curr].get());
                    curr = parent[curr];
                }
                path.push_back(nodes[curr].get());
                std::reverse(path.begin(), path.end());
                return path;
            }

            for (auto input_idx : nodes[idx]->inputs) {
                if (parent.find(input_idx) == parent.end()) {
                    parent[input_idx] = idx;
                    q.push(input_idx);
                }
            }
        }

        return {};
    }
};

// ============================================================================
// Graph construction
// ============================================================================

/// Build complete dependency graph starting from root derivations.
/// Recursively discovers all input derivations.
inline Graph
build_graph(NixContext& ctx, const std::vector<nix::StorePath>& root_paths)
{
    Graph graph;
    auto& store = *ctx.store();

    // BFS to discover all derivations
    // Use string representation for queue since StorePath isn't copyable
    std::queue<std::string> to_visit;
    std::set<std::string> seen;

    for (const auto& root : root_paths) {
        auto path_str = store.printStorePath(root);
        to_visit.push(path_str);
        seen.insert(path_str);
    }

    // First pass: discover all nodes
    while (!to_visit.empty()) {
        auto path_str = to_visit.front();
        to_visit.pop();

        // Parse the store path
        auto path = store.parseStorePath(path_str);

        // Create node
        auto idx = graph.nodes.size();
        auto node = std::make_unique<Node>(path);

        // Read derivation
        try {
            auto drv = store.readDerivation(path);
            node->name = drv.name;
            node->system = drv.platform;
            node->builder = drv.builder;
            node->args = drv.args;
            node->env = drv.env;

            for (const auto& [name, output] : drv.outputs)
                node->output_names.push_back(name);

            // Queue inputs
            for (const auto& [input_path, _] : drv.inputDrvs.map) {
                auto input_str = store.printStorePath(input_path);
                if (seen.insert(input_str).second)
                    to_visit.push(input_str);
            }
        } catch (const std::exception& e) {
            // Derivation might not be readable (e.g., not in store yet)
            node->name = std::string(path.name());
        }

        graph.path_to_index.emplace(std::move(path), idx);
        graph.nodes.push_back(std::move(node));
    }

    // Second pass: wire up relationships
    for (std::size_t idx = 0; idx < graph.nodes.size(); ++idx) {
        auto& node = graph.nodes[idx];

        try {
            auto drv = ctx.store()->readDerivation(node->drv_path);

            for (const auto& [input_path, _] : drv.inputDrvs.map) {
                auto it = graph.path_to_index.find(input_path);
                if (it != graph.path_to_index.end()) {
                    node->inputs.push_back(it->second);
                    graph.nodes[it->second]->outputs.push_back(idx);
                }
            }
        } catch (...) {
            // Skip if we can't read the derivation
        }
    }

    // Identify roots and leaves
    for (std::size_t idx = 0; idx < graph.nodes.size(); ++idx) {
        if (graph.nodes[idx]->outputs.empty())
            graph.roots.push_back(idx);
        if (graph.nodes[idx]->inputs.empty())
            graph.leaves.push_back(idx);
    }

    // Compute depths (BFS from roots)
    {
        std::queue<std::size_t> q;
        for (auto root_idx : graph.roots) {
            graph.nodes[root_idx]->depth = 0;
            q.push(root_idx);
        }

        while (!q.empty()) {
            auto idx = q.front();
            q.pop();
            auto depth = graph.nodes[idx]->depth;

            for (auto input_idx : graph.nodes[idx]->inputs) {
                auto& input_node = graph.nodes[input_idx];
                if (input_node->depth < depth + 1) {
                    input_node->depth = depth + 1;
                    q.push(input_idx);
                }
            }
        }
    }

    // Compute heights and subtree sizes (reverse topological order)
    {
        auto order = graph.topological_order();
        for (auto* node : order) {
            node->subtree_size = 1;
            for (auto input_idx : node->inputs) {
                auto* input = graph.get(input_idx);
                node->height = std::max(node->height, input->height + 1);
                node->subtree_size += input->subtree_size;
            }
        }
    }

    return graph;
}

// ============================================================================
// Graph queries
// ============================================================================

/// Find derivations matching a predicate
template <typename Pred>
std::vector<Node*> find_nodes(Graph& graph, Pred&& pred)
{
    std::vector<Node*> result;
    for (auto& node : graph.nodes)
        if (pred(*node))
            result.push_back(node.get());
    return result;
}

/// Find derivations by name substring
inline std::vector<Node*>
find_by_name(Graph& graph, std::string_view substr)
{
    return find_nodes(graph, [substr](const Node& n) {
        return n.name.find(substr) != std::string::npos;
    });
}

/// Get critical path (longest dependency chain)
inline std::vector<Node*> critical_path(Graph& graph)
{
    if (graph.nodes.empty())
        return {};

    // Find node with maximum height among roots
    Node* start = nullptr;
    for (auto root_idx : graph.roots) {
        auto* node = graph.get(root_idx);
        if (!start || node->height > start->height)
            start = node;
    }

    // Follow the path of maximum heights
    std::vector<Node*> path;
    auto* curr = start;

    while (curr) {
        path.push_back(curr);

        Node* next = nullptr;
        for (auto input_idx : curr->inputs) {
            auto* input = graph.get(input_idx);
            if (!next || input->height > next->height)
                next = input;
        }
        curr = next;
    }

    return path;
}

// ============================================================================
// Graph statistics
// ============================================================================

struct GraphStats
{
    std::size_t total_nodes;
    std::size_t root_count;
    std::size_t leaf_count;
    std::size_t max_depth;
    std::size_t max_height;
    std::size_t max_fan_in;  // Most inputs to a single node
    std::size_t max_fan_out; // Most nodes depending on a single node
    double avg_inputs;
    std::vector<std::size_t> nodes_per_depth;
};

inline GraphStats compute_stats(const Graph& graph)
{
    GraphStats stats{};
    stats.total_nodes = graph.nodes.size();
    stats.root_count = graph.roots.size();
    stats.leaf_count = graph.leaves.size();

    std::size_t total_inputs = 0;

    for (const auto& node : graph.nodes) {
        stats.max_depth = std::max(stats.max_depth, node->depth);
        stats.max_height = std::max(stats.max_height, node->height);
        stats.max_fan_in = std::max(stats.max_fan_in, node->inputs.size());
        stats.max_fan_out = std::max(stats.max_fan_out, node->outputs.size());
        total_inputs += node->inputs.size();
    }

    stats.avg_inputs = graph.nodes.empty()
                           ? 0.0
                           : static_cast<double>(total_inputs) / graph.nodes.size();

    // Count nodes per depth level
    stats.nodes_per_depth.resize(stats.max_depth + 1, 0);
    for (const auto& node : graph.nodes)
        stats.nodes_per_depth[node->depth]++;

    return stats;
}

} // namespace nxb::drv
