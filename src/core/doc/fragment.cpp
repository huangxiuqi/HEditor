#include "core/doc/fragment.h"

namespace heditor {

bool collectCovers(const NodeTree& tree, const Range& range, std::vector<Cover>* out,
                   std::string* error) {
    return collectCovers(tree, FlowIndex(tree, FlowVisibility::Design), range, out, error);
}

bool collectCovers(const NodeTree& tree, const FlowIndex& idx, const Range& range,
                   std::vector<Cover>* out, std::string* error) {
    out->clear();
    if (range.empty()) return true;

    const Pos b = idx.posAt(range.begin());
    const Node* bn = tree.find(b.container);
    if (bn == nullptr) {
        if (error != nullptr) *error = "区间起点不在树上";
        return false;
    }
    // 共同父容器：起点落在槽位上时那个容器就是父，落在文本（或标记、原子）里时父才在上一层。
    const NodeId parent = isContainer(bn->kind) ? bn->id : bn->parent->id;
    const Node* p = tree.find(parent);
    if (p == nullptr) {
        if (error != nullptr) *error = "区间起点没有父容器";
        return false;
    }

    for (const Node* c : p->children) {
        int f = 0;
        int e = 0;
        if (!idx.intervalOf(c->id, &f, &e)) continue;
        if (e <= range.begin() || f >= range.end()) continue;
        const FlowPos from = f > range.begin() ? f : range.begin();
        const FlowPos to = e < range.end() ? e : range.end();
        if (to <= from) continue;
        if (!isText(c->kind) && !isParagraphMark(c->kind)) {
            if (error != nullptr) {
                *error = std::string("区间含 ") + nodeKindName(c->kind) +
                         "（含整棵节点的区间提取还没落地：目前的可编辑集合只有文本与段落标记；"
                         "控件这类整块元素走节点级步骤 InsertNode / RemoveNode）";
            }
            return false;
        }
        out->push_back(Cover{c->id, f, from, to});
    }
    return true;
}

ExtractResult extractRange(const NodeTree& tree, Range range) {
    ExtractResult r;
    range = normalizeRange(tree, range);

    std::vector<Cover> covers;
    std::string err;
    if (!collectCovers(tree, range, &covers, &err)) {
        r.error = err;
        return r;
    }

    for (const Cover& c : covers) {
        const Node* n = tree.find(c.node);
        Piece piece;
        piece.styleRef = n->styleRef;
        if (isText(n->kind)) {
            piece.kind = PieceKind::Text;
            piece.marks = n->marks;
            piece.cps.assign(n->text.begin() + (c.from - c.first),
                             n->text.begin() + (c.to - c.first));
            if (piece.cps.empty()) continue;
        } else {
            piece.kind = PieceKind::ParaMark;
            piece.para = n->para;
        }
        r.fragment.pieces.push_back(std::move(piece));
    }
    r.ok = true;
    return r;
}

}  // namespace heditor
