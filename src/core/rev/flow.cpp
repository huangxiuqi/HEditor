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

}  // namespace

FlowIndex::FlowIndex(const Doc& doc, FlowVisibility vis) : vis_(vis) {
    const size_t slots = static_cast<size_t>(doc.next_id) + 1;
    by_id_.assign(slots, nullptr);
    first_.assign(slots, -1);
    span_.assign(slots, 0);
    if (!doc.root) {
        size_ = 0;
        return;
    }
    root_ = doc.root->id;
    int cursor = 0;
    measure(doc.root, cursor, true);
    size_ = cursor;
}

void FlowIndex::measure(const Ref& n, int& cursor, bool is_root) {
    if (vis_ == FlowVisibility::Display && n->hidden) return;

    const int start = cursor;
    const size_t slot = static_cast<size_t>(n->id);
    first_[slot] = start;
    by_id_[slot] = n.get();

    if (isContainer(n->kind)) {
        if (!is_root) ++cursor;
        for (const Ref& child : n->children) measure(child, cursor, false);
        if (!is_root) ++cursor;
    } else if (isText(n->kind)) {
        cursor += textLenOf(*n);
    } else {
        ++cursor;
    }

    span_[slot] = cursor - start;
}

int FlowIndex::contentStartOf(NodeId id) const {
    const bool is_root = (id == root_);
    return firstOf(id) + (is_root ? 0 : 1);
}

bool FlowIndex::intervalOf(NodeId id, int* first, int* end) const {
    if (!inFlow(id)) return false;
    const int start = firstOf(id);
    if (first != nullptr) *first = start;
    if (end != nullptr) *end = start + spanOf(id);
    return true;
}

FlowPos FlowIndex::flowAt(const Pos& pos) const {
    const Node* n = find(pos.container);
    if (n == nullptr || pos.offset < 0) return -1;
    if (!inFlow(pos.container)) return -1;

    if (isContainer(n->kind)) {
        const int slots = static_cast<int>(n->children.size());
        if (pos.offset > slots) return -1;
        if (pos.offset == 0) return contentStartOf(pos.container);
        const Ref& prev = n->children[static_cast<size_t>(pos.offset) - 1];
        if (!inFlow(prev->id)) return -1;
        return endOf(prev->id);
    }
    if (isText(n->kind)) {
        if (pos.offset > textLenOf(*n)) return -1;
        return firstOf(pos.container) + pos.offset;
    }
    return pos.offset == 0 ? firstOf(pos.container) : -1;
}

Pos FlowIndex::posAt(FlowPos flow) const {
    if (flow < 0) flow = 0;
    if (flow > size_) flow = size_;

    NodeId cur = root_;
    while (cur != kInvalidNodeId) {
        const Node* n = find(cur);
        if (n == nullptr || !isContainer(n->kind)) break;
        NodeId next = kInvalidNodeId;
        for (const Ref& child : n->children) {
            if (!inFlow(child->id)) continue;
            const int cfirst = firstOf(child->id);
            const int cend = endOf(child->id);
            const bool inner = (flow > cfirst && flow < cend);
            const bool leaf_head = (!isContainer(child->kind) && flow == cfirst && cfirst < cend);
            if (inner || leaf_head) {
                next = child->id;
                break;
            }
        }
        if (next == kInvalidNodeId) break;
        cur = next;
    }

    const Node* n = find(cur);
    if (n == nullptr) return Pos{root_, 0};
    if (!isContainer(n->kind)) return Pos{cur, flow - firstOf(cur)};

    if (flow == contentStartOf(cur)) return Pos{cur, 0};
    int last_slot = 0;
    for (size_t i = 0; i < n->children.size(); ++i) {
        const Ref& child = n->children[i];
        if (!inFlow(child->id)) continue;
        last_slot = static_cast<int>(i) + 1;
        if (flow == endOf(child->id)) return Pos{cur, last_slot};
    }
    return Pos{cur, last_slot};
}

}  // namespace heditor::rev
