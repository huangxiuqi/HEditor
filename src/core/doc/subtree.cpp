#include "core/doc/subtree.h"

namespace heditor {

int NodeSpec::span() const {
    if (isText(kind)) return static_cast<int>(text.size());
    if (!isContainer(kind)) return 1;  // 原子与段落标记各占一位

    // 根不含边界（它不嵌在别处），其余容器两头各占一位。
    int total = (kind == NodeKind::Document) ? 0 : 2;
    for (const NodeSpec& c : children) total += c.span();
    return total;
}

NodeSpec captureSubtree(const NodeTree& tree, NodeId id) {
    NodeSpec spec;
    const Node* n = tree.find(id);
    if (n == nullptr) return spec;

    spec.kind = n->kind;
    spec.hidden = n->hidden;
    spec.text = n->text;
    spec.para = n->para;
    spec.styleRef = n->styleRef;
    spec.marks = n->marks;
    spec.control = n->control;
    spec.cell = n->cell;
    spec.row = n->row;
    spec.table = n->table;
    spec.field = n->field;
    spec.children.reserve(n->children.size());
    for (const Node* c : n->children) spec.children.push_back(captureSubtree(tree, c->id));
    return spec;
}

NodeId materializeSubtree(NodeTree& tree, NodeId parent, int at, const NodeSpec& spec) {
    const NodeId id = (parent == kInvalidNodeId) ? tree.createDetached(spec.kind)
                                                 : tree.insertChild(parent, at, spec.kind);
    if (id == kInvalidNodeId) return kInvalidNodeId;

    Node* n = tree.find(id);
    n->hidden = spec.hidden;
    n->para = spec.para;
    n->styleRef = spec.styleRef;
    n->marks = spec.marks;
    n->control = spec.control;
    n->cell = spec.cell;
    n->row = spec.row;
    n->table = spec.table;
    n->field = spec.field;
    if (isText(spec.kind)) tree.setText(id, spec.text);

    for (const NodeSpec& c : spec.children) {
        // 每次都取当前的末尾：孩子数在孩子数变，用循环变量当槽位会错位。
        const int slot = static_cast<int>(n->children.size());
        if (materializeSubtree(tree, id, slot, c) == kInvalidNodeId) return kInvalidNodeId;
    }
    return id;
}

}  // namespace heditor
