#include "core/doc/node.h"

namespace heditor {

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
    Node* raw = section.get();
    index_[raw->id] = raw;
    root_->children.push_back(raw);
    pool_.push_back(std::move(section));
}

NodeId NodeTree::firstSectionId() const {
    for (const Node* c : root_->children) {
        if (c->kind == NodeKind::Section) return c->id;
    }
    return kInvalidNodeId;
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
    // 沿父链上溯到根才算挂在文档上。只看"有没有父节点"是不够的：已摘下子树里的后代
    // 也有父节点（指向同一棵摘下的子树），那样会把它们当成活的，于是裁决结果或编辑被写到
    // 一个不在文档里的位置上。上溯步数封顶，免得畸形文档里的环把这里转死。
    const Node* cur = n;
    for (int steps = 0; cur != nullptr && steps <= static_cast<int>(tree.nodeCount()) + 1;
         ++steps) {
        if (cur->id == tree.rootId()) return true;
        cur = cur->parent;
    }
    return false;
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

NodeId NodeTree::createDetachedWithId(NodeId id, NodeKind kind) {
    if (id == kInvalidNodeId) return kInvalidNodeId;
    if (index_.find(id) != index_.end()) return kInvalidNodeId;

    auto node = std::make_unique<Node>();
    node->id = id;
    node->kind = kind;
    Node* raw = node.get();
    index_[raw->id] = raw;
    pool_.push_back(std::move(node));
    // 分配器推进到这个 id 之后，否则后面新分配的 id 会与它撞上。
    if (id >= next_id_) next_id_ = id + 1;
    return raw->id;
}

void NodeTree::reserveIdWatermark(NodeId next) {
    if (next > next_id_) next_id_ = next;
}

void NodeTree::reset() {
    // 根留下，其余连池里的记录一起丢掉 —— 留池会让记录里的 id 撞上摘下的旧节点。
    std::vector<std::unique_ptr<Node>> kept;
    for (std::unique_ptr<Node>& up : pool_) {
        if (up.get() == root_) kept.push_back(std::move(up));
    }
    pool_.clear();
    index_.clear();
    root_->children.clear();
    root_->parent = nullptr;
    pool_ = std::move(kept);
    index_[root_->id] = root_;
    next_id_ = root_->id + 1;
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
        dst->row = src->row;
        dst->table = src->table;
        dst->field = src->field;
        dst->image = src->image;
        dst->formula = src->formula;
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
    next_id_ = other.next_id_;
}

NodeTree NodeTree::clone() const {
    NodeTree out;
    out.assignFrom(*this);
    return out;
}

}  // namespace heditor
