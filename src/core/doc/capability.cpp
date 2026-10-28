#include "core/doc/capability.h"

#include <string>
#include <vector>

namespace heditor::doc {

namespace {

FlowVisibility visibilityOf(EditMode mode) {
    return mode == EditMode::Document ? FlowVisibility::Design : FlowVisibility::Display;
}

/**
 * 内容控件的记录。种类不是控件时返回空指针 —— 调用方都在判过种类之后才取。
 */
const ControlSpec* controlOf(const Node& n) {
    return n.kind == NodeKind::ContentControl ? std::get_if<ControlSpec>(&n.payload) : nullptr;
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
 * 收集严格包住 pos 的全部内容控件，从外到内。嵌套控件要全部收齐，可编辑集合是它们的交集。
 */
void collectControlsAt(const Doc& doc, const FlowIndex& idx, FlowPos pos,
                       std::vector<const Node*>* out) {
    NodeId cur = doc.root ? doc.root->id : kInvalidNodeId;
    while (cur != kInvalidNodeId) {
        const Node* n = findNode(doc, cur);
        if (n == nullptr || !isContainer(n->kind)) return;
        NodeId next = kInvalidNodeId;
        for (const Ref& c : n->children) {
            if (strictlyInside(idx, c->id, pos)) {
                next = c->id;
                break;
            }
        }
        if (next == kInvalidNodeId) return;
        const Node* nc = findNode(doc, next);
        if (nc == nullptr) return;
        if (nc->kind == NodeKind::ContentControl) out->push_back(nc);
        cur = next;
    }
}

/**
 * 按前序收集文档里的全部内容控件。
 */
void collectAllControls(const Ref& n, std::vector<const Node*>* out) {
    if (!n) return;
    if (n->kind == NodeKind::ContentControl) out->push_back(n.get());
    for (const Ref& c : n->children) collectAllControls(c, out);
}

/**
 * 控件在回执里的名字：有 key 用 key，没有就用节点号，保证能指到具体的那一个。
 */
std::string controlName(const Node& n) {
    const ControlSpec* spec = controlOf(n);
    if (spec != nullptr && !spec->key.empty()) return "key=" + spec->key;
    return "id=" + std::to_string(n.id);
}

/**
 * 已经知道模式是表单、已经建好索引时，位置是否可编辑。三个公开入口共用它。
 */
EditCheck editableAt(const Doc& doc, const FlowIndex& idx, FlowPos pos) {
    std::vector<const Node*> chain;
    collectControlsAt(doc, idx, pos, &chain);
    if (chain.empty()) return EditCheck{false, "落点不在任何可编辑控件内"};
    for (const Node* c : chain) {
        const ControlSpec* spec = controlOf(*c);
        if (spec != nullptr && spec->readonly) {
            return EditCheck{false, "落点在只读控件（" + controlName(*c) + "）内"};
        }
    }
    return EditCheck{true, ""};
}

}  // namespace

bool caretBarrierAt(const Doc& doc, const FlowIndex& idx, FlowPos pos) {
    if (!doc.root) return false;
    std::vector<const Node*> all;
    collectAllControls(doc.root, &all);
    int f = 0;
    int e = 0;
    for (const Node* c : all) {
        if (!idx.intervalOf(c->id, &f, &e)) continue;
        // 内容区间 = 两枚括号之间；括号本身与它两侧都不算。
        if (pos <= f || pos >= e) continue;
        const ControlSpec* spec = controlOf(*c);
        if (spec == nullptr) continue;
        if (controlCaretBarrier(*spec)) return true;
    }
    return false;
}

EditCheck canEditAt(const Doc& doc, EditMode mode, FlowPos pos) {
    if (mode == EditMode::ReadOnly) return EditCheck{false, "只读模式：不接受编辑"};
    if (!doc.root) return EditCheck{false, "文档为空"};

    const FlowIndex idx(doc, visibilityOf(mode));
    if (pos < 0 || pos > idx.size()) return EditCheck{false, "位置越界"};
    // 屏障与模式无关：选择类的内容在三档里都不是可写正文。
    if (caretBarrierAt(doc, idx, pos)) {
        return EditCheck{false, "选择类控件的内容不接受编辑"};
    }
    if (mode == EditMode::Document) return EditCheck{true, ""};
    return editableAt(doc, idx, pos);
}

EditCheck canEditRange(const Doc& doc, EditMode mode, Range range) {
    if (mode == EditMode::Document) return EditCheck{true, ""};
    if (mode == EditMode::ReadOnly) return EditCheck{false, "只读模式：不接受编辑"};
    if (range.empty()) return canEditAt(doc, mode, range.anchor);
    if (!doc.root) return EditCheck{false, "文档为空"};

    const FlowIndex idx(doc, visibilityOf(mode));
    const FlowPos b = range.begin();
    const FlowPos e = range.end();
    if (b < 0 || e > idx.size()) return EditCheck{false, "位置越界"};

    const EditCheck at_begin = editableAt(doc, idx, b);
    if (!at_begin.allowed) return at_begin;
    const EditCheck at_end = editableAt(doc, idx, e);
    if (!at_end.allowed) return at_end;

    // 区间内部不能整块包住一个只读控件。只查两端漏掉"跨过只读控件"，而它恰好能删掉只读控件
    // 的全部内容（含它的一对边界），把 readonly 绕开。
    std::vector<const Node*> all;
    collectAllControls(doc.root, &all);
    int f = 0;
    int end = 0;
    for (const Node* c : all) {
        const ControlSpec* spec = controlOf(*c);
        if (spec == nullptr || !spec->readonly) continue;
        if (!idx.intervalOf(c->id, &f, &end)) continue;  // 不在流里（隐藏），本就删不到
        if (f >= b && end <= e) {
            return EditCheck{false, "区间整块包住只读控件（" + controlName(*c) + "）"};
        }
    }
    return EditCheck{true, ""};
}

FlowPos stepEditable(const Doc& doc, EditMode mode, FlowPos from, int dir) {
    if (dir == 0) return from;
    const int step = dir > 0 ? 1 : -1;
    if (!doc.root) return from;
    const FlowIndex idx(doc, visibilityOf(mode));

    if (mode != EditMode::Form) {
        // 文档与只读模式只看屏障：方向键一步一格，撞上选择类就跨过去。
        //   跨的是一整块（它的内容可能几十个码点），所以这里循环到出界为止。
        FlowPos t = from + step;
        while (t >= 0 && t <= idx.size() && caretBarrierAt(doc, idx, t)) t += step;
        if (t < 0) return 0;
        if (t > idx.size()) return idx.size();
        return t;
    }

    for (FlowPos q = from + step; q >= 0 && q <= idx.size(); q += step) {
        if (editableAt(doc, idx, q).allowed) return q;
    }
    return from;
}

TabStop tabFrom(const Doc& doc, EditMode mode, FlowPos from, bool backward) {
    // 文档模式与只读模式都不消费：前者要把 Tab 当制表符，后者要放行给宿主。
    if (mode != EditMode::Form) return TabStop{false, from, kInvalidNodeId};
    if (!doc.root) return TabStop{false, from, kInvalidNodeId};

    const FlowIndex idx(doc, visibilityOf(mode));
    std::vector<const Node*> all;
    collectAllControls(doc.root, &all);

    FlowPos best = -1;
    NodeId best_owner = kInvalidNodeId;
    for (const Node* c : all) {
        const FlowPos start = idx.flowAt(Pos{c->id, 0});
        if (start < 0) continue;  // 隐藏，不在流里
        // 可编辑控件的判据就是"内容起点在可编辑集合内"：只读控件、以及只读控件里的可编辑
        // 控件，都在这里被同一个谓词挡掉，不必另写一套嵌套判定。
        if (!editableAt(doc, idx, start).allowed) continue;
        int e = 0;
        if (!idx.intervalOf(c->id, nullptr, &e)) continue;
        // 前进取"内容起点在当前位之后"的；后退取"整个控件都在当前位之前"的。后者排除了光标
        // 所在的那个控件自己，否则从控件中间按 Shift+Tab 会先回到它自己的开头。
        const bool candidate = backward ? (e <= from) : (start > from);
        if (!candidate) continue;
        if (best < 0 || (backward ? start > best : start < best)) {
            best = start;
            best_owner = c->id;
        }
    }
    if (best < 0) return TabStop{false, from, kInvalidNodeId};
    return TabStop{true, best, best_owner};
}

EditCheck canDoStructureOp(const Doc& doc, EditMode mode, NodeId id) {
    if (mode == EditMode::Document) return EditCheck{true, ""};
    if (mode == EditMode::ReadOnly) return EditCheck{false, "只读模式：不接受结构操作"};

    const Node* n = findNode(doc, id);
    if (n == nullptr) return EditCheck{false, "节点不存在"};
    if (n->kind != NodeKind::ContentControl) {
        return EditCheck{false, std::string("结构操作只对控件开放：") + nodeKindName(n->kind)};
    }
    const ControlSpec* spec = controlOf(*n);
    if (spec != nullptr && spec->structureLock) {
        return EditCheck{false, "控件结构被锁住（" + controlName(*n) + "）"};
    }
    return EditCheck{true, ""};
}

EditCheck canEditStep(const Doc& doc, EditMode mode, const Step& step) {
    if (mode == EditMode::Document) return EditCheck{true, ""};
    if (mode == EditMode::ReadOnly) return EditCheck{false, "只读模式：不接受编辑"};

    // 内容级步骤：落在哪个区间就按可编辑集合判。
    if (const auto* s = std::get_if<InsertText>(&step)) return canEditAt(doc, mode, s->at);
    if (const auto* s = std::get_if<InsertFragment>(&step)) {
        return canEditRange(doc, mode, Range{s->at, s->at + s->content.span()});
    }
    if (const auto* s = std::get_if<DeleteRange>(&step)) {
        return canEditRange(doc, mode, Range{s->begin, s->end});
    }
    if (const auto* s = std::get_if<InsertParaMark>(&step)) return canEditAt(doc, mode, s->at);
    if (const auto* s = std::get_if<RemoveParaMark>(&step)) {
        return canEditRange(doc, mode, Range{s->at, s->at + 1});
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
        const Node* c = findNode(doc, s->at.container);
        if (c == nullptr || !isContainer(c->kind))
            return EditCheck{false, "删除节点：目标不是容器"};
        if (s->at.offset < 0 || s->at.offset >= static_cast<int>(c->children.size())) {
            return EditCheck{false, "删除节点：槽位没有孩子"};
        }
        return canDoStructureOp(doc, mode, c->children[static_cast<size_t>(s->at.offset)]->id);
    }
    // 拆壳（UnwrapNode）也是"把这个节点从结构里拿掉"，与 RemoveNode 同一个门。
    if (const auto* s = std::get_if<UnwrapNode>(&step)) {
        const Node* c = findNode(doc, s->at.container);
        if (c == nullptr || !isContainer(c->kind))
            return EditCheck{false, "拆开节点：目标不是容器"};
        if (s->at.offset < 0 || s->at.offset >= static_cast<int>(c->children.size())) {
            return EditCheck{false, "拆开节点：槽位没有孩子"};
        }
        return canDoStructureOp(doc, mode, c->children[static_cast<size_t>(s->at.offset)]->id);
    }
    // 包一层（WrapNode）是"新造一个容器"，与 InsertNode 同一个门。
    if (const auto* s = std::get_if<WrapNode>(&step)) {
        if (s->node.kind != NodeKind::ContentControl) {
            return EditCheck{false, std::string("表单模式只允许包裹控件，不接受：") +
                                        nodeKindName(s->node.kind)};
        }
        if (s->node.control.structureLock) {
            return EditCheck{false, "控件结构被锁住（包裹被拒）"};
        }
        return EditCheck{true, ""};
    }

    // 改属性是"运行时状态与策略"，由宿主回填与业务逻辑发起（第七节），不受表单模式的落点判定
    // 限制 —— 只读模式在上面已经挡住了。这里只保证目标确实是个控件。
    if (const auto* s = std::get_if<SetControlProp>(&step)) {
        const Node* n = findNode(doc, s->id);
        if (n == nullptr) return EditCheck{false, "改属性：目标不存在"};
        if (n->kind != NodeKind::ContentControl) {
            return EditCheck{false,
                             std::string("改属性：目标不是控件（") + nodeKindName(n->kind) + "）"};
        }
        return EditCheck{true, ""};
    }
    if (const auto* s = std::get_if<SetControlValue>(&step)) {
        const Node* n = findNode(doc, s->id);
        if (n == nullptr) return EditCheck{false, "改值：目标不存在"};
        if (n->kind != NodeKind::ContentControl) {
            return EditCheck{false,
                             std::string("改值：目标不是控件（") + nodeKindName(n->kind) + "）"};
        }
        return EditCheck{true, ""};
    }
    // 改格记录是结构操作：它改的是表格的形状，不是某个格子里的内容。
    if (const auto* s = std::get_if<SetCellSpec>(&step)) {
        const Node* n = findNode(doc, s->id);
        if (n == nullptr || n->kind != NodeKind::TableCell) {
            return EditCheck{false, "改格记录：目标不是单元格"};
        }
        if (mode == EditMode::ReadOnly) return EditCheck{false, "只读模式：不接受结构操作"};
        if (mode == EditMode::Form) {
            return EditCheck{false, "表单模式：表格结构操作要在文档模式里做（开关默认锁住）"};
        }
        return EditCheck{true, ""};
    }
    // 行与表的记录同样是设计动作：填表的人不该改分页与表宽。
    if (const auto* s = std::get_if<SetRowSpec>(&step)) {
        const Node* n = findNode(doc, s->id);
        if (n == nullptr || n->kind != NodeKind::TableRow) {
            return EditCheck{false, "改行记录：目标不是表格行"};
        }
        if (mode != EditMode::Document) {
            return EditCheck{false, "改行记录要在文档模式里做（表格式设计动作）"};
        }
        return EditCheck{true, ""};
    }
    if (const auto* s = std::get_if<SetTableSpec>(&step)) {
        const Node* n = findNode(doc, s->id);
        if (n == nullptr || n->kind != NodeKind::Table) {
            return EditCheck{false, "改表记录：目标不是表格"};
        }
        if (mode != EditMode::Document) {
            return EditCheck{false, "改表记录要在文档模式里做（表格式设计动作）"};
        }
        return EditCheck{true, ""};
    }
    // 图片的定位与环绕是版式：填表的人与只读态都不该改它（改尺寸另有入口，规则同此）。
    if (const auto* s = std::get_if<SetImageSpec>(&step)) {
        const Node* n = findNode(doc, s->id);
        if (n == nullptr || n->kind != NodeKind::Image) {
            return EditCheck{false, "改图片记录：目标不是图片"};
        }
        if (mode != EditMode::Document) {
            return EditCheck{false, "改图片记录要在文档模式里做（版式是设计动作）"};
        }
        return EditCheck{true, ""};
    }
    // 页面设置（含水印）是版式：填表的人与只读态都不该改它。
    if (const auto* s = std::get_if<SetSectionSpec>(&step)) {
        const Node* n = findNode(doc, s->id);
        if (n == nullptr || n->kind != NodeKind::Section) {
            return EditCheck{false, "改节记录：目标不是节"};
        }
        if (mode != EditMode::Document) {
            return EditCheck{false, "改节记录要在文档模式里做（页面设置是设计动作）"};
        }
        return EditCheck{true, ""};
    }
    return EditCheck{false, "未知步骤"};
}

}  // namespace heditor::doc
