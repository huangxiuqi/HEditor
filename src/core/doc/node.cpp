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
        case NodeKind::Document: return "Document";
        case NodeKind::Section: return "Section";
        case NodeKind::Table: return "Table";
        case NodeKind::TableRow: return "TableRow";
        case NodeKind::TableCell: return "TableCell";
        case NodeKind::ContentControl: return "ContentControl";
        case NodeKind::Quote: return "Quote";
        case NodeKind::CodeBlock: return "CodeBlock";
        case NodeKind::ListItem: return "ListItem";
        case NodeKind::Link: return "Link";
        case NodeKind::Image: return "Image";
        case NodeKind::Formula: return "Formula";
        case NodeKind::Break: return "Break";
        case NodeKind::PageBreak: return "PageBreak";
        case NodeKind::Tab: return "Tab";
        case NodeKind::Field: return "Field";
        case NodeKind::TextRun: return "TextRun";
        case NodeKind::ParagraphMark: return "ParagraphMark";
    }
    return "（未知）";
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

bool NodeTree::setText(NodeId id, std::vector<uint32_t> cps) {
    Node* n = find(id);
    if (n == nullptr || !isText(n->kind)) return false;
    n->text = std::move(cps);
    return true;
}

}  // namespace heditor
