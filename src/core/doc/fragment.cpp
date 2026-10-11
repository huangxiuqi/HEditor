#include "core/doc/fragment.h"

namespace heditor {

int Piece::span() const { return kind == PieceKind::ParaMark ? 1 : static_cast<int>(cps.size()); }

int Fragment::span() const {
    int total = 0;
    for (const Piece& p : pieces) total += p.span();
    return total;
}

bool collectCovers(const NodeTree& tree, const Range& range, std::vector<Cover>* out,
                   std::string* error) {
    out->clear();
    if (range.empty()) return true;

    const FlowIndex idx(tree, FlowVisibility::Design);
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
                         "（节点级编辑还没落地，1.2 的可编辑集合只有文本与段落标记）";
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
