#include "core/doc/flow.h"

namespace heditor {

namespace {

/**
 * 节点在父容器里的槽位序号。没有父节点时返回 -1。
 */
int indexInParent(const Node* n) {
    if (n->parent == nullptr) return -1;
    const std::vector<Node*>& siblings = n->parent->children;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i] == n) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

FlowIndex::FlowIndex(const NodeTree& tree, FlowVisibility vis) : tree_(&tree), vis_(vis) {
    int cursor = 0;
    measure(tree.rootId(), cursor, true);
    size_ = cursor;
}

void FlowIndex::measure(NodeId id, int& cursor, bool is_root) {
    const Node* n = tree_->find(id);

    // 非编辑模式下隐藏的控件整棵子树摘除：不占位置，也不给后代记区间
    // （于是 flowAt 对它们一律返回 -1，"不在流里"与"越界"同一种回执）。
    if (vis_ == FlowVisibility::Display && n->hidden) return;

    const int start = cursor;
    first_[id] = start;

    if (isContainer(n->kind)) {
        // 根的展开不含边界：它不嵌在别处，没有任何位置指向"根之前 / 之后"。
        if (!is_root) ++cursor;
        for (Node* child : n->children) measure(child->id, cursor, false);
        if (!is_root) ++cursor;
    } else if (isText(n->kind)) {
        cursor += static_cast<int>(n->text.size());
    } else {
        ++cursor;  // 原子与段落标记各占一位
    }

    span_[id] = cursor - start;
}

int FlowIndex::contentStartOf(NodeId id) const {
    const bool is_root = (id == tree_->rootId());
    return firstOf(id) + (is_root ? 0 : 1);
}

bool FlowIndex::intervalOf(NodeId id, int* first, int* end) const {
    const auto it = first_.find(id);
    if (it == first_.end()) return false;
    if (first != nullptr) *first = it->second;
    if (end != nullptr) *end = it->second + span_.at(id);
    return true;
}

FlowPos FlowIndex::flowAt(const Pos& pos) const {
    const Node* n = tree_->find(pos.container);
    if (n == nullptr || pos.offset < 0) return -1;
    // 被摘除的子树整个不在流里，它内部任何位置都不可寻址。
    if (!inFlow(pos.container)) return -1;

    if (isContainer(n->kind)) {
        const int slots = static_cast<int>(n->children.size());
        if (pos.offset > slots) return -1;
        if (pos.offset == 0) return contentStartOf(pos.container);
        // 槽位 j 的含义是"在第 j-1 个孩子之后"：那个孩子若被摘除，这个位置就不存在。
        const Node* prev = n->children[static_cast<size_t>(pos.offset) - 1];
        if (!inFlow(prev->id)) return -1;
        return endOf(prev->id);
    }
    if (isText(n->kind)) {
        if (pos.offset > static_cast<int>(n->text.size())) return -1;
        return firstOf(pos.container) + pos.offset;
    }
    // 原子与段落标记内部没有可寻址的位置，只有"自身之前"这一个。
    return pos.offset == 0 ? firstOf(pos.container) : -1;
}

Pos FlowIndex::posAt(FlowPos flow) const {
    if (flow < 0) flow = 0;
    if (flow > size_) flow = size_;

    // 先下降到最内层的"包含"这个流位置的节点。三条规则决定了归属：
    //   严格内部（firstOf < flow < endOf）→ 属于这个孩子，下降；
    //   flow == endOf(c) → 属于 c 之后，留在父容器（那个槽位就是"c 之后"）；
    //   flow == firstOf(c) → 对容器是"c 之前"（留在父容器），对叶子则归它自己
    //     —— 因为"容器的第 0 槽位"与"第一个孩子的第一个位置"是同一个流位置，
    //     规范表示取最内层，编辑时才好判断"字要插进谁里面"。
    NodeId cur = tree_->rootId();
    while (isContainer(tree_->find(cur)->kind)) {
        const Node* n = tree_->find(cur);
        NodeId next = kInvalidNodeId;
        for (Node* child : n->children) {
            // 被摘除的孩子不参与归属判定：它不在流里，没有位置可归给它。
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

    const Node* n = tree_->find(cur);
    if (!isContainer(n->kind)) {
        // 叶子内部：文本按码点偏移，原子与标记只会停在"自身之前"。
        return Pos{cur, flow - firstOf(cur)};
    }
    if (flow == contentStartOf(cur)) return Pos{cur, 0};
    // 末尾槽位要落在最后一个在流里的孩子之后：直接写 children.size() 的话，
    // 尾部若有被摘除的孩子，那个槽位是不可寻址的（见 flowAt），往返就断了。
    int last_slot = 0;
    for (size_t i = 0; i < n->children.size(); ++i) {
        if (!inFlow(n->children[i]->id)) continue;
        last_slot = static_cast<int>(i) + 1;
        if (flow == endOf(n->children[i]->id)) return Pos{cur, last_slot};
    }
    // 剩下的两种：容器结束边界之前（内容末尾），或根的全部内容之后。两者都是上面那个槽位。
    return Pos{cur, last_slot};
}

}  // namespace heditor
