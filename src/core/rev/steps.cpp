#include "core/rev/steps.h"

#include <algorithm>
#include <utility>

#include "core/doc/node.h"
#include "core/doc/subtree.h"
#include "core/rev/flow.h"
#include "core/rev/internal.h"

namespace heditor::rev {

namespace {

int textLenOf(const Node& n) {
    const auto* run = std::get_if<RunPayload>(&n.payload);
    return run == nullptr ? 0 : static_cast<int>(run->text.size());
}

/**
 * 从根到目标节点的孩子下标链。目标就是根时给出空链。
 */
bool pathTo(const Ref& node, NodeId target, std::vector<int>* path) {
    if (node->id == target) return true;
    for (size_t i = 0; i < node->children.size(); ++i) {
        path->push_back(static_cast<int>(i));
        if (pathTo(node->children[i], target, path)) return true;
        path->pop_back();
    }
    return false;
}

const Node* nodeAtPath(const Doc& doc, const std::vector<int>& path) {
    if (!doc.root) return nullptr;
    const Node* n = doc.root.get();
    for (int idx : path) {
        if (idx < 0 || idx >= static_cast<int>(n->children.size())) return nullptr;
        n = n->children[static_cast<size_t>(idx)].get();
    }
    return n;
}

/**
 * 目标节点所在那一层的路径（= 父节点的路径）。目标是根时返回 false。
 */
bool parentPathOf(const Doc& doc, NodeId target, std::vector<int>* path) {
    if (!doc.root) return false;
    if (!pathTo(doc.root, target, path)) return false;
    if (path->empty()) return false;
    path->pop_back();
    return true;
}

Ref fromSpec(const NodeSpec& s, NodeId* next, std::string* error);

Ref fromSpec(const NodeSpec& s, NodeId* next, std::string* error) {
    std::vector<Ref> kids;
    kids.reserve(s.children.size());
    for (const NodeSpec& c : s.children) {
        Ref kid = fromSpec(c, next, error);
        if (!kid) return nullptr;
        kids.push_back(std::move(kid));
    }
    Payload payload;
    switch (s.kind) {
        case NodeKind::TextRun:
            payload = RunPayload{s.text, s.marks, s.styleRef};
            break;
        case NodeKind::ParagraphMark:
            payload = MarkPayload{s.para, s.styleRef, s.marks};
            break;
        case NodeKind::ContentControl:
            payload = s.control;
            break;
        case NodeKind::Table:
            payload = s.table;
            break;
        case NodeKind::TableRow:
            payload = s.row;
            break;
        case NodeKind::TableCell:
            payload = s.cell;
            break;
        case NodeKind::Field:
            payload = s.field;
            break;
        case NodeKind::Image:
            payload = s.image;
            break;
        case NodeKind::Formula:
            payload = s.formula;
            break;
        case NodeKind::Section:
            // 节的页面设置装不进 NodeSpec（它引用同文件里的页眉页脚根），所以搬不过来。
            *error = "子树里含节：节记录不在 NodeSpec 里";
            return nullptr;
        default:
            break;
    }
    const NodeId id = (*next)++;
    return makeNode(id, s.kind, std::move(payload), std::move(kids), s.hidden);
}

struct Ctx {
    Doc doc;
    std::string error;
    /**
     * 这批步骤的流位置按哪种可见性算。整批同一个口径：批次内部不会换模式。
     */
    FlowVisibility vis = FlowVisibility::Design;
};

bool fail(Ctx* c, std::string why) {
    if (c->error.empty()) c->error = std::move(why);
    return false;
}

bool loadKids(const Doc& doc, const std::vector<int>& path, std::vector<Ref>* out) {
    const std::vector<Ref>* layer = detail::kidsAt(doc, path);
    if (layer == nullptr) return false;
    *out = *layer;
    return true;
}

bool rewrite(Ctx* c, const std::vector<int>& path, std::vector<Ref> kids) {
    Ref root = detail::withLeafChildren(c->doc, path, std::move(kids));
    if (!root) return fail(c, "路径失效");
    c->doc = adopt(std::move(root), c->doc.next_id);
    return true;
}

/**
 * 用同一个节点号换掉一个节点的内容。
 */
bool replaceOne(Ctx* c, NodeId id, Payload payload, bool hidden) {
    const Node* n = findNode(c->doc, id);
    if (n == nullptr) return fail(c, "节点不存在");
    Ref repl = makeNode(n->id, n->kind, std::move(payload), n->children, hidden);
    bool found = false;
    Ref root = detail::replaceNode(c->doc.root, id, repl, &found);
    if (!found) return fail(c, "节点不存在");
    c->doc = adopt(std::move(root), c->doc.next_id);
    return true;
}

/**
 * 流位置 → 孩子槽位，落在文本中间时先把那个 run 切开。
 *
 * 与模型层同一条理由：规范化会把相邻同属性的 run 并成一个，孩子槽位因此整体前移，
 * 只有流位置能活过它。切分会改文档，所以这里吃的是可写的 Ctx。
 */
bool slotAtFlow(Ctx* c, FlowPos flow, NodeId* container, int* slot) {
    FlowIndex idx(c->doc, c->vis);
    if (flow < 0 || flow > idx.size()) return fail(c, "流位置越界");
    const Pos p = idx.posAt(flow);
    const Node* n = findNode(c->doc, p.container);
    if (n == nullptr) return fail(c, "落点不在文档里");

    if (isText(n->kind) && p.offset > 0 && p.offset < textLenOf(*n)) {
        const RunPayload& run = std::get<RunPayload>(n->payload);
        std::vector<int> full;
        if (!pathTo(c->doc.root, n->id, &full) || full.empty()) return fail(c, "落点路径失效");
        const int self = full.back();
        std::vector<int> path(full.begin(), full.end() - 1);
        std::vector<Ref> kids;
        if (!loadKids(c->doc, path, &kids)) return fail(c, "落点路径失效");

        std::vector<uint32_t> left(run.text.begin(), run.text.begin() + p.offset);
        std::vector<uint32_t> right(run.text.begin() + p.offset, run.text.end());
        std::vector<Ref> repl;
        repl.push_back(detail::runNode(n->id, std::move(left), run.marks, run.style));
        repl.push_back(detail::runNode(c->doc.next_id++, std::move(right), run.marks, run.style));
        kids.erase(kids.begin() + self);
        kids.insert(kids.begin() + self, repl.begin(), repl.end());
        if (!rewrite(c, path, std::move(kids))) return false;
        idx = FlowIndex(c->doc, c->vis);
    }

    const Pos q = idx.posAt(flow);
    const Node* m = findNode(c->doc, q.container);
    if (m == nullptr) return fail(c, "落点不在文档里");
    if (isContainer(m->kind)) {
        *container = q.container;
        *slot = q.offset;
        return true;
    }
    std::vector<int> full;
    if (!pathTo(c->doc.root, m->id, &full) || full.empty()) return fail(c, "落点没有父容器");
    std::vector<int> parent(full.begin(), full.end() - 1);
    const Node* pn = nodeAtPath(c->doc, parent);
    if (pn == nullptr) return fail(c, "落点没有父容器");
    *container = pn->id;
    *slot = full.back();
    return true;
}

/**
 * 把一段码点插到流位置 at 上。语义与模型层 insertTextAt 逐条相同，包括"标记不同时新建
 * 节点不继承样式引用"这一处。
 */
bool insertRunAt(Ctx* c, FlowPos at, const std::vector<uint32_t>& cps, const CharFormat& marks,
                 const std::string& style) {
    if (cps.empty()) return true;
    FlowIndex idx(c->doc, c->vis);
    if (at < 0 || at > idx.size()) return fail(c, "插入位置越界");
    const Pos p = idx.posAt(at);
    const Node* n = findNode(c->doc, p.container);
    if (n == nullptr) return fail(c, "插入位置不在文档里");

    if (isText(n->kind)) {
        const RunPayload& run = std::get<RunPayload>(n->payload);
        if (run.marks == marks && run.style == style) {
            std::vector<uint32_t> grown = run.text;
            grown.insert(grown.begin() + p.offset, cps.begin(), cps.end());
            return replaceOne(c, n->id, RunPayload{std::move(grown), run.marks, run.style},
                              n->hidden);
        }
        std::vector<int> full;
        if (!pathTo(c->doc.root, n->id, &full) || full.empty())
            return fail(c, "插入位置没有父容器");
        std::vector<int> parent(full.begin(), full.end() - 1);
        std::vector<Ref> kids;
        if (!loadKids(c->doc, parent, &kids)) return fail(c, "插入路径失效");
        const int self = full.back();
        const int len = static_cast<int>(run.text.size());
        const bool has_left = p.offset > 0;
        const bool has_right = p.offset < len;
        std::vector<Ref> repl;
        if (has_left && has_right) {
            repl.push_back(detail::runNode(
                n->id, std::vector<uint32_t>(run.text.begin(), run.text.begin() + p.offset),
                run.marks, run.style));
            repl.push_back(detail::runNode(c->doc.next_id++, cps, marks, std::string()));
            repl.push_back(
                detail::runNode(c->doc.next_id++,
                                std::vector<uint32_t>(run.text.begin() + p.offset, run.text.end()),
                                run.marks, run.style));
        } else if (has_left) {
            repl.push_back(detail::runNode(n->id, run.text, run.marks, run.style));
            repl.push_back(detail::runNode(c->doc.next_id++, cps, marks, std::string()));
        } else {
            repl.push_back(detail::runNode(c->doc.next_id++, cps, marks, std::string()));
            repl.push_back(detail::runNode(n->id, run.text, run.marks, run.style));
        }
        kids.erase(kids.begin() + self);
        kids.insert(kids.begin() + self, repl.begin(), repl.end());
        return rewrite(c, parent, std::move(kids));
    }

    if (isContainer(n->kind)) {
        std::vector<int> path;
        if (!pathTo(c->doc.root, n->id, &path)) return fail(c, "插入路径失效");
        std::vector<Ref> kids;
        if (!loadKids(c->doc, path, &kids)) return fail(c, "插入路径失效");
        if (p.offset > 0) {
            const Ref& prev = kids[static_cast<size_t>(p.offset) - 1];
            if (isText(prev->kind) && std::get<RunPayload>(prev->payload).marks == marks) {
                RunPayload run = std::get<RunPayload>(prev->payload);
                run.text.insert(run.text.end(), cps.begin(), cps.end());
                kids[static_cast<size_t>(p.offset) - 1] =
                    detail::runNode(prev->id, std::move(run.text), run.marks, run.style);
                return rewrite(c, path, std::move(kids));
            }
        }
        if (p.offset < static_cast<int>(kids.size())) {
            const Ref& next = kids[static_cast<size_t>(p.offset)];
            if (isText(next->kind) && std::get<RunPayload>(next->payload).marks == marks) {
                RunPayload run = std::get<RunPayload>(next->payload);
                std::vector<uint32_t> grown = cps;
                grown.insert(grown.end(), run.text.begin(), run.text.end());
                kids[static_cast<size_t>(p.offset)] =
                    detail::runNode(next->id, std::move(grown), run.marks, run.style);
                return rewrite(c, path, std::move(kids));
            }
        }
        kids.insert(kids.begin() + p.offset, detail::runNode(c->doc.next_id++, cps, marks, style));
        return rewrite(c, path, std::move(kids));
    }

    std::vector<int> full;
    if (!pathTo(c->doc.root, n->id, &full) || full.empty()) return fail(c, "插入位置没有父容器");
    std::vector<int> parent(full.begin(), full.end() - 1);
    std::vector<Ref> kids;
    if (!loadKids(c->doc, parent, &kids)) return fail(c, "插入路径失效");
    kids.insert(kids.begin() + full.back(), detail::runNode(c->doc.next_id++, cps, marks, style));
    return rewrite(c, parent, std::move(kids));
}

/**
 * 把一枚段落标记插到流位置 at 上。落在 run 中间时先把 run 切开。
 */
bool insertMarkAt(Ctx* c, FlowPos at, const ParaFormatPatch& para, const std::string& style,
                  const CharFormat& marks) {
    FlowIndex idx(c->doc, c->vis);
    if (at < 0 || at > idx.size()) return fail(c, "分段位置越界");
    const Pos p = idx.posAt(at);
    const Node* n = findNode(c->doc, p.container);
    if (n == nullptr) return fail(c, "分段位置不在文档里");

    std::vector<int> parent_path;
    int ix = 0;
    std::vector<Ref> kids;
    if (isText(n->kind)) {
        std::vector<int> full;
        if (!pathTo(c->doc.root, n->id, &full) || full.empty())
            return fail(c, "分段位置没有父容器");
        parent_path.assign(full.begin(), full.end() - 1);
        if (!loadKids(c->doc, parent_path, &kids)) return fail(c, "分段路径失效");
        ix = full.back();
        const RunPayload& run = std::get<RunPayload>(n->payload);
        const int len = static_cast<int>(run.text.size());
        if (p.offset > 0 && p.offset < len) {
            std::vector<uint32_t> left(run.text.begin(), run.text.begin() + p.offset);
            std::vector<uint32_t> right(run.text.begin() + p.offset, run.text.end());
            std::vector<Ref> repl;
            repl.push_back(detail::runNode(n->id, std::move(left), run.marks, run.style));
            repl.push_back(
                detail::runNode(c->doc.next_id++, std::move(right), run.marks, run.style));
            kids.erase(kids.begin() + ix);
            kids.insert(kids.begin() + ix, repl.begin(), repl.end());
            ++ix;
        } else if (p.offset > 0) {
            ++ix;
        }
    } else if (isContainer(n->kind)) {
        if (!pathTo(c->doc.root, n->id, &parent_path)) return fail(c, "分段路径失效");
        if (!loadKids(c->doc, parent_path, &kids)) return fail(c, "分段路径失效");
        ix = p.offset;
    } else {
        std::vector<int> full;
        if (!pathTo(c->doc.root, n->id, &full) || full.empty())
            return fail(c, "分段位置没有父容器");
        parent_path.assign(full.begin(), full.end() - 1);
        if (!loadKids(c->doc, parent_path, &kids)) return fail(c, "分段路径失效");
        ix = full.back();
    }

    const NodeId mark = c->doc.next_id++;
    kids.insert(kids.begin() + ix,
                makeNode(mark, NodeKind::ParagraphMark, MarkPayload{para, style, marks}, {}));
    return rewrite(c, parent_path, std::move(kids));
}

struct Cover {
    NodeId node = kInvalidNodeId;
    int first = 0;
    int from = 0;
    int to = 0;
};

/**
 * 区间覆盖到的同层孩子。与模型层同一口径：任何夹在中间的非文本/标记元素都拒掉。
 */
bool collectCovers(const Doc& doc, const FlowIndex& idx, FlowPos begin, FlowPos end,
                   std::vector<Cover>* out, std::string* error) {
    out->clear();
    if (begin >= end) return true;
    const Pos b = idx.posAt(begin);
    const Node* bn = findNode(doc, b.container);
    if (bn == nullptr) {
        *error = "区间起点不在文档里";
        return false;
    }
    std::vector<int> parent_path;
    if (isContainer(bn->kind)) {
        if (!pathTo(doc.root, bn->id, &parent_path)) {
            *error = "区间起点没有父容器";
            return false;
        }
    } else if (!parentPathOf(doc, bn->id, &parent_path)) {
        *error = "区间起点没有父容器";
        return false;
    }
    const Node* p = nodeAtPath(doc, parent_path);
    if (p == nullptr) {
        *error = "区间起点没有父容器";
        return false;
    }
    for (const Ref& child : p->children) {
        int f = 0;
        int e = 0;
        if (!idx.intervalOf(child->id, &f, &e)) continue;
        if (e <= begin || f >= end) continue;
        const int from = f > begin ? f : begin;
        const int to = e < end ? e : end;
        if (to <= from) continue;
        if (!isText(child->kind) && !isParagraphMark(child->kind)) {
            *error = "删除区间含整块元素";
            return false;
        }
        out->push_back(Cover{child->id, f, from, to});
    }
    return true;
}

/**
 * 删掉流区间 [begin, end)。两端部分覆盖的文本截断，整覆盖的删掉。
 */
bool eraseFlowRange(Ctx* c, FlowPos begin, FlowPos end) {
    if (begin >= end) return true;
    const FlowIndex idx(c->doc, c->vis);
    if (begin < 0 || end > idx.size()) return fail(c, "删除区间越界");

    std::vector<Cover> covers;
    std::string err;
    if (!collectCovers(c->doc, idx, begin, end, &covers, &err)) return fail(c, std::move(err));

    // 覆盖总和必须恰好等于区间长。少了说明区间伸出了这一层（跨层），而按这一层删只会
    //   删掉其中一段 —— 那种"成功但少删了"的错，调用方看不出来。
    int covered = 0;
    for (const Cover& cov : covers) covered += cov.to - cov.from;
    if (covered != end - begin) return fail(c, "删除区间跨层或中间有洞");

    const Pos b = idx.posAt(begin);
    const Node* bn = findNode(c->doc, b.container);
    std::vector<int> path;
    if (isContainer(bn->kind)) {
        if (!pathTo(c->doc.root, bn->id, &path)) return fail(c, "删除路径失效");
    } else if (!parentPathOf(c->doc, bn->id, &path)) {
        return fail(c, "删除路径失效");
    }
    std::vector<Ref> kids;
    if (!loadKids(c->doc, path, &kids)) return fail(c, "删除路径失效");

    for (const Cover& cov : covers) {
        int slot = -1;
        for (size_t i = 0; i < kids.size(); ++i) {
            if (kids[i]->id == cov.node) {
                slot = static_cast<int>(i);
                break;
            }
        }
        if (slot < 0) return fail(c, "删除区间与文档对不上");
        const Ref& node = kids[static_cast<size_t>(slot)];
        const int span = isText(node->kind) ? textLenOf(*node) : 1;
        if (cov.from == cov.first && cov.to == cov.first + span) {
            kids.erase(kids.begin() + slot);
            continue;
        }
        const RunPayload& run = std::get<RunPayload>(node->payload);
        std::vector<uint32_t> kept(run.text.begin(), run.text.begin() + (cov.from - cov.first));
        kept.insert(kept.end(), run.text.begin() + (cov.to - cov.first), run.text.end());
        kids[static_cast<size_t>(slot)] =
            detail::runNode(node->id, std::move(kept), run.marks, run.style);
    }
    return rewrite(c, path, std::move(kids));
}

bool removeParaMarkAt(Ctx* c, FlowPos at) {
    const FlowIndex idx(c->doc, c->vis);
    if (at < 0 || at >= idx.size()) return fail(c, "删除段落标记：流位置越界");
    const Pos p = idx.posAt(at);
    const Node* n = findNode(c->doc, p.container);
    if (n == nullptr || !isParagraphMark(n->kind) || p.offset != 0) {
        return fail(c, "该位置上没有段落标记");
    }
    std::vector<int> path;
    if (!parentPathOf(c->doc, n->id, &path)) return fail(c, "段落标记没有父容器");
    std::vector<Ref> kids;
    if (!loadKids(c->doc, path, &kids)) return fail(c, "删除路径失效");
    std::vector<int> full;
    if (!pathTo(c->doc.root, n->id, &full) || full.empty()) return fail(c, "删除路径失效");
    kids.erase(kids.begin() + full.back());
    return rewrite(c, path, std::move(kids));
}

bool insertNodeAtFlow(Ctx* c, FlowPos at_flow, const NodeSpec& spec) {
    NodeId container = kInvalidNodeId;
    int slot = -1;
    if (!slotAtFlow(c, at_flow, &container, &slot)) return false;
    std::vector<int> path;
    if (!pathTo(c->doc.root, container, &path)) return fail(c, "插入节点：容器不在文档里");
    std::vector<Ref> kids;
    if (!loadKids(c->doc, path, &kids)) return fail(c, "插入节点：路径失效");
    if (slot < 0 || slot > static_cast<int>(kids.size())) return fail(c, "插入节点：槽位越界");
    Ref built = fromSpec(spec, &c->doc.next_id, &c->error);
    if (!built) return false;
    kids.insert(kids.begin() + slot, std::move(built));
    return rewrite(c, path, std::move(kids));
}

bool removeNodeAtFlow(Ctx* c, FlowPos at_flow) {
    NodeId container = kInvalidNodeId;
    int slot = -1;
    if (!slotAtFlow(c, at_flow, &container, &slot)) return false;
    std::vector<int> path;
    if (!pathTo(c->doc.root, container, &path)) return fail(c, "删除节点：容器不在文档里");
    std::vector<Ref> kids;
    if (!loadKids(c->doc, path, &kids)) return fail(c, "删除节点：路径失效");
    if (slot < 0 || slot >= static_cast<int>(kids.size())) return fail(c, "删除节点：槽位没有孩子");
    kids.erase(kids.begin() + slot);
    return rewrite(c, path, std::move(kids));
}

/**
 * 拆开一个容器：孩子原样上提到它的位置，它自己移除。
 */
bool unwrapAtFlow(Ctx* c, FlowPos at_flow) {
    NodeId container = kInvalidNodeId;
    int slot = -1;
    if (!slotAtFlow(c, at_flow, &container, &slot)) return false;
    std::vector<int> path;
    if (!pathTo(c->doc.root, container, &path)) return fail(c, "拆开节点：容器不在文档里");
    std::vector<Ref> kids;
    if (!loadKids(c->doc, path, &kids)) return fail(c, "拆开节点：路径失效");
    if (slot < 0 || slot >= static_cast<int>(kids.size())) return fail(c, "拆开节点：槽位没有孩子");
    const Ref target = kids[static_cast<size_t>(slot)];
    kids.erase(kids.begin() + slot);
    kids.insert(kids.begin() + slot, target->children.begin(), target->children.end());
    return rewrite(c, path, std::move(kids));
}

/**
 * 把流区间 [at_flow, at_flow + span_after) 里的孩子收进一个新容器。
 */
bool wrapAtFlow(Ctx* c, FlowPos at_flow, int span_after, const NodeSpec& spec) {
    NodeId container = kInvalidNodeId;
    int a = -1;
    if (!slotAtFlow(c, at_flow, &container, &a)) return false;
    int b = -1;
    if (!slotAtFlow(c, at_flow + span_after, &container, &b)) return false;
    if (a < 0 || b <= a) return fail(c, "包装区间为空");
    std::vector<int> path;
    if (!pathTo(c->doc.root, container, &path)) return fail(c, "包装节点：容器不在文档里");
    std::vector<Ref> kids;
    if (!loadKids(c->doc, path, &kids)) return fail(c, "包装节点：路径失效");
    if (b > static_cast<int>(kids.size())) return fail(c, "包装区间越界");

    std::vector<Ref> inner(kids.begin() + a, kids.begin() + b);
    Ref built = fromSpec(spec, &c->doc.next_id, &c->error);
    if (!built) return false;
    Ref wrapper = detail::withChildren(built, std::move(inner));
    kids.erase(kids.begin() + a, kids.begin() + b);
    kids.insert(kids.begin() + a, std::move(wrapper));
    return rewrite(c, path, std::move(kids));
}

bool setControlProp(Ctx* c, NodeId id, ControlProp prop, bool value) {
    const Node* n = findNode(c->doc, id);
    if (n == nullptr || n->kind != NodeKind::ContentControl) return fail(c, "目标不是控件");
    if (prop == ControlProp::Hidden) {
        if (n->hidden == value) return true;
        bool found = false;
        Ref repl = makeNode(n->id, n->kind, n->payload, n->children, value);
        Ref root = detail::replaceNode(c->doc.root, id, repl, &found);
        if (!found) return fail(c, "节点不存在");
        c->doc = adopt(std::move(root), c->doc.next_id);
        return true;
    }
    ControlSpec spec = std::get<ControlSpec>(n->payload);
    switch (prop) {
        case ControlProp::Readonly:
            spec.readonly = value;
            break;
        case ControlProp::Required:
            spec.required = value;
            break;
        case ControlProp::StructureLock:
            spec.structureLock = value;
            break;
        case ControlProp::Hidden:
            break;
    }
    return replaceOne(c, id, std::move(spec), n->hidden);
}

bool setControlValue(Ctx* c, NodeId id, const ControlValue& value) {
    const Node* n = findNode(c->doc, id);
    if (n == nullptr || n->kind != NodeKind::ContentControl) return fail(c, "目标不是控件");
    ControlSpec spec = std::get<ControlSpec>(n->payload);
    spec.value = value;
    return replaceOne(c, id, std::move(spec), n->hidden);
}

bool setControlSpec(Ctx* c, NodeId id, const ControlSpec& spec) {
    const Node* n = findNode(c->doc, id);
    if (n == nullptr || n->kind != NodeKind::ContentControl) return fail(c, "目标不是控件");
    return replaceOne(c, id, spec, n->hidden);
}

bool setCellSpec(Ctx* c, NodeId id, const CellSpec& spec) {
    const Node* n = findNode(c->doc, id);
    if (n == nullptr || n->kind != NodeKind::TableCell) return fail(c, "目标不是单元格");
    return replaceOne(c, id, spec, n->hidden);
}

bool setRowSpec(Ctx* c, NodeId id, const RowSpec& spec) {
    const Node* n = findNode(c->doc, id);
    if (n == nullptr || n->kind != NodeKind::TableRow) return fail(c, "目标不是表格行");
    return replaceOne(c, id, spec, n->hidden);
}

bool setTableSpec(Ctx* c, NodeId id, const TableSpec& spec) {
    const Node* n = findNode(c->doc, id);
    if (n == nullptr || n->kind != NodeKind::Table) return fail(c, "目标不是表格");
    return replaceOne(c, id, spec, n->hidden);
}

bool setImageSpec(Ctx* c, NodeId id, const ImageSpec& spec) {
    const Node* n = findNode(c->doc, id);
    if (n == nullptr || n->kind != NodeKind::Image) return fail(c, "目标不是图片");
    return replaceOne(c, id, spec, n->hidden);
}

bool setSectionSpec(Ctx* c, NodeId id, const SectionSpec& spec) {
    const Node* n = findNode(c->doc, id);
    if (n == nullptr || n->kind != NodeKind::Section) return fail(c, "目标不是节");
    return replaceOne(c, id, spec, n->hidden);
}

bool setImageSize(Ctx* c, NodeId id, const Length& width, const Length& height) {
    const Node* n = findNode(c->doc, id);
    if (n == nullptr || n->kind != NodeKind::Image) return fail(c, "目标不是图片");
    ImageSpec spec = std::get<ImageSpec>(n->payload);
    spec.width = width;
    spec.height = height;
    return replaceOne(c, id, std::move(spec), n->hidden);
}

bool applyOne(Ctx* c, const Step& step) {
    return std::visit(
        [&](const auto& s) -> bool {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, InsertText>) {
                return insertRunAt(c, s.at, s.cps, s.marks, s.styleRef);
            } else if constexpr (std::is_same_v<T, InsertFragment>) {
                FlowPos pos = s.at;
                for (const Piece& p : s.content.pieces) {
                    if (p.kind == PieceKind::ParaMark) {
                        if (!insertMarkAt(c, pos, p.para, p.styleRef, p.marks)) return false;
                    } else if (!insertRunAt(c, pos, p.cps, p.marks, p.styleRef)) {
                        return false;
                    }
                    pos += p.span();
                }
                return true;
            } else if constexpr (std::is_same_v<T, DeleteRange>) {
                return eraseFlowRange(c, s.begin, s.end);
            } else if constexpr (std::is_same_v<T, InsertParaMark>) {
                return insertMarkAt(c, s.at, s.para, s.styleRef, s.marks);
            } else if constexpr (std::is_same_v<T, RemoveParaMark>) {
                return removeParaMarkAt(c, s.at);
            } else if constexpr (std::is_same_v<T, SetParaMarkFormat>) {
                const Node* n = findNode(c->doc, s.id);
                if (n == nullptr || !isParagraphMark(n->kind)) return fail(c, "目标不是段落标记");
                return replaceOne(c, s.id, MarkPayload{s.para, s.styleRef, s.marks}, n->hidden);
            } else if constexpr (std::is_same_v<T, SetRun>) {
                const Node* n = findNode(c->doc, s.id);
                if (n == nullptr || n->kind != NodeKind::TextRun) {
                    return fail(c, "目标不是文本节点");
                }
                if (s.text.empty()) return fail(c, "文本节点的正文不能为空");
                // 正文一起换：切分时留下的那个节点只持有原正文的前缀。
                return replaceOne(c, s.id, RunPayload{s.text, s.marks, s.styleRef}, n->hidden);
            } else if constexpr (std::is_same_v<T, InsertNode>) {
                return insertNodeAtFlow(c, s.at_flow, s.subtree);
            } else if constexpr (std::is_same_v<T, RemoveNode>) {
                return removeNodeAtFlow(c, s.at_flow);
            } else if constexpr (std::is_same_v<T, UnwrapNode>) {
                return unwrapAtFlow(c, s.at_flow);
            } else if constexpr (std::is_same_v<T, WrapNode>) {
                return wrapAtFlow(c, s.at_flow, s.span_after, s.node);
            } else if constexpr (std::is_same_v<T, SetControlProp>) {
                return setControlProp(c, s.id, s.prop, s.value);
            } else if constexpr (std::is_same_v<T, SetControlValue>) {
                return setControlValue(c, s.id, s.value);
            } else if constexpr (std::is_same_v<T, SetControlSpec>) {
                return setControlSpec(c, s.id, s.spec);
            } else if constexpr (std::is_same_v<T, SetCellSpec>) {
                return setCellSpec(c, s.id, s.spec);
            } else if constexpr (std::is_same_v<T, SetRowSpec>) {
                return setRowSpec(c, s.id, s.spec);
            } else if constexpr (std::is_same_v<T, SetTableSpec>) {
                return setTableSpec(c, s.id, s.spec);
            } else if constexpr (std::is_same_v<T, SetImageSize>) {
                return setImageSize(c, s.id, s.width, s.height);
            } else if constexpr (std::is_same_v<T, SetSectionSpec>) {
                return setSectionSpec(c, s.id, s.spec);
            } else if constexpr (std::is_same_v<T, SetImageSpec>) {
                return setImageSpec(c, s.id, s.spec);
            }
            return fail(c, "未知步骤");
        },
        step);
}

Ref normalizeNode(const Ref& node) {
    if (!isContainer(node->kind)) return node;
    bool changed = false;
    std::vector<Ref> kids;
    kids.reserve(node->children.size());
    for (const Ref& child : node->children) {
        Ref nc = normalizeNode(child);
        if (nc != child) changed = true;
        kids.push_back(std::move(nc));
    }

    std::vector<Ref> kept;
    kept.reserve(kids.size());
    for (Ref& kid : kids) {
        if (isText(kid->kind) && textLenOf(*kid) == 0) {
            changed = true;
            continue;
        }
        kept.push_back(std::move(kid));
    }

    std::vector<Ref> merged;
    merged.reserve(kept.size());
    for (Ref& kid : kept) {
        if (!merged.empty() && isText(merged.back()->kind) && isText(kid->kind)) {
            const RunPayload& a = std::get<RunPayload>(merged.back()->payload);
            const RunPayload& b = std::get<RunPayload>(kid->payload);
            if (a.marks == b.marks) {
                std::vector<uint32_t> text = a.text;
                text.insert(text.end(), b.text.begin(), b.text.end());
                merged.back() =
                    detail::runNode(merged.back()->id, std::move(text), a.marks, a.style);
                changed = true;
                continue;
            }
        }
        merged.push_back(std::move(kid));
    }

    if (!changed) return node;
    return makeNode(node->id, node->kind, node->payload, std::move(merged), node->hidden);
}

}  // namespace

Doc normalize(const Doc& doc) {
    if (!doc.root) return doc;
    Ref root = normalizeNode(doc.root);
    if (root == doc.root) return doc;
    return adopt(std::move(root), doc.next_id);
}

EditResult applySteps(const Doc& doc, const std::vector<Step>& steps, FlowVisibility vis) {
    Ctx c{doc, {}, vis};
    for (const Step& step : steps) {
        if (!applyOne(&c, step)) {
            EditResult r;
            r.ok = false;
            r.error = c.error.empty() ? "步骤不可应用" : c.error;
            return r;
        }
    }
    c.doc = normalize(c.doc);
    return EditResult{true, std::move(c.doc), {}};
}

}  // namespace heditor::rev
