#include "core/doc/edit_capability.h"

#include <string>
#include <vector>

namespace heditor {
namespace {

/**
 * 该模式用哪套可见性。编辑态是设计态，隐藏的控件照常参与流；表单与只读下整棵摘除。
 */
FlowVisibility visibilityOf(EditMode mode) {
    return mode == EditMode::Document ? FlowVisibility::Design : FlowVisibility::Display;
}

/**
 * pos 是不是落在 id 的内部（严格）。
 *
 * 口径与 FlowIndex::posAt 的下降规则一致：区间是 [first, end)，严格内部是 first < pos < end。
 * 容器在 pos == first 处是"它之前"、在 pos == end 处是"它之后"，都不算内部；结束边界那一位
 * （end - 1）算内部，它是内容的末尾。
 */
bool strictlyInside(const FlowIndex& idx, NodeId id, FlowPos pos) {
    int f = 0;
    int e = 0;
    if (!idx.intervalOf(id, &f, &e)) return false;
    return pos > f && pos < e;
}

/**
 * 收集严格包住 pos 的全部内容控件，从外到内。
 *
 * 一路按"哪个孩子严格包含 pos"下降，每遇到一个内容控件就记下再继续往里找：嵌套控件要全部
 * 收齐，可编辑集合是它们的交集。遇到文本或原子就停（它们没有孩子）。
 */
void collectControlsAt(const NodeTree& tree, const FlowIndex& idx, FlowPos pos,
                       std::vector<const Node*>* out) {
    NodeId cur = tree.rootId();
    while (true) {
        const Node* n = tree.find(cur);
        if (n == nullptr || !isContainer(n->kind)) return;
        const Node* next = nullptr;
        for (const Node* c : n->children) {
            if (strictlyInside(idx, c->id, pos)) {
                next = c;
                break;
            }
        }
        if (next == nullptr) return;
        if (next->kind == NodeKind::ContentControl) out->push_back(next);
        cur = next->id;
    }
}

/**
 * 按前序收集树里的全部内容控件。
 */
void collectAllControls(const NodeTree& tree, NodeId id, std::vector<const Node*>* out) {
    const Node* n = tree.find(id);
    if (n == nullptr) return;
    if (n->kind == NodeKind::ContentControl) out->push_back(n);
    for (const Node* c : n->children) collectAllControls(tree, c->id, out);
}

/**
 * 控件在回执里的名字：有 key 用 key，没有就用节点 id，保证能指到具体的那一个。
 */
std::string controlName(const Node& n) {
    if (!n.control.key.empty()) return "key=" + n.control.key;
    return "id=" + std::to_string(n.id);
}

/**
 * 已经知道模式是表单、已经建好索引时，位置是否可编辑。三个公开入口共用它，
 * 免得每个都重建一次索引、也免得三处口径各写一遍。
 */
EditCheck editableAt(const NodeTree& tree, const FlowIndex& idx, FlowPos pos) {
    std::vector<const Node*> chain;
    collectControlsAt(tree, idx, pos, &chain);
    if (chain.empty()) return EditCheck{false, "落点不在任何可编辑控件内"};
    for (const Node* c : chain) {
        if (c->control.readonly) {
            return EditCheck{false, "落点在只读控件（" + controlName(*c) + "）内"};
        }
    }
    return EditCheck{true, ""};
}

}  // namespace

EditCheck canEditAt(const NodeTree& tree, EditMode mode, FlowPos pos) {
    if (mode == EditMode::Document) return EditCheck{true, ""};
    if (mode == EditMode::ReadOnly) return EditCheck{false, "只读模式：不接受编辑"};

    const FlowIndex idx(tree, visibilityOf(mode));
    if (pos < 0 || pos > idx.size()) return EditCheck{false, "位置越界"};
    return editableAt(tree, idx, pos);
}

EditCheck canEditRange(const NodeTree& tree, EditMode mode, Range range) {
    if (mode == EditMode::Document) return EditCheck{true, ""};
    if (mode == EditMode::ReadOnly) return EditCheck{false, "只读模式：不接受编辑"};
    if (range.empty()) return canEditAt(tree, mode, range.anchor);

    const FlowIndex idx(tree, visibilityOf(mode));
    const FlowPos b = range.begin();
    const FlowPos e = range.end();
    if (b < 0 || e > idx.size()) return EditCheck{false, "位置越界"};

    // 两端都要落在集合内。终点用 e（区间覆盖的最后一个位置之后那一位）：删除的右端点就落在
    // 那里，它若在集合外，说明该编辑要越出控件。
    const EditCheck at_begin = editableAt(tree, idx, b);
    if (!at_begin.allowed) return at_begin;
    const EditCheck at_end = editableAt(tree, idx, e);
    if (!at_end.allowed) return at_end;

    // 区间内部不能整块包住一个只读控件。只查两端漏掉"跨过只读控件"，而它恰好能删掉只读控件
    // 的全部内容（含它的一对边界），把 readonly 绕开。
    std::vector<const Node*> all;
    collectAllControls(tree, tree.rootId(), &all);
    int f = 0;
    int end = 0;
    for (const Node* c : all) {
        if (!c->control.readonly) continue;
        if (!idx.intervalOf(c->id, &f, &end)) continue;  // 不在流里（隐藏），本就删不到
        if (f >= b && end <= e) {
            return EditCheck{false, "区间整块包住只读控件（" + controlName(*c) + "）"};
        }
    }
    return EditCheck{true, ""};
}

FlowPos stepEditable(const NodeTree& tree, EditMode mode, FlowPos from, int dir) {
    if (dir == 0) return from;
    const int step = dir > 0 ? 1 : -1;
    const FlowIndex idx(tree, visibilityOf(mode));

    if (mode != EditMode::Form) {
        const FlowPos t = from + step;
        if (t < 0) return 0;
        if (t > idx.size()) return idx.size();
        return t;
    }

    for (FlowPos q = from + step; q >= 0 && q <= idx.size(); q += step) {
        if (editableAt(tree, idx, q).allowed) return q;
    }
    return from;
}

TabStop tabFrom(const NodeTree& tree, EditMode mode, FlowPos from, bool backward) {
    // 文档模式与只读模式都不消费：前者要把 Tab 当制表符，后者要放行给宿主。
    if (mode != EditMode::Form) return TabStop{false, from};

    const FlowIndex idx(tree, visibilityOf(mode));
    std::vector<const Node*> all;
    collectAllControls(tree, tree.rootId(), &all);

    FlowPos best = -1;
    for (const Node* c : all) {
        const FlowPos start = idx.flowAt(Pos{c->id, 0});
        if (start < 0) continue;  // 隐藏，不在流里
        // 可编辑控件的判据就是"内容起点在可编辑集合内"：只读控件、以及只读控件里的可编辑
        // 控件，都在这里被同一个谓词挡掉，不必另写一套嵌套判定。
        if (!editableAt(tree, idx, start).allowed) continue;
        int e = 0;
        if (!idx.intervalOf(c->id, nullptr, &e)) continue;
        // 前进取"内容起点在当前位之后"的；后退取"整个控件都在当前位之前"的。后者排除了光标
        // 所在的那个控件自己，否则从控件中间按 Shift+Tab 会先回到它自己的开头。
        const bool candidate = backward ? (e <= from) : (start > from);
        if (!candidate) continue;
        if (best < 0 || (backward ? start > best : start < best)) best = start;
    }
    if (best < 0) return TabStop{false, from};
    return TabStop{true, best};
}

EditCheck canDoStructureOp(const NodeTree& tree, EditMode mode, NodeId id) {
    if (mode == EditMode::Document) return EditCheck{true, ""};
    if (mode == EditMode::ReadOnly) return EditCheck{false, "只读模式：不接受结构操作"};

    const Node* n = tree.find(id);
    if (n == nullptr) return EditCheck{false, "节点不存在"};
    if (n->kind != NodeKind::ContentControl) {
        return EditCheck{false, std::string("结构操作只对控件开放：") + nodeKindName(n->kind)};
    }
    if (n->control.structureLock) {
        return EditCheck{false, "控件结构被锁住（" + controlName(*n) + "）"};
    }
    return EditCheck{true, ""};
}

EditCheck canEditStep(const NodeTree& tree, EditMode mode, const Step& step) {
    if (mode == EditMode::Document) return EditCheck{true, ""};
    if (mode == EditMode::ReadOnly) return EditCheck{false, "只读模式：不接受编辑"};

    // 内容级步骤：落在哪个区间就按可编辑集合判。
    if (const auto* s = std::get_if<InsertText>(&step)) return canEditAt(tree, mode, s->at);
    if (const auto* s = std::get_if<InsertFragment>(&step)) {
        return canEditRange(tree, mode, Range{s->at, s->at + s->content.span()});
    }
    if (const auto* s = std::get_if<DeleteRange>(&step)) {
        return canEditRange(tree, mode, Range{s->begin, s->end});
    }
    if (const auto* s = std::get_if<InsertParaMark>(&step)) return canEditAt(tree, mode, s->at);
    if (const auto* s = std::get_if<RemoveParaMark>(&step)) {
        return canEditRange(tree, mode, Range{s->at, s->at + 1});
    }

    // 节点级步骤：表单模式下只允许对声明可删的控件做，其余元素属于编辑态的内容。
    if (const auto* s = std::get_if<InsertNode>(&step)) {
        if (s->subtree.kind != NodeKind::ContentControl) {
            return EditCheck{false, std::string("表单模式只允许插入控件，不接受：") +
                                        nodeKindName(s->subtree.kind)};
        }
        if (s->subtree.control.structureLock) {
            return EditCheck{false, "控件结构被锁住（插入被拒）"};
        }
        return EditCheck{true, ""};
    }
    if (const auto* s = std::get_if<RemoveNode>(&step)) {
        const Node* c = tree.find(s->at.container);
        if (c == nullptr || !isContainer(c->kind))
            return EditCheck{false, "删除节点：目标不是容器"};
        if (s->at.offset < 0 || s->at.offset >= static_cast<int>(c->children.size())) {
            return EditCheck{false, "删除节点：槽位没有孩子"};
        }
        return canDoStructureOp(tree, mode, c->children[static_cast<size_t>(s->at.offset)]->id);
    }

    // 改属性是"运行时状态与策略"，由宿主回填与业务逻辑发起（第七节），不受表单模式的落点判定
    // 限制 —— 只读模式在上面已经挡住了。这里只保证目标确实是个控件。
    if (const auto* s = std::get_if<SetControlProp>(&step)) {
        const Node* n = tree.find(s->id);
        if (n == nullptr) return EditCheck{false, "改属性：目标不存在"};
        if (n->kind != NodeKind::ContentControl) {
            return EditCheck{false,
                             std::string("改属性：目标不是控件（") + nodeKindName(n->kind) + "）"};
        }
        return EditCheck{true, ""};
    }
    if (const auto* s = std::get_if<SetControlValue>(&step)) {
        const Node* n = tree.find(s->id);
        if (n == nullptr) return EditCheck{false, "改值：目标不存在"};
        if (n->kind != NodeKind::ContentControl) {
            return EditCheck{false,
                             std::string("改值：目标不是控件（") + nodeKindName(n->kind) + "）"};
        }
        return EditCheck{true, ""};
    }
    // 改跨度是结构操作：它改的是表格的形状，不是某个格子里的内容。它的开关按第九节的说法
    // 挂在被操作的元素上（表格 / 行 / 单元格），而那个开关要等表格自己成为控件才有地方挂，
    // 所以现在按"默认锁住"处理：表单模式拒绝，与控件结构的默认一致。
    if (const auto* s = std::get_if<SetTableCellSpan>(&step)) {
        const Node* n = tree.find(s->id);
        if (n == nullptr || n->kind != NodeKind::TableCell) {
            return EditCheck{false, "改跨度：目标不是单元格"};
        }
        if (mode == EditMode::ReadOnly) return EditCheck{false, "只读模式：不接受结构操作"};
        if (mode == EditMode::Form) {
            return EditCheck{false, "表单模式：表格结构操作要在文档模式里做（开关默认锁住）"};
        }
        return EditCheck{true, ""};
    }
    return EditCheck{false, "未知步骤"};
}

}  // namespace heditor
