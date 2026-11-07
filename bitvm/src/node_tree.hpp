#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "bitvm/graph.hpp"
#include "bitvm/proto/types.hpp"

namespace bitvm::detail
{

struct Node {
    proto::NodeId                  id       = 0;
    std::uint32_t                  def      = 0;
    bool                           running  = true;
    Node*                          parent   = nullptr;
    Node*                          previous = nullptr;
    Node*                          next     = nullptr;
    Node*                          first    = nullptr;
    Node*                          last     = nullptr;
    std::unique_ptr<GraphInstance> synth;
};

/// non-owning execution tree; only construction allocates storage.
/// node destruction belongs to the control thread, after the removal sink retires a node.
class NodeTree {
public:
    struct RemovalSink {
        void* context;
        void (*removed) (void* context, Node* node) noexcept;
    };

    explicit NodeTree (std::size_t capacity);
    NodeTree (const NodeTree&)            = delete;
    NodeTree& operator= (const NodeTree&) = delete;

    Node&         root () noexcept;
    const Node&   root () const noexcept;
    Node*         find (proto::NodeId id) const noexcept;
    std::uint32_t node_count () const noexcept;
    std::uint32_t group_count () const noexcept;

    proto::ErrorCode
    insert (Node* node, proto::AddAction add, proto::NodeId target, RemovalSink sink) noexcept;
    proto::ErrorCode move (Node* node, proto::AddAction add, proto::NodeId target) noexcept;
    void             erase_subtree (Node* node, RemovalSink sink) noexcept;
    void             clear (Node* group, RemovalSink sink) noexcept;

    static Node* after_subtree (Node* node) noexcept;

private:
    struct Slot {
        Node*         node  = nullptr;
        unsigned char state = 0;
    };

    bool             insert_table (Node* node) noexcept;
    void             erase_table (proto::NodeId id) noexcept;
    proto::ErrorCode placement (Node* target, proto::AddAction add) const noexcept;
    static void      detach (Node* node) noexcept;
    static void      attach (Node* node, proto::AddAction add, Node* target) noexcept;

    Node              root_;
    std::vector<Slot> slots_;
    std::uint32_t     node_count_  = 0;
    std::uint32_t     group_count_ = 1;
};

}  // namespace bitvm::detail
