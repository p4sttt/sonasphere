#include "node_tree.hpp"

namespace bitvm::detail
{

NodeTree::NodeTree (std::size_t capacity) : slots_ (capacity * 2 + 1) {}

Node&
NodeTree::root () noexcept {
    return root_;
}

const Node&
NodeTree::root () const noexcept {
    return root_;
}

Node*
NodeTree::find (proto::NodeId id) const noexcept {
    if (id == 0) {
        return const_cast<Node*> (&root_);
    }
    auto pos = static_cast<std::size_t> (id) % slots_.size();
    for (std::size_t count = 0; count < slots_.size(); ++count) {
        const auto& slot = slots_[pos];
        if (slot.state == 0) {
            return nullptr;
        }
        if (slot.state == 1 && slot.node->id == id) {
            return slot.node;
        }
        pos = (pos + 1) % slots_.size();
    }
    return nullptr;
}

std::uint32_t
NodeTree::node_count () const noexcept {
    return node_count_;
}

std::uint32_t
NodeTree::group_count () const noexcept {
    return group_count_;
}

bool
NodeTree::insert_table (Node* node) noexcept {
    auto pos = static_cast<std::size_t> (node->id) % slots_.size();
    for (std::size_t count = 0; count < slots_.size(); ++count) {
        auto& slot = slots_[pos];
        if (slot.state != 1) {
            slot = { node, 1 };
            return true;
        }
        pos = (pos + 1) % slots_.size();
    }
    return false;
}

void
NodeTree::erase_table (proto::NodeId id) noexcept {
    auto pos = static_cast<std::size_t> (id) % slots_.size();
    for (std::size_t count = 0; count < slots_.size(); ++count) {
        auto& slot = slots_[pos];
        if (slot.state == 0) {
            return;
        }
        if (slot.state == 1 && slot.node->id == id) {
            // tombstones preserve probe chains after deletion.
            slot = { nullptr, 2 };
            return;
        }
        pos = (pos + 1) % slots_.size();
    }
}

void
NodeTree::detach (Node* node) noexcept {
    if (node->previous) {
        node->previous->next = node->next;
    } else {
        node->parent->first = node->next;
    }
    if (node->next) {
        node->next->previous = node->previous;
    } else {
        node->parent->last = node->previous;
    }
    node->parent = node->previous = node->next = nullptr;
}

void
NodeTree::attach (Node* node, proto::AddAction add, Node* target) noexcept {
    if (add == proto::AddAction::head || add == proto::AddAction::tail) {
        node->parent = target;
        if (add == proto::AddAction::head) {
            node->next = target->first;
            if (target->first) {
                target->first->previous = node;
            } else {
                target->last = node;
            }
            target->first = node;
        } else {
            node->previous = target->last;
            if (target->last) {
                target->last->next = node;
            } else {
                target->first = node;
            }
            target->last = node;
        }
    } else {
        node->parent = target->parent;
        if (add == proto::AddAction::before || add == proto::AddAction::replace) {
            node->previous = target->previous;
            node->next     = target;
            if (target->previous) {
                target->previous->next = node;
            } else {
                node->parent->first = node;
            }
            target->previous = node;
        } else {
            node->previous = target;
            node->next     = target->next;
            if (target->next) {
                target->next->previous = node;
            } else {
                node->parent->last = node;
            }
            target->next = node;
        }
    }
}

Node*
NodeTree::after_subtree (Node* node) noexcept {
    while (node && !node->next) {
        node = node->parent;
    }
    return node ? node->next : nullptr;
}

proto::ErrorCode
NodeTree::placement (Node* target, proto::AddAction add) const noexcept {
    if (!target) {
        return proto::ErrorCode::not_found;
    }
    if (add == proto::AddAction::head || add == proto::AddAction::tail) {
        return target->synth ? proto::ErrorCode::invalid_arg : proto::ErrorCode::ok;
    }
    return target == &root_ ? proto::ErrorCode::invalid_arg : proto::ErrorCode::ok;
}

proto::ErrorCode
NodeTree::insert (
    Node*            node,
    proto::AddAction add,
    proto::NodeId    target_id,
    RemovalSink      sink
) noexcept {
    if (find (node->id)) {
        return proto::ErrorCode::exists;
    }
    auto* target = find (target_id);
    if (auto error = placement (target, add); error != proto::ErrorCode::ok) {
        return error;
    }
    if (!insert_table (node)) {
        return proto::ErrorCode::limit;
    }
    attach (node, add, target);
    if (add == proto::AddAction::replace) {
        erase_subtree (target, sink);
    }
    if (node->synth) {
        ++node_count_;
    } else {
        ++group_count_;
    }
    return proto::ErrorCode::ok;
}

proto::ErrorCode
NodeTree::move (Node* node, proto::AddAction add, proto::NodeId target_id) noexcept {
    if (node == &root_) {
        return proto::ErrorCode::invalid_arg;
    }
    auto* target = find (target_id);
    if (auto error = placement (target, add); error != proto::ErrorCode::ok) {
        return error;
    }
    for (auto* parent = target; parent; parent = parent->parent) {
        if (parent == node) {
            return proto::ErrorCode::invalid_arg;
        }
    }
    detach (node);
    attach (node, add, target);
    return proto::ErrorCode::ok;
}

void
NodeTree::erase_subtree (Node* node, RemovalSink sink) noexcept {
    // postorder without recursion; work is bounded by the allocated node count.
    Node* current = node;
    while (current) {
        if (current->first) {
            current = current->first;
            continue;
        }
        auto*      parent = current->parent;
        const bool last   = current == node;
        erase_table (current->id);
        if (current->synth) {
            --node_count_;
        } else {
            --group_count_;
        }
        detach (current);
        // the sink may immediately destroy the detached node.
        sink.removed (sink.context, current);
        current = last ? nullptr : parent;
    }
}

void
NodeTree::clear (Node* group, RemovalSink sink) noexcept {
    while (group->first) {
        erase_subtree (group->first, sink);
    }
}

}  // namespace bitvm::detail
