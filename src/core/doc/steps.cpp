#include "core/doc/steps.h"

#include <algorithm>

namespace heditor {

namespace {

int indexInParent(const Node* n) {
    if (n == nullptr || n->parent == nullptr) return -1;
    const std::vector<Node*>& kids = n->parent->children;
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i] == n) return static_cast<int>(i);
    }
    return -1;
}

FlowIndex indexOf(const NodeTree& tree) { return FlowIndex(tree, FlowVisibility::Design); }

bool inRange(FlowPos pos, int size) { return pos >= 0 && pos <= size; }

/**
 * 把一段码点插到流位置 at 上。
 *
 * 标记与紧邻的文本节点相同就并入它：插入不该平白造出一个"同标记的邻居"，那正是规范化要合并
 * 掉的形状；并入也让"插入之后撤销"能精确回到原样。标记不同才新建节点，而落在文本中间时还要
 * 把那个节点切开 —— 否则新内容会整段跑到原有文本的一侧去。
 */
void insertTextAt(NodeTree& tree, const FlowIndex& idx, FlowPos at,
                  const std::vector<uint32_t>& cps, const CharFormat& marks,
                  const std::string& style_ref) {
    const Pos p = idx.posAt(at);
    const Node* n = tree.find(p.container);
    if (n == nullptr) return;

    if (isText(n->kind)) {
        if (n->marks == marks && n->styleRef == style_ref) {
            std::vector<uint32_t> grown = n->text;
            grown.insert(grown.begin() + p.offset, cps.begin(), cps.end());
            tree.setText(n->id, std::move(grown));
            return;
        }
        const NodeId parent = n->parent->id;
        const int ix = indexInParent(n);
        const bool has_left = p.offset > 0;
        const bool has_right = p.offset < static_cast<int>(n->text.size());
        if (has_left && has_right) {
            const std::vector<uint32_t> right(n->text.begin() + p.offset, n->text.end());
            const std::vector<uint32_t> left(n->text.begin(), n->text.begin() + p.offset);
            const CharFormat kept = n->marks;
            const std::string kept_style = n->styleRef;
            tree.setText(n->id, left);
            const NodeId tail = tree.insertChild(parent, ix + 1, NodeKind::TextRun);
            tree.setText(tail, right);
            tree.find(tail)->marks = kept;
            tree.find(tail)->styleRef = kept_style;
        }
        const NodeId fresh = tree.insertChild(parent, ix + (has_left ? 1 : 0), NodeKind::TextRun);
        tree.setText(fresh, cps);
        tree.find(fresh)->marks = marks;
        return;
    }

    if (isContainer(n->kind)) {
        // 先试前一个孩子（插在末尾那种输入），再试后一个孩子（插在开头那种）。
        if (p.offset > 0) {
            Node* prev = n->children[static_cast<size_t>(p.offset) - 1];
            if (isText(prev->kind) && prev->marks == marks) {
                std::vector<uint32_t> grown = prev->text;
                grown.insert(grown.end(), cps.begin(), cps.end());
                tree.setText(prev->id, std::move(grown));
                return;
            }
        }
        if (p.offset < static_cast<int>(n->children.size())) {
            Node* next = n->children[static_cast<size_t>(p.offset)];
            if (isText(next->kind) && next->marks == marks) {
                std::vector<uint32_t> grown = cps;
                grown.insert(grown.end(), next->text.begin(), next->text.end());
                tree.setText(next->id, std::move(grown));
                return;
            }
        }
        const NodeId fresh = tree.insertChild(p.container, p.offset, NodeKind::TextRun);
        tree.setText(fresh, cps);
        tree.find(fresh)->marks = marks;
        tree.find(fresh)->styleRef = style_ref;
        return;
    }

    // 落在段落标记或原子之前：插在它前面。标记与原子只占一个流位置，只有"之前"这一个位置。
    const NodeId parent = n->parent->id;
    const NodeId fresh = tree.insertChild(parent, indexInParent(n), NodeKind::TextRun);
    tree.setText(fresh, cps);
    tree.find(fresh)->marks = marks;
    tree.find(fresh)->styleRef = style_ref;
}

/**
 * 把一个段落标记插到流位置 at 上。落在文本中间时先把文本切开，标记插在两半之间。
 */
void insertParaMarkAt(NodeTree& tree, const FlowIndex& idx, FlowPos at, const ParaFormatPatch& para,
                      const std::string& style_ref) {
    const Pos p = idx.posAt(at);
    const Node* n = tree.find(p.container);
    if (n == nullptr) return;

    NodeId parent = kInvalidNodeId;
    int ix = 0;
    if (isText(n->kind)) {
        parent = n->parent->id;
        ix = indexInParent(n);
        if (p.offset > 0 && p.offset < static_cast<int>(n->text.size())) {
            const std::vector<uint32_t> right(n->text.begin() + p.offset, n->text.end());
            const std::vector<uint32_t> left(n->text.begin(), n->text.begin() + p.offset);
            const CharFormat kept = n->marks;
            const std::string kept_style = n->styleRef;
            tree.setText(n->id, left);
            const NodeId tail = tree.insertChild(parent, ix + 1, NodeKind::TextRun);
            tree.setText(tail, right);
            tree.find(tail)->marks = kept;
            tree.find(tail)->styleRef = kept_style;
            // 切开之后右半在 ix + 1，标记要插在两半之间。
            ++ix;
        } else if (p.offset > 0) {
            ++ix;
        }
    } else if (isContainer(n->kind)) {
        parent = p.container;
        ix = p.offset;
    } else {
        parent = n->parent->id;
        ix = indexInParent(n);
    }

    const NodeId mark = tree.insertChild(parent, ix, NodeKind::ParagraphMark);
    if (mark == kInvalidNodeId) return;
    tree.find(mark)->para = para;
    tree.find(mark)->styleRef = style_ref;
}

/**
 * 删掉流区间 [begin, end)。
 *
 * 覆盖到的是同一层孩子，且都是文本或段落标记（构造删除步骤时已经验过）。两端被部分覆盖的
 * 文本节点截断，中间被完整覆盖的整个删掉。截断先做（它不改孩子下标），删除从后往前做
 * （从后往前删，前面的下标才不会失效）。
 */
bool eraseRange(NodeTree& tree, FlowPos begin, FlowPos end, std::string* error) {
    if (begin >= end) return true;
    std::vector<Cover> covers;
    std::string err;
    if (!collectCovers(tree, Range{begin, end}, &covers, &err)) {
        if (error != nullptr) *error = err;
        return false;
    }

    std::vector<NodeId> fully;
    for (const Cover& c : covers) {
        const Node* n = tree.find(c.node);
        if (n == nullptr) continue;
        const int span = isText(n->kind) ? static_cast<int>(n->text.size()) : 1;
        if (c.from == c.first && c.to == c.first + span) {
            fully.push_back(c.node);
            continue;
        }
        // 部分覆盖只可能是文本：段落标记与原子各占一个流位置，要么整覆盖要么不覆盖。
        const std::vector<uint32_t>& src = n->text;
        std::vector<uint32_t> kept(src.begin(), src.begin() + (c.from - c.first));
        kept.insert(kept.end(), src.begin() + (c.to - c.first), src.end());
        tree.setText(c.node, std::move(kept));
    }

    for (auto it = fully.rbegin(); it != fully.rend(); ++it) {
        const Node* n = tree.find(*it);
        const int ix = indexInParent(n);
        if (ix < 0) continue;
        tree.eraseChild(n->parent->id, ix);
    }
    return true;
}

template <typename... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};
template <typename... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

/**
 * 读一个控件属性。目标不是内容控件时返回 false。
 */
bool readControlProp(const NodeTree& tree, NodeId id, ControlProp prop, bool* out) {
    const Node* n = tree.find(id);
    if (n == nullptr || n->kind != NodeKind::ContentControl) return false;
    switch (prop) {
        case ControlProp::Hidden:
            *out = n->hidden;
            return true;
        case ControlProp::Readonly:
            *out = n->control.readonly;
            return true;
        case ControlProp::Required:
            *out = n->control.required;
            return true;
        case ControlProp::StructureLock:
            *out = n->control.structureLock;
            return true;
    }
    return false;
}

/**
 * 写一个控件属性。目标不是内容控件时返回 false。
 */
bool writeControlProp(NodeTree& tree, NodeId id, ControlProp prop, bool value) {
    Node* n = tree.find(id);
    if (n == nullptr || n->kind != NodeKind::ContentControl) return false;
    switch (prop) {
        case ControlProp::Hidden:
            n->hidden = value;
            return true;
        case ControlProp::Readonly:
            n->control.readonly = value;
            return true;
        case ControlProp::Required:
            n->control.required = value;
            return true;
        case ControlProp::StructureLock:
            n->control.structureLock = value;
            return true;
    }
    return false;
}

/**
 * 读一个控件属性侧的值。目标不是内容控件时返回 false。
 */
bool readControlValue(const NodeTree& tree, NodeId id, ControlValue* out) {
    const Node* n = tree.find(id);
    if (n == nullptr || n->kind != NodeKind::ContentControl) return false;
    *out = n->control.value;
    return true;
}

/**
 * 写一个控件属性侧的值。目标不是内容控件时返回 false。
 */
bool writeControlValue(NodeTree& tree, NodeId id, const ControlValue& value) {
    Node* n = tree.find(id);
    if (n == nullptr || n->kind != NodeKind::ContentControl) return false;
    n->control.value = value;
    return true;
}

}  // namespace

namespace {

/**
 * 把流位置按一串步骤的映射推过去。表格式与批量设值都靠它：每步的坐标属于它自己那一刻。
 */
FlowPos throughSteps(const std::vector<Step>& prev, FlowPos pos) {
    for (const Step& s : prev) pos = mapPos(s, pos, Assoc::Before);
    return pos;
}

}  // namespace

void pushMappedStep(std::vector<Step>* out, Step step) {
    if (auto* ins = std::get_if<InsertNode>(&step)) {
        ins->at_flow = throughSteps(*out, ins->at_flow);
    } else if (auto* rm = std::get_if<RemoveNode>(&step)) {
        rm->at_flow = throughSteps(*out, rm->at_flow);
    }
    out->push_back(std::move(step));
}

DeleteBuild makeDeleteRange(const NodeTree& tree, Range range) {
    DeleteBuild out;
    const ExtractResult ex = extractRange(tree, range);
    if (!ex.ok) {
        out.error = ex.error;
        return out;
    }
    const Range norm = normalizeRange(tree, range);
    DeleteRange s;
    s.begin = norm.begin();
    s.end = norm.end();
    s.removed = ex.fragment;
    out.step = Step{s};
    out.ok = true;
    return out;
}

ParaMarkBuild makeRemoveParaMark(const NodeTree& tree, FlowPos at) {
    ParaMarkBuild out;
    const FlowIndex idx(tree, FlowVisibility::Design);
    if (at < 0 || at >= idx.size()) {
        out.error = "删除段落标记：流位置越界";
        return out;
    }
    const Pos p = idx.posAt(at);
    const Node* n = tree.find(p.container);
    if (n == nullptr || !isParagraphMark(n->kind) || p.offset != 0) {
        out.error = "删除段落标记：该位置上没有段落标记";
        return out;
    }
    RemoveParaMark s;
    s.at = at;
    s.para = n->para;
    s.styleRef = n->styleRef;
    out.step = Step{s};
    out.ok = true;
    return out;
}

NodeBuild makeInsertNode(NodeTree& tree, const Pos& at, const NodeSpec& spec) {
    NodeBuild out;
    const Node* c = tree.find(at.container);
    if (c == nullptr || !isContainer(c->kind)) {
        out.error = "插入节点：目标不是容器";
        return out;
    }
    if (at.offset < 0 || at.offset > static_cast<int>(c->children.size())) {
        out.error = "插入节点：槽位越界";
        return out;
    }
    if (!acceptsChild(c->kind, spec.kind)) {
        out.error = std::string("插入节点：") + nodeKindName(c->kind) + " 不接受 " +
                    nodeKindName(spec.kind) + " 作为子节点";
        return out;
    }
    const NodeId detached = materializeSubtree(tree, kInvalidNodeId, 0, spec);
    if (detached == kInvalidNodeId) {
        out.error = "插入节点：子树建不出来";
        return out;
    }

    InsertNode s;
    s.at = at;
    // 摘下的子树不在流里，所以这个流位置与物化前后都一致。
    s.at_flow = indexOf(tree).flowAt(at);
    s.subtree = spec;
    s.detached = detached;
    out.step = Step{s};
    out.ok = true;
    return out;
}

NodeBuild makeRemoveNode(const NodeTree& tree, const Pos& at) {
    NodeBuild out;
    const Node* c = tree.find(at.container);
    if (c == nullptr || !isContainer(c->kind)) {
        out.error = "删除节点：目标不是容器";
        return out;
    }
    if (at.offset < 0 || at.offset >= static_cast<int>(c->children.size())) {
        out.error = "删除节点：槽位没有孩子";
        return out;
    }
    const Node* kid = c->children[static_cast<size_t>(at.offset)];

    RemoveNode s;
    s.at = at;
    s.at_flow = indexOf(tree).flowAt(at);
    s.removed = captureSubtree(tree, kid->id);
    s.detached = kid->id;
    out.step = Step{s};
    out.ok = true;
    return out;
}

StepResult canApply(const NodeTree& tree, const Step& step) {
    const FlowIndex idx = indexOf(tree);
    const int size = idx.size();
    StepResult r;
    std::visit(Overloaded{
                   [&](const InsertText& s) {
                       if (!inRange(s.at, size)) {
                           r.error = "插入位置越界";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const InsertFragment& s) {
                       if (!inRange(s.at, size)) {
                           r.error = "插入位置越界";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const DeleteRange& s) {
                       if (s.begin > s.end || !inRange(s.begin, size) || !inRange(s.end, size)) {
                           r.error = "删除区间越界";
                           return;
                       }
                       std::vector<Cover> covers;
                       std::string err;
                       if (!collectCovers(tree, Range{s.begin, s.end}, &covers, &err)) {
                           r.error = err;
                           return;
                       }
                       int span = 0;
                       for (const Cover& c : covers) span += c.to - c.from;
                       if (span != s.end - s.begin) {
                           r.error = "删除区间与文档对不上（步骤过期了）";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const InsertParaMark& s) {
                       if (!inRange(s.at, size)) {
                           r.error = "分段位置越界";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const RemoveParaMark& s) {
                       if (!inRange(s.at, size)) {
                           r.error = "取消分段的位置越界";
                           return;
                       }
                       const Pos p = idx.posAt(s.at);
                       const Node* n = tree.find(p.container);
                       if (n == nullptr || !isParagraphMark(n->kind) || p.offset != 0) {
                           r.error = "该位置上没有段落标记";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const InsertNode& s) {
                       const Node* c = tree.find(s.at.container);
                       if (c == nullptr || !isContainer(c->kind)) {
                           r.error = "插入节点：目标不是容器";
                           return;
                       }
                       if (s.at.offset < 0 || s.at.offset > static_cast<int>(c->children.size())) {
                           r.error = "插入节点：槽位越界";
                           return;
                       }
                       if (!acceptsChild(c->kind, s.subtree.kind)) {
                           r.error = std::string("插入节点：") + nodeKindName(c->kind) +
                                     " 不接受 " + nodeKindName(s.subtree.kind) + " 作为子节点";
                           return;
                       }
                       if (idx.flowAt(s.at) != s.at_flow) {
                           r.error = "插入节点：流位置与文档对不上（步骤过期了）";
                           return;
                       }
                       if (s.detached != kInvalidNodeId) {
                           const Node* d = tree.find(s.detached);
                           if (d == nullptr) {
                               r.error = "插入节点：要挂回的子树不在池里";
                               return;
                           }
                           // 已经挂在树上说明这一步被应用过却没撤销过，再挂一次会拆散父子链。
                           if (d->parent != nullptr) {
                               r.error = "插入节点：要挂回的子树已经挂在树上了";
                               return;
                           }
                           if (d->kind != s.subtree.kind) {
                               r.error = "插入节点：待挂子树的种类与步骤不符";
                               return;
                           }
                       }
                       r.ok = true;
                   },
                   [&](const RemoveNode& s) {
                       const Node* c = tree.find(s.at.container);
                       if (c == nullptr || !isContainer(c->kind)) {
                           r.error = "删除节点：目标不是容器";
                           return;
                       }
                       if (s.at.offset < 0 || s.at.offset >= static_cast<int>(c->children.size())) {
                           r.error = "删除节点：槽位没有孩子";
                           return;
                       }
                       const Node* kid = c->children[static_cast<size_t>(s.at.offset)];
                       if (kid->kind != s.removed.kind) {
                           r.error = "删除节点：该位置的孩子与步骤记录不符";
                           return;
                       }
                       if (s.detached != kInvalidNodeId && kid->id != s.detached) {
                           r.error = "删除节点：该位置的孩子的身份与步骤不符";
                           return;
                       }
                       if (idx.flowAt(s.at) != s.at_flow) {
                           r.error = "删除节点：流位置与文档对不上（步骤过期了）";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const SetControlProp& s) {
                       bool now = false;
                       if (!readControlProp(tree, s.id, s.prop, &now)) {
                           r.error = "改属性：目标不是内容控件";
                           return;
                       }
                       // 记的原值要与文档当前值一致，否则这一步是与别的编辑抢同一个属性
                       // （或者步骤被重放到了别的文档上）。拒绝比改错强。
                       if (now != s.before) {
                           r.error = std::string("改属性：") + controlPropName(s.prop) +
                                     " 的当前值与步骤记的原值不符（步骤过期了）";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const SetControlValue& s) {
                       const Node* n = tree.find(s.id);
                       if (n == nullptr || n->kind != NodeKind::ContentControl) {
                           r.error = "改值：目标不是内容控件";
                           return;
                       }
                       // 文本类的值在内容里，走内容级步骤。在这一层改它的 value 字段等于改一个
                       // 取值时不会被读到的位置，看起来成功了、实际什么都没变。
                       if (n->control.valueShape == ValueShape::Text) {
                           r.error = "改值：文本类的值在内容里，不走属性侧的值步骤";
                           return;
                       }
                       ControlValue now;
                       if (!readControlValue(tree, s.id, &now)) {
                           r.error = "改值：目标不是内容控件";
                           return;
                       }
                       if (!(now == s.before)) {
                           r.error = "改值：当前值与步骤记的原值不符（步骤过期了）";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const SetTableCellSpan& s) {
                       const Node* n = tree.find(s.id);
                       if (n == nullptr || n->kind != NodeKind::TableCell) {
                           r.error = "改跨度：目标不是单元格";
                           return;
                       }
                       // 0 或负数不是可表达的跨度：真放过去，派生网格会把这一行算短，
                       // 而"每行跨度之和相等"那条不变量要到下一次检查才报出来。
                       if (s.span.row_span < 1 || s.span.col_span < 1) {
                           r.error = "改跨度：跨度至少是 1";
                           return;
                       }
                       if (!(n->cell == s.before)) {
                           r.error = "改跨度：当前跨度与步骤记的原值不符（步骤过期了）";
                           return;
                       }
                       r.ok = true;
                   },
               },
               step);
    return r;
}

StepResult applyStep(NodeTree& tree, const Step& step) {
    StepResult r = canApply(tree, step);
    if (!r.ok) return r;

    const FlowIndex idx = indexOf(tree);
    std::visit(
        Overloaded{
            [&](const InsertText& s) { insertTextAt(tree, idx, s.at, s.cps, s.marks, s.styleRef); },
            [&](const InsertFragment& s) {
                FlowPos pos = s.at;
                for (const Piece& p : s.content.pieces) {
                    const FlowIndex here = indexOf(tree);
                    if (p.kind == PieceKind::ParaMark) {
                        insertParaMarkAt(tree, here, pos, p.para, p.styleRef);
                    } else {
                        insertTextAt(tree, here, pos, p.cps, p.marks, p.styleRef);
                    }
                    pos += p.span();
                }
            },
            [&](const DeleteRange& s) {
                std::string err;
                eraseRange(tree, s.begin, s.end, &err);
            },
            [&](const InsertParaMark& s) { insertParaMarkAt(tree, idx, s.at, s.para, s.styleRef); },
            [&](const RemoveParaMark& s) {
                const Pos p = idx.posAt(s.at);
                const Node* n = tree.find(p.container);
                tree.eraseChild(n->parent->id, indexInParent(n));
            },
            [&](const InsertNode& s) {
                if (s.detached != kInvalidNodeId) {
                    tree.attachChild(s.at.container, s.at.offset, s.detached);
                    return;
                }
                // 没有现成的子树（例如步骤由序列化还原）：按记录现建一棵，内容一样、
                // 身份是新的。
                materializeSubtree(tree, s.at.container, s.at.offset, s.subtree);
            },
            [&](const RemoveNode& s) { tree.eraseChild(s.at.container, s.at.offset); },
            [&](const SetControlProp& s) { writeControlProp(tree, s.id, s.prop, s.value); },
            [&](const SetControlValue& s) { writeControlValue(tree, s.id, s.value); },
            [&](const SetTableCellSpan& s) {
                Node* n = tree.find(s.id);
                if (n != nullptr) n->cell = s.span;
            },
        },
        step);
    return r;
}

Step invertStep(const Step& step) {
    return std::visit(Overloaded{
                          [](const InsertText& s) -> Step {
                              DeleteRange d;
                              d.begin = s.at;
                              d.end = s.at + static_cast<FlowPos>(s.cps.size());
                              Piece piece;
                              piece.kind = PieceKind::Text;
                              piece.cps = s.cps;
                              piece.marks = s.marks;
                              piece.styleRef = s.styleRef;
                              d.removed.pieces.push_back(std::move(piece));
                              return Step{d};
                          },
                          [](const InsertFragment& s) -> Step {
                              DeleteRange d;
                              d.begin = s.at;
                              d.end = s.at + s.content.span();
                              d.removed = s.content;
                              return Step{d};
                          },
                          [](const DeleteRange& s) -> Step {
                              InsertFragment f;
                              f.at = s.begin;
                              f.content = s.removed;
                              return Step{f};
                          },
                          [](const InsertParaMark& s) -> Step {
                              RemoveParaMark x;
                              x.at = s.at;
                              x.para = s.para;
                              x.styleRef = s.styleRef;
                              return Step{x};
                          },
                          [](const RemoveParaMark& s) -> Step {
                              InsertParaMark x;
                              x.at = s.at;
                              x.para = s.para;
                              x.styleRef = s.styleRef;
                              return Step{x};
                          },
                          [](const InsertNode& s) -> Step {
                              RemoveNode d;
                              d.at = s.at;
                              d.at_flow = s.at_flow;
                              d.removed = s.subtree;
                              d.detached = s.detached;
                              return Step{d};
                          },
                          [](const RemoveNode& s) -> Step {
                              InsertNode i;
                              i.at = s.at;
                              i.at_flow = s.at_flow;
                              i.subtree = s.removed;
                              i.detached = s.detached;
                              return Step{i};
                          },
                          [](const SetControlProp& s) -> Step {
                              SetControlProp x;
                              x.id = s.id;
                              x.prop = s.prop;
                              x.value = s.before;
                              x.before = s.value;
                              return Step{x};
                          },
                          [](const SetControlValue& s) -> Step {
                              SetControlValue x;
                              x.id = s.id;
                              x.value = s.before;
                              x.before = s.value;
                              return Step{x};
                          },
                          [](const SetTableCellSpan& s) -> Step {
                              SetTableCellSpan x;
                              x.id = s.id;
                              x.span = s.before;
                              x.before = s.span;
                              return Step{x};
                          },
                      },
                      step);
}

FlowPos mapPos(const Step& step, FlowPos pos, Assoc assoc) {
    return std::visit(Overloaded{
                          [&](const InsertText& s) -> FlowPos {
                              const FlowPos n = static_cast<FlowPos>(s.cps.size());
                              if (pos < s.at) return pos;
                              if (pos > s.at) return pos + n;
                              return assoc == Assoc::After ? s.at + n : s.at;
                          },
                          [&](const InsertFragment& s) -> FlowPos {
                              const FlowPos n = s.content.span();
                              if (pos < s.at) return pos;
                              if (pos > s.at) return pos + n;
                              return assoc == Assoc::After ? s.at + n : s.at;
                          },
                          [&](const DeleteRange& s) -> FlowPos {
                              if (pos <= s.begin) return pos;
                              if (pos >= s.end) return pos - (s.end - s.begin);
                              return s.begin;
                          },
                          [&](const InsertParaMark& s) -> FlowPos {
                              if (pos < s.at) return pos;
                              if (pos > s.at) return pos + 1;
                              return assoc == Assoc::After ? s.at + 1 : s.at;
                          },
                          [&](const RemoveParaMark& s) -> FlowPos {
                              if (pos <= s.at) return pos;
                              if (pos >= s.at + 1) return pos - 1;
                              return s.at;
                          },
                          [&](const InsertNode& s) -> FlowPos {
                              const FlowPos n = static_cast<FlowPos>(s.subtree.span());
                              if (pos < s.at_flow) return pos;
                              if (pos > s.at_flow) return pos + n;
                              return assoc == Assoc::After ? s.at_flow + n : s.at_flow;
                          },
                          [&](const RemoveNode& s) -> FlowPos {
                              const FlowPos n = static_cast<FlowPos>(s.removed.span());
                              if (pos <= s.at_flow) return pos;
                              if (pos >= s.at_flow + n) return pos - n;
                              return s.at_flow;
                          },
                          // 属性不改编辑态的位置。hidden 会改呈现态的流结构，但那要重排
                          // （重建派生索引），不是把某个位置搬到别处。
                          [&](const SetControlProp&) -> FlowPos { return pos; },
                          [&](const SetControlValue&) -> FlowPos { return pos; },
                          // 跨度不改流位置，也不改流结构：它改的是"这一格覆盖多少格位"。
                          [&](const SetTableCellSpan&) -> FlowPos { return pos; },
                      },
                      step);
}

}  // namespace heditor
