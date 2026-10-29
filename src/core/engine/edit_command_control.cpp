/**
 * 命令层的读取与控件写入：格式回显、整块删除、控件属性与取值、插表格与控件、图片注册。
 *
 * 读取一律按"光标所在处"定位；写入与插入走同一条事务路径，撤销与重排不必另开通道。
 */
#include "core/engine/edit_command.h"
#include "core/engine/edit_command_util.h"

#include "core/doc/subtree.h"
#include "core/doc/table_grid.h"
#include "core/engine/control_spans.h"
#include "core/engine/structure_index.h"
#include "core/utils/utf8.h"

#include <algorithm>
#include <functional>
#include <variant>

namespace heditor {

using namespace detail;

CharFormat EditCommand::marksAtFlow(FlowPos at, std::string* style_ref) const {
    if (style_ref != nullptr) style_ref->clear();
    if (!hasFlow()) return CharFormat{};
    const Pos p = flowIndex().posAt(at);
    const doc::Node* n = doc::findNode(doc(), p.container);
    if (n == nullptr) return CharFormat{};

    /**
     * 取一个节点的文本载荷；不是文本节点就给空。
     * @param node 候选
     * @return 文本载荷（不是文本节点时为空）
     */
    auto runOf = [](const doc::Node* node) -> const doc::RunPayload* {
        if (node == nullptr || !isText(node->kind)) return nullptr;
        return std::get_if<doc::RunPayload>(&node->payload);
    };

    /**
     * 采用一个文本节点的标记。
     * @param run 文本载荷
     * @return 标记（并记下样式引用）
     */
    auto adopt = [&](const doc::RunPayload& run) {
        if (style_ref != nullptr) *style_ref = run.style;
        return run.marks;
    };

    // 落点在文本节点内部：左边就是这个节点里的字，直接用它。
    //   只有 offset > 0 才算里面 —— offset == 0 是一个"两个节点之间"的位置，见下。
    if (const auto* self = runOf(n); self != nullptr && p.offset > 0) return adopt(*self);

    // 其余的落点统一化成"在父容器的第 slot 个孩子之前"，再按 左 → 右 找最近的文本兄弟。
    //
    //   文本节点 offset == 0 也必须走这条路：posAt 把"两段之间的位置"归给右边那个叶子
    //   （容器的第 0 槽位与第一个孩子的第一个位置是同一个流位置，规范表示取最内层），
    //   于是"紧挨着粗体字左边"与"粗体字自己第一个字之前"是同一个位置。直接采用它，敲字
    //   就沿用了右边的格式：在粗体字前面敲出来的也变粗，紧接粗体字后面敲反倒变回普通格式，
    //   跟"接着已有的字往下写"正好相反。取左边的邻居才是接着写。
    //
    //   落在容器槽位、段落标记（不占字符却占一个流位置）与原子上的情形共用这条规则：
    //   段落末尾敲字时流位置正好是段落标记的起点，左边那一段才是要接着写的。
    const doc::Node* parent = nullptr;
    int slot = 0;
    if (isContainer(n->kind)) {
        parent = n;
        slot = p.offset;
    } else {
        parent = doc::parentOf(doc(), n->id);
        if (parent == nullptr) return CharFormat{};
        for (size_t i = 0; i < parent->children.size(); ++i) {
            if (parent->children[i] && parent->children[i]->id == n->id) {
                slot = static_cast<int>(i);
                break;
            }
        }
    }
    // 先往左找（敲字接在已有文字后面是最常见的一种），左边没有才往右找（段落开头）。
    for (int i = slot - 1; i >= 0; --i) {
        if (const auto* run = runOf(parent->children[static_cast<size_t>(i)].get())) {
            return adopt(*run);
        }
    }
    for (int i = slot; i < static_cast<int>(parent->children.size()); ++i) {
        if (const auto* run = runOf(parent->children[static_cast<size_t>(i)].get())) {
            return adopt(*run);
        }
    }
    // 空段落：这一段一个文本节点都没有，接着敲的字只能沿用结束标记上的字符格式。
    //   那正是"选中空单元格改格式"落下去的地方，两处读同一份，回显与行为才不分叉。
    for (const doc::Ref& c : parent->children) {
        if (!c || !isParagraphMark(c->kind)) continue;
        const auto* mp = std::get_if<doc::MarkPayload>(&c->payload);
        if (mp == nullptr) continue;
        if (style_ref != nullptr) *style_ref = mp->style;
        return mp->marks;
    }
    return CharFormat{};
}

CharFormat EditCommand::marksAtSelectionStart(FlowPos lo, FlowPos hi,
                                              std::string* style_ref) const {
    if (style_ref != nullptr) style_ref->clear();
    if (!hasFlow() || hi <= lo) return marksAtFlow(lo, style_ref);
    const doc::FlowIndex& idx = flowIndex();
    CharFormat out;
    bool found = false;
    std::function<void(const doc::Ref&)> walk = [&](const doc::Ref& n) {
        if (!n || found) return;
        if (n->kind == NodeKind::TextRun) {
            int f = 0;
            int e = 0;
            // 与选区相交的第一个文本节点。遍历顺序就是文档顺序，所以"第一个"不必比较坐标。
            if (idx.intervalOf(n->id, &f, &e) && e > lo && f < hi) {
                const auto& rp = std::get<doc::RunPayload>(n->payload);
                if (style_ref != nullptr) *style_ref = rp.style;
                out = rp.marks;
                found = true;
            }
        }
        for (const doc::Ref& c : n->children) walk(c);
    };
    walk(doc().root);
    // 选区里一个文本节点都没有（整段只有一个控件框）：退回"落点左边"。这种位置上敲字
    //   本来就是接着左边写的，两个口径在这里不冲突。
    if (found) return out;
    return marksAtFlow(lo, style_ref);
}

/**
 * 删控件与改控件属性：结构操作的引擎入口。
 *
 * 两件事都只动树，改完交给 commitEdit 一次重建 —— 与文本编辑走同一条路，于是撤销、重排、
 * 重绘、记账自动一致，不必另开一条特权通道。
 */
bool EditCommand::removeControl(int index, bool keep_content, FlowPos caret_before) {
    if (!hasFlow()) return false;
    if (index < 0 || index >= controls().size()) return false;
    const NodeId id = controlNodeIds()[static_cast<size_t>(index)];
    const doc::Node* c = doc::findNode(doc(), id);
    if (c == nullptr || c->kind != NodeKind::ContentControl) return false;
    const doc::Node* parent = doc::parentOf(doc(), id);
    if (parent == nullptr) return false;
    const NodeId parent_id = parent->id;
    int idx = -1;
    for (size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i]->id == id) {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (idx < 0) return false;

    // 结构操作的门：表单模式默认被 structureLock 锁住，文档模式放行（作者在设计表单）。
    if (!doc::canDoStructureOp(doc(), mode(), id).allowed) {
        noteBlocked();
        return false;
    }

    int lo = -1;
    int hi = -1;
    if (!flowIndex().intervalOf(id, &lo, &hi)) return false;

    std::vector<Step> steps;
    pending_before_ = Range{};
    pending_after_ = Range{};
    // 撤销恢复的是"删除之前光标在哪"：键盘那一路把原位置传进来（它可能在控件里、也可能
    //   在控件之前），不传时按"选中整个控件"记 —— 那是显式删控件按钮的叙事。
    pending_before_ = (caret_before >= 0) ? Range{caret_before, caret_before} : Range{lo, hi};
    pending_after_ = Range{lo, lo};

    if (keep_content) {
        // 拆壳：内容上提、节点移除。两个 span 的差就是节点自己那两枚边界。
        UnwrapNode u;
        u.at = Pos{parent_id, idx};
        u.at_flow = lo;
        u.span_before = hi - lo;
        u.span_after = hi - lo - 2;
        u.node = NodeSpec{};
        u.detached = id;
        steps.push_back(Step{u});
    } else {
        RemoveNode r;
        r.at = Pos{parent_id, idx};
        r.at_flow = lo;
        r.removed = NodeSpec{};
        r.detached = id;
        steps.push_back(Step{r});
    }

    const int dirty = controls().at(index).range.start.para;
    // 段数可能变（控件里的段落标记没了，或者上提之后换了容器），所以走全量重编译。
    if (!commit(std::move(steps), dirty)) {
        noteBlocked();
        return false;
    }
    // 落点 = 控件区间的起点：保留内容时那里正是它的第一枚内容。
    pos().setCaretAtFlow(lo);
    return true;
}

EditCommand::PropResult EditCommand::setControlProp(int index, ControlProp prop, bool value) {
    if (index < 0 || index >= controls().size()) return PropResult::Failed;
    const NodeId id = controlNodeIds()[static_cast<size_t>(index)];
    bool before = false;
    if (!readPropValue(doc::findNode(doc(), id), prop, &before)) return PropResult::Failed;
    // 幂等：没变就不提交 —— 撤销栈里多一条什么都没做的记录只会让人困惑。
    if (before == value) return PropResult::Unchanged;

    SetControlProp s;
    s.id = id;
    s.prop = prop;
    s.value = value;
    s.before = before;

    std::vector<Step> steps;
    pending_before_ = Range{};
    pending_after_ = Range{};
    steps.push_back(Step{s});
    // hidden 改呈现态的流结构（整棵摘除），其余三项不改结构。
    const bool structural = (prop == ControlProp::Hidden);
    const int dirty = controls().at(index).range.start.para;
    if (!commit(std::move(steps), dirty)) return PropResult::Failed;
    return structural ? PropResult::Structural : PropResult::Changed;
}

bool EditCommand::posAfterParagraph(int para, Pos* out) const {
    if (out == nullptr || !hasFlow()) return false;
    if (para < 0 || para >= static_cast<int>(blockSrc().size())) return false;
    const BlockSource& src = blockSrc()[static_cast<size_t>(para)];
    // 有标记时锚在标记上，没有时锚在段首元素上 —— 两者都指向"这一段所在的那一层"。
    const NodeId anchor = (src.mark != kInvalidNodeId) ? src.mark : src.first;
    const doc::Node* parent = doc::parentOf(doc(), anchor);
    if (parent == nullptr) return false;
    int slot = -1;
    for (size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i]->id == anchor) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) return false;
    // 有标记：落点是标记之后那一槽。没有标记：末尾那段之后就是所属容器的末尾 —— 用段首
    //   元素的槽位 +1 会在"这一段有多个 run"时插到段中间去。
    *out = (src.mark != kInvalidNodeId)
               ? Pos{parent->id, slot + 1}
               : Pos{parent->id, static_cast<int>(parent->children.size())};
    return true;
}

bool EditCommand::commitInsert(FlowPos at, const NodeSpec& spec, FlowPos focus,
                               doc::MergeRun merge) {
    if (at < 0 || at > flowIndex().size()) {
        noteBlocked();
        return false;
    }
    InsertNode in;
    in.at = flowIndex().posAt(at);
    in.at_flow = at;
    in.subtree = spec;

    std::vector<Step> steps;
    steps.push_back(Step{in});
    // 撤销回到"插入之前光标在哪"，不是回到插入点：两者在"光标停在段首、表格落在段末"这类
    //   位置上相差一整段，而撤销的语义是回到按下去之前那一拍。
    pending_before_ = pos().selection();
    pending_after_ = Range{focus, focus};
    // 起始脏段按插入点算：表格带来的新段就从那里开始，之后的内容整体后移。
    if (!commit(std::move(steps), pos().paraForFlow(at), merge)) {
        noteBlocked();
        return false;
    }
    pos().setCaretAtFlow(focus);
    return true;
}

bool EditCommand::insertTable(int rows, int cols, bool header) {
    if (!hasFlow()) return false;
    // 插入结构是设计动作：表单模式在填值、只读模式不许改，两者都不该长出新的表。
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    NodeSpec spec;
    if (!makeEmptyTableSpec(rows, cols, header, &spec)) {
        noteBlocked();
        return false;
    }

    Pos at{};
    if (!posAfterParagraph(pos().caret().para, &at)) {
        noteBlocked();
        return false;
    }
    const FlowPos at_flow = flowIndex().flowAt(at);
    if (at_flow < 0) {
        noteBlocked();
        return false;
    }
    // 光标落进第一格：接着打字就是填这一格（不是落在表之前或表之后）。
    const FlowPos focus = at_flow + kFirstTableCellOffset;
    const doc::Node* parent = doc::findNode(doc(), at.container);
    const bool tail = parent == nullptr || at.offset >= static_cast<int>(parent->children.size());
    if (!tail) return commitInsert(at_flow, spec, focus);

    // 表落在文档（或这一格）的末尾：补一段空的接在表后面。
    //   文档以表结尾时，表后面没有任何能停光标的地方，点空白处也只是回到上一段 ——
    //   "在表后接着写"就没有落点。两段插入算同一次操作（Start + Continue 合成一步撤销）。
    if (!commitInsert(at_flow, spec, focus, doc::MergeRun::Start)) return false;
    const doc::Node* host = doc::findNode(doc(), at.container);
    if (host == nullptr || at.offset >= static_cast<int>(host->children.size())) return true;
    FlowPos table_lo = 0;
    FlowPos table_hi = 0;
    if (!flowIndex().intervalOf(host->children[static_cast<size_t>(at.offset)]->id, &table_lo,
                                &table_hi)) {
        return true;
    }
    NodeSpec mark;
    mark.kind = NodeKind::ParagraphMark;
    InsertNode after;
    after.at = flowIndex().posAt(table_hi);
    after.at_flow = table_hi;
    after.subtree = mark;
    std::vector<Step> steps;
    steps.push_back(Step{after});
    const Range keep{focus, focus};
    pending_before_ = keep;
    pending_after_ = keep;
    if (commit(std::move(steps), pos().paraForFlow(table_hi), doc::MergeRun::Continue)) {
        pos().setCaretAtFlow(focus);
    }
    return true;
}

bool EditCommand::insertControl(const ControlSpec& spec, const std::string& label) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }

    // 只给了类型名的记录在这里补齐：值形态、只读、光标屏障与特有层都按类型定。
    //   调用方（wasm 绑定、原生判据）给的是"要哪一种控件"，不是一份填满的记录。
    ControlSpec s = spec;
    if (std::holds_alternative<std::monostate>(s.detail)) {
        const ControlSpec def = makeControlSpec(s.controlType);
        s.detail = def.detail;
        if (s.valueShape == ValueShape::Text) s.valueShape = def.valueShape;
        s.readonly = s.readonly || def.readonly;
    }
    // 单控件的标签是作者给的文案；组控件的标签来自选项表。
    if (auto* c = std::get_if<ChoiceDetail>(&s.detail)) {
        if (c->options.empty() && c->label.empty()) c->label = label;
    }

    // 文本类的初始内容就是 label；其余按记录生成（图形 + 标签、空的下拉与日期）。
    const std::string content =
        (s.valueShape == ValueShape::Text) ? label : controlDisplayText(s, s.value);

    NodeSpec node;
    node.kind = NodeKind::ContentControl;
    node.control = s;
    const std::vector<uint32_t> cps = utf8ToCodepoints(content);
    if (!cps.empty()) {
        NodeSpec run;
        run.kind = NodeKind::TextRun;
        run.text = cps;
        node.children.push_back(std::move(run));
    }

    const TextPos caret = pos().caret();
    const FlowPos at = pos().flowAt(caret);
    if (at < 0) {
        noteBlocked();
        return false;
    }
    // 控件是行内容器，不切断段落：落点始终在光标处。
    //
    //   光标摆哪儿分两种：可编辑的字段摆在内容末尾（"接着填这个字段"）；有屏障的
    //   （选择类、下拉、日期）摆在整块之后 —— 它们的内容里根本停不住，摆进去等于把
    //   光标塞进一个按不动的格子，下一次插入又会落进它的内容里，于是"连点两次"长出嵌套。
    const bool barrier = controlCaretBarrier(s);
    const FlowPos focus = at + 1 + static_cast<FlowPos>(cps.size()) + (barrier ? 1 : 0);
    return commitInsert(at, node, focus);
}

bool EditCommand::filterTypedText(FlowPos at_flow, const std::vector<uint32_t>& cps,
                                  std::vector<uint32_t>* out) const {
    if (out == nullptr) return false;
    out->clear();
    FlowPos lo = 0;
    FlowPos hi = 0;
    const int index = innermostControl(at_flow, &lo, &hi);
    const TextDetail* filter = nullptr;
    std::string current;
    if (index >= 0) {
        const doc::Node* n = doc::findNode(doc(), controlNodeIds()[static_cast<size_t>(index)]);
        if (n != nullptr) {
            const auto* spec = std::get_if<ControlSpec>(&n->payload);
            if (spec != nullptr) filter = textOf(*spec);
            for (const doc::Ref& c : n->children) {
                if (c->kind != NodeKind::TextRun) continue;
                current = codepointsToUtf8(std::get<doc::RunPayload>(c->payload).text);
                break;
            }
        }
    }
    for (uint32_t cp : cps) {
        if (filter != nullptr && filter->filter == ContentFilter::Number) {
            if (!numberAccepts(current, cp)) continue;
            current += codepointsToUtf8(std::vector<uint32_t>{cp});
        }
        out->push_back(cp);
    }
    return !out->empty();
}

bool EditCommand::buildContentReplace(NodeId id, const std::string& text,
                                      std::vector<Step>* steps) const {
    if (steps == nullptr) return false;
    const doc::Node* n = doc::findNode(doc(), id);
    if (n == nullptr) return false;
    // 只认"一段纯文本"：多个文本节点或混着别的元素时不动它 —— 猜一个形状重写会毁掉
    //   作者放进去的东西。生成的控件内容（图形 + 标签、格式化后的日期）都是这一段。
    const doc::Node* run = nullptr;
    for (const doc::Ref& c : n->children) {
        if (c->kind != NodeKind::TextRun || run != nullptr) return false;
        run = c.get();
    }
    const std::vector<uint32_t> want = utf8ToCodepoints(text);
    if (run == nullptr) {
        if (want.empty()) return true;
        NodeSpec spec;
        spec.kind = NodeKind::TextRun;
        spec.text = want;
        InsertNode in;
        in.at = Pos{id, 0};
        const FlowPos at = flowIndex().flowAt(in.at);
        if (at < 0) return false;
        in.at_flow = at;
        in.subtree = std::move(spec);
        steps->push_back(Step{in});
        return true;
    }
    const auto& payload = std::get<doc::RunPayload>(run->payload);
    if (payload.text == want) return true;
    if (!want.empty()) {
        SetRun sr;
        sr.id = run->id;
        sr.text = want;
        sr.marks = payload.marks;
        sr.styleRef = payload.style;
        sr.beforeText = payload.text;
        sr.before = payload.marks;
        sr.beforeStyleRef = payload.style;
        steps->push_back(Step{sr});
        return true;
    }
    // 内容清空：文本节点必须整枚摘掉 —— 规范化会把空文本节点删掉，留一个空节点等于
    //   留下一个下一次提交就会消失的中间态。
    RemoveNode rn;
    rn.at = Pos{id, 0};
    const FlowPos at = flowIndex().flowAt(rn.at);
    if (at < 0) return false;
    rn.at_flow = at;
    rn.removed.kind = NodeKind::TextRun;
    rn.removed.text = payload.text;
    rn.removed.marks = payload.marks;
    rn.removed.styleRef = payload.style;
    rn.detached = run->id;
    steps->push_back(Step{rn});
    return true;
}

bool EditCommand::setControlSpec(int index, const ControlPatch& patch) {
    if (!hasFlow()) return false;
    // 改定义是设计动作：版式与字段结构归作者，填表的人只碰值。
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    if (index < 0 || index >= static_cast<int>(controlNodeIds().size())) return false;
    const NodeId id = controlNodeIds()[static_cast<size_t>(index)];
    const doc::Node* n = doc::findNode(doc(), id);
    if (n == nullptr || n->kind != NodeKind::ContentControl) return false;
    const auto* spec = std::get_if<ControlSpec>(&n->payload);
    if (spec == nullptr) return false;

    ControlSpec next = *spec;
    if (!applyControlPatch(&next, patch, nullptr)) {
        noteBlocked();
        return false;
    }
    // 选项被删掉时，指向它的值跟着清掉 —— 留着就是"值不在选项表里"的脏数据。
    pruneControlValue(next, &next.value);

    std::vector<Step> steps;
    if (!(next == *spec)) {
        SetControlSpec s;
        s.id = id;
        s.spec = next;
        s.before = *spec;
        steps.push_back(Step{s});
    }
    // 文本类的值就是内容，改定义不该碰它（作者打的字不是配置的投影）。
    //   其余三类的内容由定义与值共同生成，定义变了就得整段重建。
    if (next.valueShape != ValueShape::Text) {
        if (!buildContentReplace(id, controlDisplayText(next, next.value), &steps)) {
            noteBlocked();
            return false;
        }
    }
    if (steps.empty()) return true;

    pending_before_ = Range{};
    pending_after_ = Range{};
    const int dirty = controls().at(index).range.start.para;
    return commit(std::move(steps), dirty);
}

bool EditCommand::setControlValue(int index, const ControlValue& value) {
    if (!hasFlow()) return false;
    if (index < 0 || index >= static_cast<int>(controlNodeIds().size())) return false;
    const NodeId id = controlNodeIds()[static_cast<size_t>(index)];
    const doc::Node* n = doc::findNode(doc(), id);
    if (n == nullptr || n->kind != NodeKind::ContentControl) return false;
    const auto* spec = std::get_if<ControlSpec>(&n->payload);
    if (spec == nullptr) return false;

    // 业务只读：填表的人只能看。光标那条路由 insertableAtFlow 挡着，而弹层（下拉、日期）
    //   与脚本走的正是这里 —— 不拦就等于只读只管得住键盘。
    if (spec->readonly) {
        noteBlocked();
        return false;
    }

    std::string error;
    if (!controlValueValid(*spec, value, &error)) {
        noteBlocked();
        return false;
    }

    std::vector<Step> steps;
    if (spec->valueShape == ValueShape::Text) {
        // 文本类的值就是内容：比较与写入都落在内容上，记录上的 value 不参与。
        if (const TextDetail* td = textOf(*spec)) {
            if (td->filter == ContentFilter::Number && !numberTextValid(value.text)) {
                noteBlocked();
                return false;
            }
        }
        if (!buildContentReplace(id, value.text, &steps)) {
            noteBlocked();
            return false;
        }
    } else {
        if (spec->value == value) return true;
        // 内容先换：它就是值的显示投影，两者在同一次提交里一起变。
        if (!buildContentReplace(id, controlDisplayText(*spec, value), &steps)) {
            noteBlocked();
            return false;
        }
        SetControlValue scv;
        scv.id = id;
        scv.value = value;
        scv.before = spec->value;
        steps.push_back(Step{scv});
    }
    if (steps.empty()) return true;

    pending_before_ = Range{};
    pending_after_ = Range{};
    const int dirty = controls().at(index).range.start.para;
    return commit(std::move(steps), dirty);
}

bool EditCommand::toggleControl(int index, int option) {
    if (!hasFlow()) return false;
    if (index < 0 || index >= static_cast<int>(controlNodeIds().size())) return false;
    const NodeId id = controlNodeIds()[static_cast<size_t>(index)];
    const doc::Node* n = doc::findNode(doc(), id);
    if (n == nullptr || n->kind != NodeKind::ContentControl) return false;
    const auto* spec = std::get_if<ControlSpec>(&n->payload);
    if (spec == nullptr) return false;
    const auto* choice = choiceOf(*spec);
    if (choice == nullptr) return false;

    // 与 setControlValue 同一条：业务只读的控件勾不动。
    if (spec->readonly) {
        noteBlocked();
        return false;
    }

    // 先算出要改的每一处（控件, 新值），再一次性提交：互斥组里"取消别人"与"选自己"
    //   必须是同一个撤销步，否则撤销一下会回到"两个都选中"的中间态。
    std::vector<std::pair<NodeId, ControlValue>> changes;
    if (choice->options.empty()) {
        ControlValue v = spec->value;
        // 没配互斥组的单选是一个独立开关：点一下选、再点一下取消。配了组才按单选语义
        //   只许选中（同组里由下面那段把别人取消）；复选本来就是开关。
        v.flag = (choice->multi || choice->group.empty()) ? !v.flag : true;
        changes.emplace_back(id, v);
        if (!choice->multi && !choice->group.empty()) {
            for (const NodeId other : controlNodeIds()) {
                if (other == id) continue;
                const doc::Node* on = doc::findNode(doc(), other);
                if (on == nullptr) continue;
                const auto* os = std::get_if<ControlSpec>(&on->payload);
                if (os == nullptr) continue;
                const auto* oc = choiceOf(*os);
                if (oc == nullptr || oc->multi || oc->options.size() > 0) continue;
                if (oc->group != choice->group || !os->value.flag) continue;
                ControlValue ov = os->value;
                ov.flag = false;
                changes.emplace_back(other, ov);
            }
        }
    } else {
        if (option < 0 || option >= static_cast<int>(choice->options.size())) return false;
        const std::string& val = choice->options[static_cast<size_t>(option)].value;
        ControlValue v = spec->value;
        if (choice->multi) {
            bool found = false;
            std::vector<std::string> kept;
            for (const std::string& item : v.items) {
                if (item == val) {
                    found = true;
                } else {
                    kept.push_back(item);
                }
            }
            if (!found) kept.push_back(val);
            v.items = std::move(kept);
            v.text.clear();
        } else {
            v.text = val;
            v.items.clear();
        }
        changes.emplace_back(id, v);
    }

    std::vector<Step> steps;
    for (const auto& ch : changes) {
        const doc::Node* target = doc::findNode(doc(), ch.first);
        if (target == nullptr) continue;
        const auto* ts = std::get_if<ControlSpec>(&target->payload);
        if (ts == nullptr) continue;
        if (!buildContentReplace(ch.first, controlDisplayText(*ts, ch.second), &steps)) {
            noteBlocked();
            return false;
        }
        SetControlValue s;
        s.id = ch.first;
        s.value = ch.second;
        s.before = ts->value;
        steps.push_back(Step{s});
    }
    if (steps.empty()) return true;

    pending_before_ = Range{};
    pending_after_ = Range{};
    const int dirty = controls().at(index).range.start.para;
    return commit(std::move(steps), dirty);
}

bool EditCommand::insertImage(const ImageSpec& spec) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    // 既没有资源、也没有像素尺寸与显示尺寸时拒绝：排版会退回一个 4:3 的默认框，
    //   画面上是一个"不知道多大的空框"，而调用方以为图插进去了。
    const bool sized = spec.px_w > 0 && spec.px_h > 0;
    const bool authored =
        spec.width.kind != LengthKind::Auto || spec.height.kind != LengthKind::Auto;
    if (spec.resource_id.empty() && !sized && !authored) {
        noteBlocked();
        return false;
    }

    NodeSpec node;
    node.kind = NodeKind::Image;
    node.image = spec;
    const TextPos caret = pos().caret();
    const FlowPos at = pos().flowAt(caret);
    if (at < 0) {
        noteBlocked();
        return false;
    }
    // 图片占一个流位置，光标落到它之后 —— 紧接着敲字就是"图后面那段文字"。
    return commitInsert(at, node, at + 1);
}

bool EditCommand::resizeImage(NodeId id, float w_pt, float h_pt, bool continue_run) {
    if (!(w_pt > 0.0f) || !(h_pt > 0.0f)) return false;
    // 改图片尺寸是设计动作：表单模式在填值、只读模式不许改，两者都不该动版式。
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const doc::Node* n = doc::findNode(doc(), id);
    if (n == nullptr || n->kind != NodeKind::Image) return false;
    const auto* im = std::get_if<ImageSpec>(&n->payload);
    if (im == nullptr) return false;

    // 幂等按磅比：长度落在 EMU 上，浮点换算回来可能差一位 —— 差一位不该留下一个
    //   什么都没改的撤销步骤。
    auto ptOf = [](const Length& len) {
        return len.kind == LengthKind::Abs ? emuToPt(len.abs_emu) : -1.0;
    };
    if (std::fabs(ptOf(im->width) - w_pt) < 0.01 && std::fabs(ptOf(im->height) - h_pt) < 0.01) {
        return true;
    }

    SetImageSize s;
    s.id = id;
    s.width = Length::abs(ptToEmu(w_pt));
    s.height = Length::abs(ptToEmu(h_pt));
    s.beforeWidth = im->width;
    s.beforeHeight = im->height;

    std::vector<Step> steps;
    steps.push_back(Step{s});
    // 改尺寸不动光标：撤销与重做都该回到按下去之前那一拍的位置。
    //   留空 Range 会被当成"文档开头"，表现是缩几次再撤销，光标跳到全文最前。
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    // 图片在这一段里：行盒变高，从这一段起后面全部要重排。
    const FlowPos at = flowIndex().flowAt(Pos{id, 0});
    const int dirty = (at >= 0) ? pos().paraForFlow(at) : 0;
    return commit(std::move(steps), dirty,
                  continue_run ? doc::MergeRun::Continue : doc::MergeRun::Start);
}

}  // namespace heditor
