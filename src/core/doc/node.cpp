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
        case NodeKind::HeaderFooter:
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
        case NodeKind::HeaderFooter:
            return "HeaderFooter";
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

int NodeTree::sectionCount() const {
    int count = 0;
    for (const Node* c : root_->children) {
        if (c->kind == NodeKind::Section) ++count;
    }
    return count;
}

NodeId NodeTree::sectionAt(int index) const {
    if (index < 0) return kInvalidNodeId;
    int seen = 0;
    for (const Node* c : root_->children) {
        if (c->kind != NodeKind::Section) continue;
        if (seen == index) return c->id;
        ++seen;
    }
    return kInvalidNodeId;
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

void NodeTree::assignFrom(const NodeTree& other) {
    if (this == &other) return;

    pool_.clear();
    index_.clear();
    root_ = nullptr;
    section_ = nullptr;
    next_id_ = 1;

    // 先按池的顺序建出全部节点（id 原样保留），再补父子链 —— 补链要按 id 查新节点，
    // 所以两遍走。摘下的子树也在池里，一并拷过来，节的页眉页脚引用因此不会指向空。
    std::unordered_map<NodeId, Node*> remap;
    remap.reserve(other.pool_.size());
    pool_.reserve(other.pool_.size());
    for (const std::unique_ptr<Node>& src : other.pool_) {
        auto dst = std::make_unique<Node>();
        dst->id = src->id;
        dst->kind = src->kind;
        dst->text = src->text;
        dst->hidden = src->hidden;
        dst->para = src->para;
        dst->styleRef = src->styleRef;
        dst->marks = src->marks;
        dst->control = src->control;
        dst->cell = src->cell;
        dst->table = src->table;
        dst->field = src->field;
        dst->section = src->section;
        remap[dst->id] = dst.get();
        index_[dst->id] = dst.get();
        pool_.push_back(std::move(dst));
    }
    // 第二遍只搬 parent 与 children 这两个指针字段：整节点赋值会把旧指针也拷进来。
    for (size_t i = 0; i < other.pool_.size(); ++i) {
        Node* dst = pool_[i].get();
        const Node* src = other.pool_[i].get();
        dst->parent = (src->parent == nullptr) ? nullptr : remap[src->parent->id];
        dst->children.clear();
        dst->children.reserve(src->children.size());
        for (const Node* c : src->children) dst->children.push_back(remap[c->id]);
    }

    root_ = (other.root_ == nullptr) ? nullptr : remap[other.root_->id];
    section_ = (other.section_ == nullptr) ? nullptr : remap[other.section_->id];
    next_id_ = other.next_id_;
}

NodeTree NodeTree::clone() const {
    NodeTree out;
    out.assignFrom(*this);
    return out;
}

}  // namespace heditor
