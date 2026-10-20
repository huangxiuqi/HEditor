#include "core/rev/flow.h"

namespace heditor::rev {

namespace {

/**
 * 文本节点占几个流位置。载荷不是文本那一支时按 0 计 —— 种类与载荷对不上是构造方的错，
 * 这里退化成"不占位"而不是崩溃：索引是派生量，崩在这里看不出是谁造错了节点。
 */
int textLenOf(const Node& n) {
    const auto* run = std::get_if<RunPayload>(&n.payload);
    return run == nullptr ? 0 : static_cast<int>(run->text.size());
}

/**
 * 节点号的散列。节点号按 1、2、3 递增分配，低位几乎连续，直接取模会让相邻的号挤在
 * 同一段槽里；先混一遍高位再取模，探测长度才与"随机"一致。
 */
std::size_t hashId(NodeId id) {
    std::uint64_t x = id;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return static_cast<std::size_t>(x);
}

}  // namespace

FlowIndex::FlowIndex(const Doc& doc, FlowVisibility vis) : vis_(vis) {
    if (!doc.root) return;
    root_ = doc.root->id;
    int cursor = 0;
    measure(doc.root, cursor, true);
    size_ = cursor;
    buildLookup();
}

void FlowIndex::measure(const Ref& n, int& cursor, bool is_root) {
    if (vis_ == FlowVisibility::Display && n->hidden) return;

    const int start = cursor;
    const int idx = static_cast<int>(nodes_.size());
    nodes_.push_back(n.get());
    first_.push_back(start);
    span_.push_back(0);

    if (isContainer(n->kind)) {
        if (!is_root) ++cursor;
        for (const Ref& child : n->children) measure(child, cursor, false);
        if (!is_root) ++cursor;
    } else if (isText(n->kind)) {
        cursor += textLenOf(*n);
    } else {
        ++cursor;
    }

    span_[static_cast<std::size_t>(idx)] = cursor - start;
}

void FlowIndex::buildLookup() {
    std::size_t cap = 16;
    while (cap < nodes_.size() * 2) cap <<= 1;
    lookup_.assign(cap, Entry{});
    mask_ = cap - 1;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const NodeId id = nodes_[i]->id;
        std::size_t slot = hashId(id) & mask_;
        while (lookup_[slot].id != kInvalidNodeId) slot = (slot + 1) & mask_;
        lookup_[slot].id = id;
        lookup_[slot].idx = static_cast<int>(i);
    }
}

int FlowIndex::indexOf(NodeId id) const {
    if (id == kInvalidNodeId || lookup_.empty()) return -1;
    std::size_t slot = hashId(id) & mask_;
    while (lookup_[slot].id != kInvalidNodeId) {
        if (lookup_[slot].id == id) return lookup_[slot].idx;
        slot = (slot + 1) & mask_;
    }
    return -1;
}

std::size_t FlowIndex::allocatedBytes() const {
    return nodes_.capacity() * sizeof(const Node*) + first_.capacity() * sizeof(int) +
           span_.capacity() * sizeof(int) + lookup_.capacity() * sizeof(Entry);
}

int FlowIndex::contentStartOf(NodeId id) const {
    const int idx = indexOf(id);
    if (idx < 0) return -1;
    return firstAt(idx) + (id == root_ ? 0 : 1);
}

bool FlowIndex::intervalOf(NodeId id, int* first, int* end) const {
    const int idx = indexOf(id);
    if (idx < 0) return false;
    if (first != nullptr) *first = firstAt(idx);
    if (end != nullptr) *end = endAt(idx);
    return true;
}

FlowPos FlowIndex::flowAt(const Pos& pos) const {
    const int idx = indexOf(pos.container);
    if (idx < 0 || pos.offset < 0) return -1;
    const Node* n = nodeAt(idx);

    if (isContainer(n->kind)) {
        const int slots = static_cast<int>(n->children.size());
        if (pos.offset > slots) return -1;
        if (pos.offset == 0) return contentStartOf(pos.container);
        const Ref& prev = n->children[static_cast<std::size_t>(pos.offset) - 1];
        const int prev_idx = indexOf(prev->id);
        if (prev_idx < 0) return -1;
        return endAt(prev_idx);
    }
    if (isText(n->kind)) {
        if (pos.offset > textLenOf(*n)) return -1;
        return firstAt(idx) + pos.offset;
    }
    return pos.offset == 0 ? firstAt(idx) : -1;
}

Pos FlowIndex::posAt(FlowPos flow) const {
    if (flow < 0) flow = 0;
    if (flow > size_) flow = size_;

    NodeId cur = root_;
    int cur_idx = indexOf(cur);
    while (cur_idx >= 0) {
        const Node* n = nodeAt(cur_idx);
        if (!isContainer(n->kind)) break;
        NodeId next = kInvalidNodeId;
        for (const Ref& child : n->children) {
            const int ci = indexOf(child->id);
            if (ci < 0) continue;
            const int cfirst = firstAt(ci);
            const int cend = endAt(ci);
            const bool inner = (flow > cfirst && flow < cend);
            const bool leaf_head = (!isContainer(child->kind) && flow == cfirst && cfirst < cend);
            if (inner || leaf_head) {
                next = child->id;
                break;
            }
        }
        if (next == kInvalidNodeId) break;
        cur = next;
        cur_idx = indexOf(cur);
    }

    if (cur_idx < 0) return Pos{root_, 0};
    const Node* n = nodeAt(cur_idx);
    if (!isContainer(n->kind)) return Pos{cur, flow - firstAt(cur_idx)};

    if (flow == contentStartOf(cur)) return Pos{cur, 0};
    int last_slot = 0;
    for (std::size_t i = 0; i < n->children.size(); ++i) {
        const int ci = indexOf(n->children[i]->id);
        if (ci < 0) continue;
        last_slot = static_cast<int>(i) + 1;
        if (flow == endAt(ci)) return Pos{cur, last_slot};
    }
    return Pos{cur, last_slot};
}

}  // namespace heditor::rev
