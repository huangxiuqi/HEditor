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
 * 流位置 → 它落在哪个孩子槽位上（只读，不改树）。
 *
 * 落进文本节点内部的位置，切开之后会成为"那两半之间"那一格 —— 所以它在孩子槽位上的编号
 * 是"这个文本节点之后"，不是它自己。这里只做这个换算，切分归 applyStep（canApply 不能改树）。
 */
Pos slotForFlow(const NodeTree& tree, const FlowIndex& idx, FlowPos flow) {
    const Pos p = idx.posAt(flow);
    const Node* n = tree.find(p.container);
    if (n == nullptr) return Pos{};
    if (isContainer(n->kind)) return p;
    if (n->parent == nullptr) return Pos{};
    // 文本节点内部（offset > 0）要切开，位置于是落在它的两半之间；offset == 0 时不用切，
    //   位置就在它自己那一格上。原子与段落标记内部没有可寻址的位置，只有"自身之前"。
    const bool split = isText(n->kind) && p.offset > 0;
    return Pos{n->parent->id, indexInParent(n) + (split ? 1 : 0)};
}

/**
 * 一段流区间在一个容器里对应哪些孩子（按切开文本之后的切片算），并给出它们的起始槽位。
 *
 * 内容被整个包在一个容器孩子里时返回 false：那一段的容器该是那个孩子，不是本层 ——
 * 硬包的话会把容器劈成两半，那是模型里说不出来的形状。
 * 被整个包在一个文本孩子里不算错：文本可以切（被规范化合并掉的边界就是这么回来的）。
 */
bool runForFlowRange(const NodeTree& tree, const FlowIndex& idx, NodeId container, FlowPos begin,
                     FlowPos end, std::vector<const Node*>* out, int* start_slot) {
    const Node* c = tree.find(container);
    if (c == nullptr || !isContainer(c->kind)) return false;
    out->clear();
    int first = -1;
    bool split_head = false;
    for (size_t i = 0; i < c->children.size(); ++i) {
        const Node* kid = c->children[i];
        if (!idx.inFlow(kid->id)) continue;
        int kf = 0;
        int ke = 0;
        if (!idx.intervalOf(kid->id, &kf, &ke)) continue;
        if (ke <= begin) continue;
        if (kf >= end) break;
        if (kf < begin && ke > end && isContainer(kid->kind)) return false;
        if (first < 0) {
            first = static_cast<int>(i);
            split_head = (kf < begin);
        }
        out->push_back(kid);
    }
    if (first < 0) return false;
    if (start_slot != nullptr) *start_slot = first + (split_head ? 1 : 0);
    return true;
}

/**
 * 按流位置取一个孩子槽位，必要时先把文本节点从中间切开（会改树）。
 *
 * 为什么非走流位置不可：规范化在每次提交之后跑一遍，它把相邻且同标记的文本节点并成一个 ——
 * 孩子槽位从此整体前移，步骤里记下的下标再也对不上，而流位置是唯一能活过它的坐标。
 * 所以凡是"指向某个孩子"的步骤都要能按流位置把这个槽位重新算出来。
 */
bool slotAtFlow(NodeTree& tree, FlowPos flow, NodeId* container, int* slot) {
    FlowIndex idx = indexOf(tree);
    const Pos p = idx.posAt(flow);
    const Node* n = tree.find(p.container);
    if (n == nullptr) return false;
    if (isText(n->kind) && p.offset > 0 && p.offset < static_cast<int>(n->text.size()) &&
        n->parent != nullptr) {
        const NodeId parent = n->parent->id;
        const int ix = indexInParent(n);
        // 先把两半都取出来再动树：insertChild 会让节点指针失效。
        const std::vector<uint32_t> left(n->text.begin(), n->text.begin() + p.offset);
        const std::vector<uint32_t> right(n->text.begin() + p.offset, n->text.end());
        const CharFormat kept = n->marks;
        const std::string kept_style = n->styleRef;
        tree.setText(n->id, left);
        const NodeId tail = tree.insertChild(parent, ix + 1, NodeKind::TextRun);
        tree.setText(tail, right);
        tree.find(tail)->marks = kept;
        tree.find(tail)->styleRef = kept_style;
        idx = indexOf(tree);  // 树变了，重新算一遍
    }
    const Pos q = slotForFlow(tree, idx, flow);
    if (q.container == kInvalidNodeId) return false;
    if (container != nullptr) *container = q.container;
    if (slot != nullptr) *slot = q.offset;
    return true;
}

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
bool eraseRange(NodeTree& tree, const FlowIndex& idx, FlowPos begin, FlowPos end,
                std::string* error) {
    if (begin >= end) return true;
    std::vector<Cover> covers;
    std::string err;
    if (!collectCovers(tree, idx, Range{begin, end}, &covers, &err)) {
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
 * 两个书写模式是否相同（逐字段比）。
 *
 * 单写一个而不是给 WritingMode 加 operator==：那个结构在文本层，它的字段全是枚举，
 * 默认相等本来是对的，但 std::optional<WritingMode> 的默认相等又要靠它 —— 而
 * std::optional 的默认相等在 MSVC 上被隐式删除时不报错、只在真去比较的地方炸开。
 * 比较写在需要它的这一侧，坏掉的时候一眼能看出是哪一处。
 */
bool sameWritingMode(const WritingMode& a, const WritingMode& b) {
    return a.inline_axis == b.inline_axis && a.block_flow == b.block_flow &&
           a.orientation == b.orientation;
}

/**
 * 两个段落格式 patch 是否逐字段相同。检查器与步骤守卫都要问这个问题。
 */
bool sameParaPatch(const ParaFormatPatch& a, const ParaFormatPatch& b) {
    if (a.font_index != b.font_index || a.size != b.size || a.line_factor != b.line_factor ||
        a.space_after != b.space_after || a.page_break_before != b.page_break_before ||
        a.language != b.language || a.color != b.color ||
        a.fallback_font_indices != b.fallback_font_indices) {
        return false;
    }
    if (a.wm.has_value() != b.wm.has_value()) return false;
    return !a.wm.has_value() || sameWritingMode(*a.wm, *b.wm);
}

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

namespace {

/**
 * 取一段文本区间：与 extractRange 的差别只在"不做端点规范化"。
 *
 * 规范化会把落在不可半分单位内部的端点向外扩到那个单位的边界（"不能被切开"），
 * 那条规则是给按结构取区间的场合用的；编辑删的是码点，起点落在控件内容里时扩到控件开头
 * 意味着"删一个字"变成"删掉整个控件的内容"。所以编辑这条路显式要求调用方给出已经切分好的
 * 区间（严格落在一个文本节点里），并在 makeDeleteTextSteps 的说明里写明这一点。
 */
bool extractTextRange(const NodeTree& tree, const FlowIndex& idx, FlowPos begin, FlowPos end,
                      Fragment* out, std::string* error) {
    std::vector<Cover> covers;
    if (!collectCovers(tree, idx, Range{begin, end}, &covers, error)) return false;
    Fragment frag;
    int span = 0;
    for (const Cover& c : covers) {
        const Node* n = tree.find(c.node);
        if (n == nullptr) continue;
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
        span += c.to - c.from;
        frag.pieces.push_back(std::move(piece));
    }
    if (span != end - begin) {
        if (error != nullptr) *error = "删除文本：区间与文档对不上（步骤过期了）";
        return false;
    }
    *out = std::move(frag);
    return true;
}

}  // namespace

std::vector<Step> makeDeleteTextSteps(const NodeTree& tree, FlowPos from, FlowPos to) {
    std::vector<Step> out;
    if (from < 0 || to <= from) return out;
    const FlowIndex idx = indexOf(tree);
    if (to > idx.size()) return out;

    // 按"所属文本节点"分组：同一节点上的连续位置合成一段。
    //   逐位问 posAt 是 O(位置数 x 扇出) —— posAt 要从根下降到最内层，每层线性扫孩子，
    //   2000 段的文档每次约 O(段数)，整篇删除就是亿级。改为逐节点跳跃：拿到当前节点后
    //   直接跳到它的流末端，下一个位置必然落在别的节点上（节点区间两两不重叠）。
    std::vector<std::pair<int, int>> segs;
    FlowPos p = from;
    while (p < to) {
        const Pos np = idx.posAt(p);
        const Node* n = tree.find(np.container);
        int nf = 0;
        int ne = 0;
        if (n == nullptr || !isText(n->kind) || !idx.intervalOf(n->id, &nf, &ne)) {
            ++p;  // 容器边界、段落标记、原子：这一位不产生步骤
            continue;
        }
        const int seg_to = (ne < to) ? ne : to;
        if (seg_to > p) segs.push_back({p, seg_to});
        p = ne;  // 跳过整个文本节点
    }

    // 从后往前发步：删右边的不会移动左边的坐标，每步的坐标就是它自己那一刻的坐标。
    out.reserve(segs.size());
    for (size_t i = segs.size(); i-- > 0;) {
        DeleteRange s;
        s.begin = segs[i].first;
        s.end = segs[i].second;
        if (!extractTextRange(tree, idx, s.begin, s.end, &s.removed, nullptr)) return {};
        out.push_back(Step{s});
    }
    return out;
}

std::vector<Step> makeDeleteRangeOneStep(const NodeTree& tree, const FlowIndex& idx,
                                         FlowPos from, FlowPos to) {
    std::vector<Step> out;
    if (from < 0 || to <= from || to > idx.size()) return out;
    Fragment frag;
    std::string err;
    // 区间里出现文本与段落标记之外的东西（控件、原子、容器边界）时它会失败，
    //   调用方据此退回"按节点切分"那条路。
    if (!extractTextRange(tree, idx, from, to, &frag, &err)) return out;
    DeleteRange s;
    s.begin = from;
    s.end = to;
    s.removed = std::move(frag);
    out.push_back(Step{s});
    return out;
}

ParaMarkBuild makeRemoveParaMark(const NodeTree& tree, FlowPos at) {
    return makeRemoveParaMark(tree, FlowIndex(tree, FlowVisibility::Design), at);
}

ParaMarkBuild makeRemoveParaMark(const NodeTree& tree, const FlowIndex& idx, FlowPos at) {
    ParaMarkBuild out;
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

NodeBuild makeRemoveAtom(const NodeTree& tree, const FlowIndex& idx, FlowPos at) {
    NodeBuild out;
    if (!inRange(at, idx.size())) {
        out.error = "删除原子：流位置越界";
        return out;
    }
    // 原子是叶子，它自己的起点就是它在流里唯一可寻址的位置（posAt 对叶子把"流位置 == 叶子
    //   起点"归给叶子自己），所以这里必然拿到原子本身、且段内偏移为 0。
    const Pos p = idx.posAt(at);
    const Node* n = tree.find(p.container);
    if (n == nullptr || !isAtom(n->kind)) {
        out.error = "删除原子：该位置上没有原子";
        return out;
    }
    if (p.offset != 0) {
        out.error = "删除原子：位置落在原子内部（原子没有可寻址的内部）";
        return out;
    }
    const Pos slot = slotForFlow(tree, idx, at);
    if (slot.container == kInvalidNodeId) {
        out.error = "删除原子：算不出它所在的槽位";
        return out;
    }
    RemoveNode s;
    s.at = slot;
    s.at_flow = at;
    s.removed = captureSubtree(tree, n->id);
    s.detached = n->id;
    out.step = Step{s};
    out.ok = true;
    return out;
}

StepResult canApply(const NodeTree& tree, const Step& step) {
    return canApply(tree, indexOf(tree), step);
}
StepResult canApply(const NodeTree& tree, const FlowIndex& idx, const Step& step) {
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
                       if (!collectCovers(tree, idx, Range{s.begin, s.end}, &covers, &err)) {
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
                   [&](const SetParaMarkFormat& s) {
                       const Node* n = tree.find(s.id);
                       if (n == nullptr || !isParagraphMark(n->kind)) {
                           r.error = "改段落格式：目标不是段落标记";
                           return;
                       }
                       // 记的原值要与文档当前值一致，否则这一步是在与别的编辑抢同一枚标记
                       // （或者步骤被重放到了别的文档上）。拒绝比改错强。
                       if (!sameParaPatch(n->para, s.before) || n->styleRef != s.beforeStyleRef) {
                           r.error = "改段落格式：当前值与步骤记的原值不符（步骤过期了）";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const SetRunMarks& s) {
                       const Node* n = tree.find(s.id);
                       if (n == nullptr || n->kind != NodeKind::TextRun) {
                           r.error = "改字符格式：目标不是文本节点";
                           return;
                       }
                       // 与段落格式同一条守卫：记的原值要与文档当前值一致，否则这一步
                       //   是在与别的编辑抢同一个节点（或者被重放到了别的文档上）。
                       if (!(n->marks == s.before) || n->styleRef != s.beforeStyleRef) {
                           r.error = "改字符格式：当前值与步骤记的原值不符（步骤过期了）";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const InsertNode& s) {
                       // 判据落在流位置上：槽位下标活不过一次规范化（相邻同标记的文本节点
                       //   会被并成一个，后面的槽位整体前移）—— 与 UnwrapNode 同一条。
                       if (!inRange(s.at_flow, size)) {
                           r.error = "插入节点：流位置与文档对不上（越界）";
                           return;
                       }
                       const Pos p = slotForFlow(tree, idx, s.at_flow);
                       const Node* c = tree.find(p.container);
                       if (c == nullptr || !isContainer(c->kind)) {
                           r.error = "插入节点：目标不是容器";
                           return;
                       }
                       if (!acceptsChild(c->kind, s.subtree.kind)) {
                           r.error = std::string("插入节点：") + nodeKindName(c->kind) +
                                     " 不接受 " + nodeKindName(s.subtree.kind) + " 作为子节点";
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
                       // 与 UnwrapNode 同一条：槽位下标活不过一次规范化，判据落在流位置上 ——
                       //   那一格上必须站着一个与记录同种（同身份）的节点。
                       //   槽位用 slotForFlow 而不是 posAt 取：posAt 会下降进叶子孩子
                       //   （"流位置等于叶子的起点"归叶子自己），要删的正是那个叶子。
                       if (!inRange(s.at_flow, size)) {
                           r.error = "删除节点：流位置与文档对不上（越界）";
                           return;
                       }
                       const Pos p = slotForFlow(tree, idx, s.at_flow);
                       const Node* c = tree.find(p.container);
                       if (c == nullptr || !isContainer(c->kind)) {
                           r.error = "删除节点：目标不是容器";
                           return;
                       }
                       if (p.offset < 0 || p.offset >= static_cast<int>(c->children.size())) {
                           r.error = "删除节点：槽位没有孩子";
                           return;
                       }
                       const Node* kid = c->children[static_cast<size_t>(p.offset)];
                       if (kid->kind != s.removed.kind) {
                           r.error = "删除节点：该位置的孩子与步骤记录不符";
                           return;
                       }
                       if (s.detached != kInvalidNodeId && kid->id != s.detached) {
                           r.error = "删除节点：该位置的孩子的身份与步骤不符";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const UnwrapNode& s) {
                       // 槽位下标活不过一次规范化（相邻同标记文本节点会被合并，后面的槽位整体
                       //   前移），所以判据落在流位置上：那一格上必须站着一个与记录同种的节点。
                       const Pos p = idx.posAt(s.at_flow);
                       const Node* c = tree.find(p.container);
                       if (c == nullptr || !isContainer(c->kind)) {
                           r.error = "拆开节点：目标不是容器";
                           return;
                       }
                       if (p.container != s.at.container) {
                           // 流位置落进了文本节点内部 —— 那儿不可能站着一个容器：说明这个流
                           //  位置上要拆的节点已经不在树上了（步骤过期，或已经应用过）。
                           r.error = "拆开节点：那个流位置上没有容器（步骤过期了）";
                           return;
                       }
                       if (p.offset < 0 || p.offset >= static_cast<int>(c->children.size())) {
                           r.error = "拆开节点：槽位没有孩子";
                           return;
                       }
                       const Node* node = c->children[static_cast<size_t>(p.offset)];
                       if (node->kind != s.node.kind) {
                           r.error = "拆开节点：该位置的孩子与步骤记录不符";
                           return;
                       }
                       if (s.detached != kInvalidNodeId && node->id != s.detached) {
                           r.error = "拆开节点：该位置的孩子的身份与步骤不符";
                           return;
                       }
                       // 上提之后每个孩子都要落在父容器里 —— 父容器可能不接受它的种类。
                       //   先查再动手：半途拒绝会留下"孩子已经搬了一半"的树。
                       for (const Node* kid : node->children) {
                           if (!acceptsChild(c->kind, kid->kind)) {
                               r.error = std::string("拆开节点：") + nodeKindName(c->kind) +
                                         " 不能直接容纳 " + nodeKindName(kid->kind);
                               return;
                           }
                       }
                       r.ok = true;
                   },
                   [&](const WrapNode& s) {
                       // 两端的槽位不能各算各的：规范化把整段文字并成一个文本节点之后，
                       //   两个端点会落在同一个孩子上，各自换算都得到同一个槽位（个数就成了 0）。
                       //   所以按"这一段流区间在容器里对应哪些切片"一次性算出来。
                       if (s.span_before < 2) {
                           r.error = "包起节点：区间太窄，装不下一个容器";
                           return;
                       }
                       const Pos a = slotForFlow(tree, idx, s.at_flow);
                       const Node* c = tree.find(a.container);
                       if (c == nullptr || !isContainer(c->kind)) {
                           r.error = "包起节点：目标不是容器";
                           return;
                       }
                       std::vector<const Node*> run;
                       int start_slot = -1;
                       // 区间取 [at_flow, at_flow + span_after)：这是这一步所在状态里的地址，
                       //   也就是拆开之后（少了那两枚边界）的地址。span_before 是拆之前节点
                       //   的跨度，只在 mapPos 与校验宽度时用。
                       if (!runForFlowRange(tree, idx, a.container, s.at_flow,
                                            s.at_flow + s.span_after, &run, &start_slot)) {
                           r.error = "包起节点：这段流区间横跨了容器边界";
                           return;
                       }
                       if (run.size() != s.node.children.size()) {
                           r.error = "包起节点：槽位里有 " + std::to_string(run.size()) +
                                     " 个孩子，步骤记的是 " +
                                     std::to_string(s.node.children.size());
                           return;
                       }
                       for (size_t k = 0; k < run.size(); ++k) {
                           if (run[k]->kind != s.node.children[k].kind) {
                               r.error = "包起节点：槽位里的孩子与步骤记录不符";
                               return;
                           }
                       }
                       if (!acceptsChild(c->kind, s.node.kind)) {
                           r.error = std::string("包起节点：") + nodeKindName(c->kind) +
                                     " 不接受 " + nodeKindName(s.node.kind);
                           return;
                       }
                       if (s.detached == kInvalidNodeId || tree.find(s.detached) == nullptr) {
                           r.error = "包起节点：要挂回的节点不在池里";
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
    // 判据与应用共用同一份流索引：两边各建一次等于把每步的成本翻倍，
    //   而一批四千步的删除里那就是几千次全树遍历。
    const FlowIndex idx = indexOf(tree);
    StepResult r = canApply(tree, idx, step);
    if (!r.ok) return r;

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
                eraseRange(tree, idx, s.begin, s.end, &err);
            },
            [&](const InsertParaMark& s) { insertParaMarkAt(tree, idx, s.at, s.para, s.styleRef); },
            [&](const RemoveParaMark& s) {
                const Pos p = idx.posAt(s.at);
                const Node* n = tree.find(p.container);
                tree.eraseChild(n->parent->id, indexInParent(n));
            },
            [&](const SetParaMarkFormat& s) {
                Node* n = tree.find(s.id);
                if (n == nullptr) return;
                n->para = s.para;
                n->styleRef = s.styleRef;
            },
            [&](const SetRunMarks& s) {
                Node* n = tree.find(s.id);
                if (n == nullptr) return;
                n->marks = s.marks;
                n->styleRef = s.styleRef;
            },
            [&](const InsertNode& s) {
                // 槽位按流位置当场重算，必要时把文本节点切开：撤销"删掉一个节点"时，那两段
                //   相邻的文字已被规范化并成一个节点，步骤里记下的槽位下标不再指向它
                //   （见 UnwrapNode / WrapNode 同一条理由）。
                NodeId container = kInvalidNodeId;
                int slot = -1;
                if (!slotAtFlow(tree, s.at_flow, &container, &slot)) return;
                if (s.detached != kInvalidNodeId) {
                    tree.attachChild(container, slot, s.detached);
                    return;
                }
                // 没有现成的子树（例如步骤由序列化还原）：按记录现建一棵，内容一样、
                // 身份是新的。
                materializeSubtree(tree, container, slot, s.subtree);
            },
            [&](const RemoveNode& s) {
                NodeId container = kInvalidNodeId;
                int slot = -1;
                if (!slotAtFlow(tree, s.at_flow, &container, &slot)) return;
                tree.eraseChild(container, slot);
            },
            [&](const UnwrapNode& s) {
                // 槽位按流位置当场重算：参数里那个 at.offset 只是记录，规范化的合并会让它过期。
                NodeId container = kInvalidNodeId;
                int slot = -1;
                if (!slotAtFlow(tree, s.at_flow, &container, &slot)) return;
                const Node* c = tree.find(container);
                if (c == nullptr || slot < 0 || slot >= static_cast<int>(c->children.size()))
                    return;
                const Node* probe = tree.find(s.detached);
                if (probe == nullptr) return;
                // 从后往前搬，每枚都挂到同一个槽位（节点自己后面那一格），顺序因此保持；
                //   而节点的下标全程不动，最后摘它的时候不必重算。
                const int count = static_cast<int>(probe->children.size());
                for (int k = count - 1; k >= 0; --k) {
                    const Node* cur = tree.find(s.detached);
                    if (cur == nullptr || k >= static_cast<int>(cur->children.size())) break;
                    const NodeId kid = cur->children[static_cast<size_t>(k)]->id;
                    tree.eraseChild(s.detached, k);
                    tree.attachChild(container, slot + 1, kid);
                }
                tree.eraseChild(container, slot);
            },
            [&](const WrapNode& s) {
                // 区间取 [at_flow, at_flow + span_after)：这是拆开之后（少了那两枚边界）的
                //   地址，也正是这一步所在状态里的地址。两端都按流位置定位、该切的文本就切
                //   （见 slotAtFlow）：切出来的两个槽位就是区间起止。
                NodeId container = kInvalidNodeId;
                int a = -1;
                int b = -1;
                if (!slotAtFlow(tree, s.at_flow, &container, &a)) return;
                if (!slotAtFlow(tree, s.at_flow + s.span_after, &container, &b)) return;
                if (a < 0 || b <= a) return;
                for (int k = b - 1; k >= a; --k) {
                    const Node* c = tree.find(container);
                    if (c == nullptr || k >= static_cast<int>(c->children.size())) break;
                    const NodeId kid = c->children[static_cast<size_t>(k)]->id;
                    tree.eraseChild(container, k);
                    tree.attachChild(s.detached, 0, kid);
                }
                tree.attachChild(container, a, s.detached);
            },
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
                          [](const SetParaMarkFormat& s) -> Step {
                              SetParaMarkFormat x;
                              x.id = s.id;
                              x.para = s.before;
                              x.styleRef = s.beforeStyleRef;
                              x.before = s.para;
                              x.beforeStyleRef = s.styleRef;
                              return Step{x};
                          },
                          [](const SetRunMarks& s) -> Step {
                              SetRunMarks x;
                              x.id = s.id;
                              x.marks = s.before;
                              x.styleRef = s.beforeStyleRef;
                              x.before = s.marks;
                              x.beforeStyleRef = s.styleRef;
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
                          // 这两个互为逆：字段一一对应，于是往返严格。
                          [](const UnwrapNode& s) -> Step {
                              WrapNode w;
                              w.at = s.at;
                              w.at_flow = s.at_flow;
                              w.span_before = s.span_before;
                              w.span_after = s.span_after;
                              w.node = s.node;
                              w.detached = s.detached;
                              return Step{w};
                          },
                          [](const WrapNode& s) -> Step {
                              UnwrapNode u;
                              u.at = s.at;
                              u.at_flow = s.at_flow;
                              u.span_before = s.span_before;
                              u.span_after = s.span_after;
                              u.node = s.node;
                              u.detached = s.detached;
                              return Step{u};
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
                          // 拆开：区间里的流长度从"节点占的"缩到"内容占的"，差就是节点自己
                          //   那两枚边界。落在区间里的位置夹到区间末尾 —— 它先前的去处已经不在。
                          [&](const UnwrapNode& s) -> FlowPos {
                              const FlowPos shrink = s.span_before - s.span_after;
                              if (pos <= s.at_flow) return pos;
                              if (pos >= s.at_flow + s.span_before) return pos - shrink;
                              return s.at_flow +
                                     std::min(pos - s.at_flow, static_cast<FlowPos>(s.span_after));
                          },
                          // 包起：反过来。落在内容里的位置越过新出现的那枚开括号，所以 +1。
                          [&](const WrapNode& s) -> FlowPos {
                              const FlowPos grow = s.span_before - s.span_after;
                              if (pos <= s.at_flow) return pos;
                              if (pos >= s.at_flow + s.span_after) return pos + grow;
                              return pos + 1;
                          },
                          // 属性不改编辑态的位置。hidden 会改呈现态的流结构，但那要重排
                          // （重建派生索引），不是把某个位置搬到别处。
                          [&](const SetControlProp&) -> FlowPos { return pos; },
                          [&](const SetControlValue&) -> FlowPos { return pos; },
                          // 跨度不改流位置，也不改流结构：它改的是"这一格覆盖多少格位"。
                          [&](const SetTableCellSpan&) -> FlowPos { return pos; },
                          // 段落格式与字符格式同理：改的是节点上的数据，位置一个都不动。
                          [&](const SetParaMarkFormat&) -> FlowPos { return pos; },
                          [&](const SetRunMarks&) -> FlowPos { return pos; },
                      },
                      step);
}

bool mapStepPositions(Step* target, const Step& mapped_by, Assoc assoc) {
    if (target == nullptr) return false;
    // 属性、跨度与段落格式步骤不带流位置，搬一次等于没搬（但它们不是"搬不动"，所以算成功）。
    if (std::holds_alternative<SetControlProp>(*target) ||
        std::holds_alternative<SetControlValue>(*target) ||
        std::holds_alternative<SetTableCellSpan>(*target) ||
        std::holds_alternative<SetParaMarkFormat>(*target) ||
        std::holds_alternative<SetRunMarks>(*target)) {
        return true;
    }
    if (auto* s = std::get_if<InsertText>(target)) {
        s->at = mapPos(mapped_by, s->at, assoc);
        return true;
    }
    if (auto* s = std::get_if<InsertFragment>(target)) {
        s->at = mapPos(mapped_by, s->at, assoc);
        return true;
    }
    if (auto* s = std::get_if<DeleteRange>(target)) {
        s->begin = mapPos(mapped_by, s->begin, assoc);
        s->end = mapPos(mapped_by, s->end, assoc);
        return true;
    }
    if (auto* s = std::get_if<InsertParaMark>(target)) {
        s->at = mapPos(mapped_by, s->at, assoc);
        return true;
    }
    if (auto* s = std::get_if<RemoveParaMark>(target)) {
        s->at = mapPos(mapped_by, s->at, assoc);
        return true;
    }
    // 节点级步骤：流位置能搬，但 Pos（槽位）搬不动 —— 半个映射比不映射更危险。
    return false;
}

}  // namespace heditor
