#include "core/doc/node.h"

namespace heditor {

bool isContainer(NodeKind kind) {
    // 逐个列出而不是 default: true —— 少写一种时 -Wswitch 会报出来，
    // 而"忘了把新种类归到哪一类"正是最容易发生、又最难发现的那种错。
    switch (kind) {
        case NodeKind::Document:
        case NodeKind::Section:
        case NodeKind::Table:
        case NodeKind::TableRow:
        case NodeKind::TableCell:
        case NodeKind::ContentControl:
        case NodeKind::Quote:
        case NodeKind::CodeBlock:
        case NodeKind::ListItem:
        case NodeKind::Link:
            return true;
        case NodeKind::Image:
        case NodeKind::Formula:
        case NodeKind::Break:
        case NodeKind::PageBreak:
        case NodeKind::Tab:
        case NodeKind::Field:
        case NodeKind::TextRun:
        case NodeKind::ParagraphMark:
            return false;
    }
    return false;
}

bool isText(NodeKind kind) { return kind == NodeKind::TextRun; }

bool isParagraphMark(NodeKind kind) { return kind == NodeKind::ParagraphMark; }

const char* nodeKindName(NodeKind kind) {
    switch (kind) {
        case NodeKind::Document:
            return "Document";
        case NodeKind::Section:
            return "Section";
        case NodeKind::Table:
            return "Table";
        case NodeKind::TableRow:
            return "TableRow";
        case NodeKind::TableCell:
            return "TableCell";
        case NodeKind::ContentControl:
            return "ContentControl";
        case NodeKind::Quote:
            return "Quote";
        case NodeKind::CodeBlock:
            return "CodeBlock";
        case NodeKind::ListItem:
            return "ListItem";
        case NodeKind::Link:
            return "Link";
        case NodeKind::Image:
            return "Image";
        case NodeKind::Formula:
            return "Formula";
        case NodeKind::Break:
            return "Break";
        case NodeKind::PageBreak:
            return "PageBreak";
        case NodeKind::Tab:
            return "Tab";
        case NodeKind::Field:
            return "Field";
        case NodeKind::TextRun:
            return "TextRun";
        case NodeKind::ParagraphMark:
            return "ParagraphMark";
    }
    return "（未知）";
}

bool acceptsChild(NodeKind parent, NodeKind child) {
    switch (parent) {
        case NodeKind::Document:
            return child == NodeKind::Section;
        case NodeKind::Table:
            return child == NodeKind::TableRow;
        case NodeKind::TableRow:
            return child == NodeKind::TableCell;
        default:
            return true;
    }
}

NodeTree::NodeTree() {
    auto doc = std::make_unique<Node>();
    doc->id = allocateId();
    doc->kind = NodeKind::Document;
    root_ = doc.get();
    index_[root_->id] = root_;
    pool_.push_back(std::move(doc));

    auto section = std::make_unique<Node>();
    section->id = allocateId();
    section->kind = NodeKind::Section;
    section->parent = root_;
    section_ = section.get();
    index_[section_->id] = section_;
    root_->children.push_back(section_);
    pool_.push_back(std::move(section));
}

Node* NodeTree::find(NodeId id) {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : it->second;
}

const Node* NodeTree::find(NodeId id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : it->second;
}

bool isAttached(const NodeTree& tree, NodeId id) {
    const Node* n = tree.find(id);
    if (n == nullptr) return false;
    return n->parent != nullptr || id == tree.rootId();
}

NodeId NodeTree::insertChild(NodeId parent, int at, NodeKind kind) {
    Node* p = find(parent);
    if (p == nullptr || !isContainer(p->kind)) return kInvalidNodeId;
    if (at < 0 || at > static_cast<int>(p->children.size())) return kInvalidNodeId;

    auto node = std::make_unique<Node>();
    node->id = allocateId();
    node->kind = kind;
    node->parent = p;
    Node* raw = node.get();
    index_[raw->id] = raw;
    p->children.insert(p->children.begin() + at, raw);
    pool_.push_back(std::move(node));
    return raw->id;
}

NodeId NodeTree::eraseChild(NodeId parent, int at) {
    Node* p = find(parent);
    if (p == nullptr) return kInvalidNodeId;
    if (at < 0 || at >= static_cast<int>(p->children.size())) return kInvalidNodeId;

    Node* c = p->children[static_cast<size_t>(at)];
    p->children.erase(p->children.begin() + at);
    // 只解开它自己与父节点的链：子树的内部父子关系保持原样，撤销时可以整棵搬回。
    c->parent = nullptr;
    return c->id;
}

NodeId NodeTree::appendChild(NodeId parent, NodeKind kind) {
    const Node* p = find(parent);
    if (p == nullptr) return kInvalidNodeId;
    return insertChild(parent, static_cast<int>(p->children.size()), kind);
}

NodeId NodeTree::attachChild(NodeId parent, int at, NodeId detached) {
    Node* p = find(parent);
    if (p == nullptr || !isContainer(p->kind)) return kInvalidNodeId;
    if (at < 0 || at > static_cast<int>(p->children.size())) return kInvalidNodeId;

    Node* c = find(detached);
    if (c == nullptr) return kInvalidNodeId;
    // 必须是被摘下的：parent 非空说明它还挂在别处，再挂一次会让同一棵子树出现在两个父亲下面。
    if (c->parent != nullptr || c == root_) return kInvalidNodeId;
    // 目标容器若在这棵子树内部，挂上就成环（此后从根走不到它，整块变成孤儿）。
    for (const Node* up = p; up != nullptr; up = up->parent) {
        if (up == c) return kInvalidNodeId;
    }

    p->children.insert(p->children.begin() + at, c);
    c->parent = p;
    return c->id;
}

NodeId NodeTree::createDetached(NodeKind kind) {
    auto node = std::make_unique<Node>();
    node->id = allocateId();
    node->kind = kind;
    Node* raw = node.get();
    index_[raw->id] = raw;
    pool_.push_back(std::move(node));
    return raw->id;
}

bool NodeTree::setText(NodeId id, std::vector<uint32_t> cps) {
    Node* n = find(id);
    if (n == nullptr || !isText(n->kind)) return false;
    n->text = std::move(cps);
    return true;
}

}  // namespace heditor
