#include "core/doc/control_index.h"

#include <algorithm>

namespace heditor {

namespace {

const std::vector<NodeId> kNoControls;

/**
 * 前序收集一棵子树里的控件（含给定的根自己）。
 *
 * 从给定的根开始走，而不是从文档根：被摘下的子树不在树上，但它的内部结构还在，
 * 所以"删掉了哪棵子树"也能在它被摘下来之后照样数清楚。
 */
void collectControls(const NodeTree& tree, NodeId id, std::vector<NodeId>* out) {
    const Node* n = tree.find(id);
    if (n == nullptr) return;
    if (n->kind == NodeKind::ContentControl) out->push_back(id);
    for (const Node* c : n->children) collectControls(tree, c->id, out);
}

/**
 * 这棵子树里有没有控件。用来区分"值替换"那类只动文本节点的增删 ——
 * 它不该为控件索引付代价。
 */
bool containsControl(const NodeTree& tree, NodeId id) {
    std::vector<NodeId> ids;
    collectControls(tree, id, &ids);
    return !ids.empty();
}

}  // namespace

void ControlIndex::rebuild(const NodeTree& tree) {
    key_of_.clear();
    by_key_.clear();

    std::vector<NodeId> ids;
    collectControls(tree, tree.rootId(), &ids);
    for (NodeId id : ids) {
        const Node* n = tree.find(id);
        key_of_[id] = n->control.key;
        by_key_[n->control.key].push_back(id);
    }
}

bool ControlIndex::onStep(const NodeTree& tree, const Step& step) {
    NodeId subtree = kInvalidNodeId;
    if (const auto* s = std::get_if<InsertNode>(&step)) {
        subtree = s->detached;
    } else if (const auto* s = std::get_if<RemoveNode>(&step)) {
        subtree = s->detached;
    } else {
        // 其余步骤既不增删控件、也不改业务编码：敲字、分段、删除文本、改属性都不动这两张表。
        return true;
    }

    // 身份不在步骤里（序列化还原出来的 InsertNode）：插进去的可能是控件，索引跟不了。
    if (subtree == kInvalidNodeId) return false;

    // 子树里没有控件就不动表。这一步是热路径的关键：给文本控件整体设值是"摘掉旧文本节点、
    // 插一个新文本节点"，一次批量设一千个字段会走一千遍，每次重扫全篇就白费了。
    if (!containsControl(tree, subtree)) return true;

    // 含控件的增删是低频的（加一个字段、删一个字段），重扫一遍换"键表按文档顺序"这条正确性。
    rebuild(tree);
    return true;
}

const std::string* ControlIndex::keyOf(NodeId id) const {
    const auto it = key_of_.find(id);
    return it == key_of_.end() ? nullptr : &it->second;
}

const std::vector<NodeId>& ControlIndex::byKey(const std::string& key) const {
    const auto it = by_key_.find(key);
    return it == by_key_.end() ? kNoControls : it->second;
}

std::vector<NodeId> controlIds(const NodeTree& tree) {
    std::vector<NodeId> ids;
    collectControls(tree, tree.rootId(), &ids);
    return ids;
}

}  // namespace heditor
