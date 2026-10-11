#include "core/doc/para.h"

#include <algorithm>

namespace heditor {

ParaIndex::ParaIndex(const NodeTree& tree, FlowVisibility vis) { rebuild(tree, vis); }

void ParaIndex::rebuild(const NodeTree& tree, FlowVisibility vis) {
    tree_ = &tree;
    vis_ = vis;
    marks_.clear();
    scan(tree.rootId());
}

void ParaIndex::scan(NodeId id) {
    const Node* n = tree_->find(id);
    if (n == nullptr || !isContainer(n->kind)) return;
    // 呈现态下隐藏的子树整棵不在流里，连它的标记数组都不建 —— 与 FlowIndex 同一条规则。
    if (vis_ == FlowVisibility::Display && n->hidden) return;

    std::vector<int> marks;
    for (size_t i = 0; i < n->children.size(); ++i) {
        const Node* c = n->children[i];
        if (vis_ == FlowVisibility::Display && c->hidden) continue;
        if (isParagraphMark(c->kind)) marks.push_back(static_cast<int>(i));
        scan(c->id);
    }
    marks_.emplace(n->id, std::move(marks));
}

const std::vector<int>* ParaIndex::marksOf(NodeId container) const {
    const auto it = marks_.find(container);
    return it == marks_.end() ? nullptr : &it->second;
}

int ParaIndex::markCount(NodeId container) const {
    const std::vector<int>* m = marksOf(container);
    return m == nullptr ? -1 : static_cast<int>(m->size());
}

int ParaIndex::paragraphCount(NodeId container) const {
    const std::vector<int>* m = marksOf(container);
    if (m == nullptr) return -1;
    const int n = static_cast<int>(tree_->find(container)->children.size());
    // 最后一个标记之后还有孩子 → 多一段尚未结束的。空容器是 0 段。
    const int last_mark = m->empty() ? -1 : m->back();
    const bool trailing = (n - last_mark - 1) > 0;
    return static_cast<int>(m->size()) + (trailing ? 1 : 0);
}

ParaNo ParaIndex::paragraphNoIn(NodeId container, int at) const {
    const std::vector<int>* m = marksOf(container);
    if (m == nullptr) return 0;
    if (at < 0 || at > static_cast<int>(tree_->find(container)->children.size())) return 0;
    const auto it = std::lower_bound(m->begin(), m->end(), at);
    return static_cast<ParaNo>(it - m->begin()) + 1;
}

bool ParaIndex::paragraphRange(NodeId container, ParaNo no, int* begin, int* end) const {
    const std::vector<int>* m = marksOf(container);
    if (m == nullptr || no < 1) return false;
    const int n = static_cast<int>(tree_->find(container)->children.size());
    const int k = static_cast<int>(m->size());
    if (no > k + 1) return false;

    const int b = (no == 1) ? 0 : (*m)[static_cast<size_t>(no) - 2] + 1;
    const int e = (no <= k) ? (*m)[static_cast<size_t>(no) - 1] + 1 : n;
    // 内容末尾已被标记结束时，那个"下一段"还没有内容，它不是一个段落。
    if (b >= e) return false;
    if (begin != nullptr) *begin = b;
    if (end != nullptr) *end = e;
    return true;
}

NodeId ParaIndex::markOfParagraph(NodeId container, ParaNo no, bool* ok) const {
    const std::vector<int>* m = marksOf(container);
    if (m == nullptr || no < 1 || no > static_cast<int>(m->size())) {
        if (ok != nullptr) *ok = false;
        return kInvalidNodeId;
    }
    if (ok != nullptr) *ok = true;
    const int child = (*m)[static_cast<size_t>(no) - 1];
    return tree_->find(container)->children[static_cast<size_t>(child)]->id;
}

std::vector<NodeId> ParaIndex::containers() const {
    std::vector<NodeId> out;
    out.reserve(marks_.size());
    for (const auto& kv : marks_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

bool ParaIndex::onInsert(NodeId container, int at, NodeId inserted) {
    const auto it = marks_.find(container);
    if (it == marks_.end() || at < 0) return false;
    const Node* c = tree_->find(inserted);
    if (c == nullptr) return false;
    if (vis_ == FlowVisibility::Display && c->hidden) return true;

    std::vector<int>& m = it->second;
    for (int& v : m) {
        if (v >= at) ++v;
    }
    if (isParagraphMark(c->kind)) {
        m.insert(std::lower_bound(m.begin(), m.end(), at), at);
    }
    // 插进来的若是容器，它自己也要有一条 —— 漏了它，记账范围就与重建的结果不同，
    // 而"范围一致"正是增量维护的第一条判据（新子树此刻是空的，scan 只建一条空数组）。
    scan(inserted);
    return true;
}

bool ParaIndex::onErase(NodeId container, int at, NodeId erased) {
    const auto it = marks_.find(container);
    if (it == marks_.end() || at < 0) return false;
    const Node* c = tree_->find(erased);
    if (c == nullptr) return false;
    if (vis_ == FlowVisibility::Display && c->hidden) return true;

    std::vector<int>& m = it->second;
    // 这个下标是不是标记由数组本身回答，不靠调用方报的种类 —— 报了错的种类也不会删错。
    const auto pos = std::lower_bound(m.begin(), m.end(), at);
    if (pos != m.end() && *pos == at) m.erase(pos);
    for (int& v : m) {
        if (v > at) --v;
    }
    // 被摘下的子树里的容器不再在流里，它们的记账项要一并去掉。
    drop(erased);
    return true;
}

void ParaIndex::drop(NodeId id) {
    const Node* n = tree_->find(id);
    if (n == nullptr || !isContainer(n->kind)) return;
    marks_.erase(id);
    for (const Node* c : n->children) drop(c->id);
}

}  // namespace heditor
