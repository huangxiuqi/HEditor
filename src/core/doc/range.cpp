#include "core/doc/range.h"

#include "core/doc/flow.h"

namespace heditor {

namespace {

/**
 * 端点能不能落在这个容器的内部。
 *
 * 返回 false 的是不可半分单位：控件、表格与它的行列、链接。文档、节、引用、代码块、列表项
 * 是分段容器，端点落在它们内部就是落在某一段的边上。文本与原子不是容器：文本内部的每个
 * 码点位置本来就合法，而原子只占一个流位置（没有"严格内部"这种位置），所以一律允许。
 *
 * 逐种列出而不是给 default：新加一种容器时 -Wswitch 会报出来，而"新种类归到哪一类"
 * 正是最容易漏、又最难发现的地方。
 */
bool isSplittable(NodeKind kind) {
    switch (kind) {
        case NodeKind::Document:
        case NodeKind::Section:
        case NodeKind::HeaderFooter:
        case NodeKind::Quote:
        case NodeKind::CodeBlock:
        case NodeKind::ListItem:
        case NodeKind::TextRun:
        case NodeKind::ParagraphMark:
        case NodeKind::Image:
        case NodeKind::Formula:
        case NodeKind::Break:
        case NodeKind::PageBreak:
        case NodeKind::Tab:
        case NodeKind::Field:
            return true;
        case NodeKind::Table:
        case NodeKind::TableRow:
        case NodeKind::TableCell:
        case NodeKind::ContentControl:
        case NodeKind::Link:
            return false;
    }
    return true;
}

/**
 * 找 pos 所属的最外层不可半分单位。找到就返回它的 id 与流位置区间，没找到返回无效 id。
 *
 * 自顶向下找第一个"严格包含 pos 且不可半分"的节点就是最外层 —— 区间是嵌套的，先遇上的
 * 那个一定在最外面。遇到可半分的容器就继续下钻；下钻到文本里说明 pos 是个普通的位置。
 */
NodeId unitAt(const NodeTree& tree, const FlowIndex& idx, NodeId id, FlowPos pos, int* first,
              int* end) {
    const Node* n = tree.find(id);
    if (n == nullptr || !isContainer(n->kind)) return kInvalidNodeId;
    for (const Node* c : n->children) {
        int f = 0;
        int e = 0;
        if (!idx.intervalOf(c->id, &f, &e)) continue;
        if (!(pos > f && pos < e)) continue;
        if (!isSplittable(c->kind)) {
            *first = f;
            *end = e;
            return c->id;
        }
        return unitAt(tree, idx, c->id, pos, first, end);
    }
    return kInvalidNodeId;
}

}  // namespace

Range normalizeRange(const NodeTree& tree, Range range) {
    if (range.empty()) return range;

    const FlowIndex idx(tree, FlowVisibility::Design);
    int f = 0;
    int e = 0;

    const FlowPos begin = range.begin();
    if (unitAt(tree, idx, tree.rootId(), begin, &f, &e) != kInvalidNodeId) {
        if (range.anchor == begin) {
            range.anchor = f;
        } else {
            range.focus = f;
        }
    }

    const FlowPos end = range.end();
    if (unitAt(tree, idx, tree.rootId(), end, &f, &e) != kInvalidNodeId) {
        if (range.anchor == end) {
            range.anchor = e;
        } else {
            range.focus = e;
        }
    }
    return range;
}

}  // namespace heditor
