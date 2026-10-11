#include "core/doc/normalize.h"

namespace heditor {

namespace {

/**
 * 把一个容器这一层规范到不动为止。
 *
 * 先清空 run、再合并，两件事分成两趟：合成一趟的话，一个空 run 会被算成一次"合并"，
 * 读读数的人就分不清是哪一种规范化起了作用。两趟都是线性的 —— 清空那一趟每轮要么前进、
 * 要么让孩子数减一；合并那一趟合并之后下标不前进（合并出来的那个可能还能跟下一个合并），
 * 但每合并一次孩子数就减一。
 */
void normalizeLevel(NodeTree& tree, NodeId container, NormalizeResult* out) {
    for (int i = 0;;) {
        // 每一步都重新取：上一步的删除会让那个孩子的指针数组失效。
        const Node* c = tree.find(container);
        if (c == nullptr) return;
        if (i >= static_cast<int>(c->children.size())) break;
        Node* n = c->children[static_cast<size_t>(i)];
        if (isText(n->kind) && n->text.empty()) {
            tree.eraseChild(container, i);
            ++out->removed;
            continue;
        }
        ++i;
    }

    for (int i = 0;;) {
        const Node* c = tree.find(container);
        if (c == nullptr) return;
        const int n = static_cast<int>(c->children.size());
        if (i + 1 >= n) return;
        Node* a = c->children[static_cast<size_t>(i)];
        Node* b = c->children[static_cast<size_t>(i) + 1];
        if (isText(a->kind) && isText(b->kind) && a->marks == b->marks) {
            std::vector<uint32_t> merged = a->text;
            merged.insert(merged.end(), b->text.begin(), b->text.end());
            tree.setText(a->id, std::move(merged));
            tree.eraseChild(container, i + 1);
            ++out->merged;
            continue;
        }
        ++i;
    }
}

/**
 * 自底向上：先把每个子容器规范成规范形状，再规范本层。
 *
 * 递归不会改动本层的孩子列表（子容器规范的是它自己的子树，不会删掉它自己），
 * 所以遍历本层的孩子是安全的。顺序定成自底向上，是为了让"退出时整棵子树都已规范"成立。
 */
void normalizeSubtree(NodeTree& tree, NodeId id, NormalizeResult* out) {
    const Node* c = tree.find(id);
    if (c == nullptr || !isContainer(c->kind)) return;
    for (size_t k = 0; k < c->children.size(); ++k) {
        normalizeSubtree(tree, c->children[k]->id, out);
    }
    normalizeLevel(tree, id, out);
}

}  // namespace

NormalizeResult normalizeDocument(NodeTree& tree) {
    NormalizeResult out;
    normalizeSubtree(tree, tree.rootId(), &out);
    return out;
}

}  // namespace heditor
