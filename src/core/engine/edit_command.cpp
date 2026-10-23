/**
 * 命令层的实现：一次编辑怎么变成一条事务（声明与边界见 edit_command.h）。
 *
 * 插入与删除先过落点判定（policyAt / policyErase），再按字素簇吸附；跨段编辑（分段、合并）
 * 与结构操作（删控件、改控件属性）走同一条"造事务 → 交出去"的路 —— 于是撤销、重排、重绘、
 * 记账自动一致，不必另开一条特权通道。
 */
#include "core/engine/edit_command.h"

#include "core/doc/subtree.h"

#include <algorithm>
#include <functional>
#include <variant>

namespace heditor {

namespace {

/**
 * 一步删除的起点（把整批删除步骤从后往前排时用）。
 * 只认两种删除步骤 —— 这个序列里不会有别的。
 */
FlowPos stepStartFlow(const Step& s) {
    if (const auto* d = std::get_if<DeleteRange>(&s)) return d->begin;
    if (const auto* r = std::get_if<RemoveParaMark>(&s)) return r->at;
    if (const auto* n = std::get_if<RemoveNode>(&s)) return n->at_flow;
    return -1;
}

/**
 * 读控件的运行时属性。四个分居两处：hidden 在节点上（呈现态的流位置换算要看它），
 * 其余三个在控件数据里。
 */
bool readPropValue(const rev::Node* n, ControlProp p, bool* out) {
    if (n == nullptr || n->kind != NodeKind::ContentControl || out == nullptr) return false;
    const auto* spec = std::get_if<ControlSpec>(&n->payload);
    if (spec == nullptr) return false;
    switch (p) {
        case ControlProp::Hidden:
            *out = n->hidden;
            return true;
        case ControlProp::Readonly:
            *out = spec->readonly;
            return true;
        case ControlProp::Required:
            *out = spec->required;
            return true;
        case ControlProp::StructureLock:
            *out = spec->structureLock;
            return true;
    }
    return false;
}

}  // namespace

namespace {

/**
 * 一枚原子整枚删掉：按流位置寻址。原子没有可寻址的内部，它在流里只占自己那一格。
 */
NodeBuild removeAtomStep(const rev::Doc& doc, const rev::FlowIndex& idx, FlowPos at) {
    NodeBuild out;
    if (at < 0 || at >= idx.size()) {
        out.error = "删除原子：流位置越界";
        return out;
    }
    const Pos p = idx.posAt(at);
    const rev::Node* n = rev::findNode(doc, p.container);
    if (n == nullptr || !isAtom(n->kind) || p.offset != 0) {
        out.error = "删除原子：该位置上没有原子";
        return out;
    }
    RemoveNode r;
    r.at_flow = at;
    out.step = Step{r};
    out.ok = true;
    return out;
}

/**
 * 删一枚段落标记。格式与样式引用从文档里读出来带上 —— 撤销要靠它还原，手填一份等于把
 * "这一段长什么样"记两遍。
 */
ParaMarkBuild removeMarkStep(const rev::Doc& doc, const rev::FlowIndex& idx, FlowPos at) {
    ParaMarkBuild out;
    if (at < 0 || at >= idx.size()) {
        out.error = "删除段落标记：流位置越界";
        return out;
    }
    const Pos p = idx.posAt(at);
    const rev::Node* n = rev::findNode(doc, p.container);
    if (n == nullptr || !isParagraphMark(n->kind) || p.offset != 0) {
        out.error = "删除段落标记：该位置上没有段落标记";
        return out;
    }
    const auto* mp = std::get_if<rev::MarkPayload>(&n->payload);
    if (mp == nullptr) {
        out.error = "删除段落标记：载荷不是段落标记";
        return out;
    }
    RemoveParaMark s;
    s.at = at;
    s.para = mp->para;
    s.styleRef = mp->style;
    out.step = Step{s};
    out.ok = true;
    return out;
}

/**
 * 删一个流区间：同层且只含文本与段落标记时一条 DeleteRange 就够。
 *
 * 区间里出现整块元素或跨层时返回空，由调用方另走结构步骤（删原子、删整表）。
 */
std::vector<Step> deleteWholeRange(const rev::Doc& doc, const rev::FlowIndex& idx, FlowPos from,
                                   FlowPos to) {
    if (from >= to) return {};
    if (from < 0 || to > idx.size()) return {};
    const Pos b = idx.posAt(from);
    const rev::Node* bn = rev::findNode(doc, b.container);
    if (bn == nullptr) return {};
    const rev::Node* parent = isContainer(bn->kind) ? bn : rev::parentOf(doc, bn->id);
    if (parent == nullptr) return {};

    // 覆盖总和必须恰好等于区间长：跨层（区间伸出了这一层的孩子表）或中间有洞时
    //   不满足，宁可返回空让调用方另走结构步骤，也不要发出半截区间 —— 那一类症状是
    //   "选中了整张表，只删掉第一格的字"。
    int covered = 0;
    for (const rev::Ref& c : parent->children) {
        int f = 0;
        int e = 0;
        if (!idx.intervalOf(c->id, &f, &e)) continue;
        if (e <= from || f >= to) continue;
        if (!isText(c->kind) && !isParagraphMark(c->kind)) return {};
        covered += (e < to ? e : to) - (f > from ? f : from);
    }
    if (covered != to - from) return {};

    DeleteRange s;
    s.begin = from;
    s.end = to;
    return {Step{s}};
}

/**
 * 按文本节点切分的删除步骤：每个被区间盖到的 run 各发一条，原子与段落标记不产生步骤
 * （它们由调用方另发结构步骤）。从后往前发 —— 删右边不会移动左边的坐标。
 *
 * 逐节点跳跃而不是逐位问 posAt：后者每次都要从根下降、每层线性扫孩子，整篇删除是亿级。
 */
std::vector<Step> deleteTextSteps(const rev::Doc& doc, const rev::FlowIndex& idx, FlowPos from,
                                  FlowPos to) {
    std::vector<Step> out;
    if (from < 0 || to <= from || to > idx.size()) return out;
    std::vector<std::pair<FlowPos, FlowPos>> segs;
    FlowPos p = from;
    while (p < to) {
        const Pos np = idx.posAt(p);
        const rev::Node* n = rev::findNode(doc, np.container);
        int nf = 0;
        int ne = 0;
        if (n == nullptr || !isText(n->kind) || !idx.intervalOf(n->id, &nf, &ne) || ne <= p) {
            ++p;  // 容器边界、段落标记、原子：这一位不产生步骤
            continue;
        }
        const int seg_to = (ne < to) ? ne : to;
        if (seg_to > p) segs.push_back({p, seg_to});
        p = ne;
    }
    out.reserve(segs.size());
    for (size_t i = segs.size(); i-- > 0;) {
        DeleteRange s;
        s.begin = segs[i].first;
        s.end = segs[i].second;
        out.push_back(Step{s});
    }
    return out;
}

}  // namespace
/**
 * 落点判定：走模型层的能力表（树 + 流位置）。
 * 六个编辑入口与四个光标入口都调它，免得十来处各写一遍。
 */
rev::EditCheck EditCommand::policyAt(TextPos p) const {
    if (formDegraded()) return rev::EditCheck{true, ""};
    // 侧别由位置自己推（光标这一处用光标自己的侧别，见 sideForPos）——
    //   不从控件下标反推：同一个偏移上可能压着好几枚括号，反推会猜错。
    const FlowPos f = pos().flowPosAtSide(p.para, p.offset, pos().sideForPos(p));
    if (f < 0) return rev::EditCheck{false, "落点不在文档流里"};
    return rev::canEditAt(doc(), mode(), f);
}

rev::EditCheck EditCommand::policyErase(TextPos a, TextPos b) const {
    // 档 1 的删除不跨段；段落标记的删除走合并那条路（另有判定）。
    if (a.para != b.para) return rev::EditCheck{false, "删除区间跨段"};
    if (formDegraded()) return rev::EditCheck{true, ""};
    // 区间 = "这段文本码点自己占的那一段"，与实际删除（eraseOnTree）同一把尺子。
    //   端点取错侧就会被判成越出控件而拒掉，表象是"删到只剩一个字就删不动了"；
    //   取宽了又会把相邻的括号圈进来，表象是"删完光标跳一格"。
    int rel_from = 0;
    int rel_to = 0;
    if (!pos().textSpanAt(a.para, a.offset, b.offset, &rel_from, &rel_to))
        return rev::EditCheck{false, "删除区间不在文档流里"};
    const FlowPos base = blockFlow()[static_cast<size_t>(a.para)];
    return rev::canEditRange(doc(), mode(), Range{base + rel_from, base + rel_to});
}

/**
 * 表单模式的安全阀：一份"一个可编辑控件都没有"的文档退化为文档模式，而不是什么都不让改。
 *
 * 理由：一个还没配字段的文档切到表单模式，用户期望的是"跟平时一样"，而不是突然一个字都
 *   打不进去 —— 后者会被当成功能坏了，没人会想到是策略在生效。它是个刻意的安全阀，不是
 *   遗漏；将来若要做"严格表单"，应该另加一个 EditMode，不要改这里。
 *
 * 判据只看导出后的控件集合（隐藏控件在呈现态本就不在里面），有一个非只读的就为真。
 */
bool EditCommand::formDegraded() const { return mode() == EditMode::Form && !hasEditableControl(); }

bool EditCommand::hasEditableControl() const {
    for (int i = 0; i < controls().size(); ++i) {
        if (!controls().at(i).read_only) return true;
    }
    return false;
}

bool EditCommand::insertCodepoints(int para, size_t offset, const std::vector<uint32_t>& cps) {
    if (cps.empty()) return false;
    if (para < 0 || para >= static_cast<int>(blocks().size())) return false;
    // 落点就是光标（打字与缩进都走这里）而选区非空：这一次输入是"替换选中内容"。
    //   落到别处的插入（程序化插入）不动选区 —— 那种调用的坐标是按调用时的文档算出来
    //   的，删掉一段会让它整批一起过期。
    if (pos().hasSelection()) {
        const TextPos here = pos().caret();
        if (para == here.para && offset == here.offset) return replaceSelectionWithText(cps);
    }
    // 插入前必须吸附：否则一串 e + U+0301 中间被插进字符，基字与标记就分家了。
    const size_t at = pos().snapToCluster(para, offset);

    // 策略判定用的是吸附之后的位置 —— 判定用的位置和真正落笔的位置
    //   必须是同一个。否则会出现"策略放行了一个它根本没看过的位置"这种漏洞：
    //   吸附把 at 挪到簇边界之后，落笔点可能已经跨到另一个控件里去了。
    const TextPos ins{para, at};
    if (!policyAt(ins).allowed) {
        noteBlocked();
        return false;
    }

    // 落笔在树上：插入是一条步骤，经事务提交，块流随后由新树重编译出来。
    // 落点的流位置由 (偏移, 侧别) 给出 —— 侧别取光标自己存的那个。
    //   从控件下标反推（老的 flowPosAt）在多枚括号压在同一个偏移上时会猜错：空嵌套里
    //   "最后一枚右括号之前"会被猜成"第一枚左括号之后"，字与光标一起插到别处去。
    const FlowPos at_flow = pos().flowPosAtSide(para, at, pos().sideForPos(ins));
    if (at_flow < 0) {
        noteBlocked();
        return false;
    }
    InsertText s;
    s.at = at_flow;
    s.cps = cps;
    // 沿用落点左侧的字符格式：否则在粗体字中间敲出来的字不是粗的。
    s.marks = marksAtFlow(at_flow, &s.styleRef);
    std::vector<Step> steps;
    pending_before_ = Range{};
    pending_after_ = Range{};
    pending_before_ = Range{at_flow, at_flow};
    const FlowPos after = at_flow + static_cast<FlowPos>(cps.size());
    pending_after_ = Range{after, after};
    steps.push_back(Step{s});
    if (!commit(std::move(steps), para)) {
        noteBlocked();
        return false;
    }
    // 落点 = 插入区间的终点，事务里已经算好了（tx.selection_after）—— 直接摆过去，
    //   不再"丢成一个码点偏移再猜侧别"。
    // 段号不变、段内码点偏移整体后移：控件的开闭括号会跟着内容走，而段的码点里
    //   没有它们，所以区间端点不必手工修。
    pos().setCaretAtFlow(after);
    return true;
}

bool EditCommand::insertCodepoint(int para, size_t offset, uint32_t cp) {
    return insertCodepoints(para, offset, std::vector<uint32_t>{cp});
}

bool EditCommand::insertTextUtf8(int para, size_t offset, const std::string& utf8) {
    return insertCodepoints(para, offset, utf8ToCodepoints(utf8));
}

bool EditCommand::typeTextUtf8(const std::string& utf8) {
    return insertCodepoints(pos().caret().para, pos().caret().offset, utf8ToCodepoints(utf8));
}

bool EditCommand::controlFlowSpan(int index, FlowPos* lo, FlowPos* hi) const {
    if (!hasFlow()) return false;
    if (index < 0 || index >= static_cast<int>(controlNodeIds().size())) return false;
    int first = -1;
    int end = -1;
    if (!flowIndex().intervalOf(controlNodeIds()[static_cast<size_t>(index)], &first, &end))
        return false;
    if (lo != nullptr) *lo = first;
    if (hi != nullptr) *hi = end;
    return true;
}

bool EditCommand::controlIsEmpty(int index) const {
    FlowPos lo = 0;
    FlowPos hi = 0;
    if (!controlFlowSpan(index, &lo, &hi)) return false;
    // 容器的两枚边界各占一格，中间什么都没有时区间恰好两格宽。
    return hi - lo == 2;
}

int EditCommand::controlEndingAt(FlowPos p) const {
    for (int i = 0; i < static_cast<int>(controlNodeIds().size()); ++i) {
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!controlFlowSpan(i, &lo, &hi)) continue;
        if (hi == p) return i;
    }
    return -1;
}

int EditCommand::controlStartingAt(FlowPos p) const {
    for (int i = 0; i < static_cast<int>(controlNodeIds().size()); ++i) {
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!controlFlowSpan(i, &lo, &hi)) continue;
        if (lo == p) return i;
    }
    return -1;
}

/**
 * 退格贴着控件括号时的四种情形（从左到右的书写方向）。
 *
 * 光标在闭括号右面：控件有内容 ⇒ 光标进控件（落到内容末尾），再删掉左边那一项；空 ⇒ 删控件。
 * 光标在开括号右面：控件有内容 ⇒ 光标退到开括号左面，一个字都不删；空 ⇒ 删控件
 *   （空控件的内部就这一格，两条规则落在同一个位置上）。
 *
 * "删掉左边那一项"只有两种东西：一个字符，或者一个空控件。左边是个有内容的控件时，就按
 * 同一套规则再往里进一层 —— 嵌套不是特例，是这个循环自然走出来的（参考实现 DCWriterLite
 * 的 DocumentControler.Backspace 也是拿 while 一路进入）。选"进入"而不是"直接删掉整个
 * 内层控件"，是因为后者会把用户没打算删的内容一起带走。
 */
bool EditCommand::eraseAtomBeside(FlowPos cur, bool left) {
    if (!hasFlow() || cur < 0) return false;
    const FlowPos f = left ? (cur - 1) : cur;
    if (f < 0 || f >= flowIndex().size()) return false;
    const Pos p = flowIndex().posAt(f);
    const rev::Node* n = rev::findNode(doc(), p.container);
    if (n == nullptr || !isAtom(n->kind) || p.offset != 0) return false;
    if (!formDegraded() && !rev::canEditRange(doc(), mode(), Range{f, f + 1}).allowed) {
        noteBlocked();
        return false;
    }
    const NodeBuild nb = removeAtomStep(doc(), flowIndex(), f);
    if (!nb.ok) return false;
    std::vector<Step> steps;
    pending_before_ = Range{};
    pending_after_ = Range{};
    // 撤销恢复的是"按键之前光标在哪"（与删控件同一条）：记成原子自己那一格的话，撤销之后
    //   光标会落到原子前面，而按键之前它在后面。
    pending_before_ = Range{cur, cur};
    pending_after_ = Range{f, f};
    steps.push_back(nb.step);
    if (!commit(std::move(steps), pos().paraForFlow(f))) return false;
    pos().setCaretAtFlow(f);
    return true;
}

EditCommand::BracketHit EditCommand::bracketBackspace(FlowPos* at) {
    if (at == nullptr || !hasFlow() || controls().empty()) return BracketHit::None;
    *at = -1;
    const FlowPos cur = pos().selection().focus;

    const int open_before = controlStartingAt(cur - 1);
    if (open_before >= 0) {
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!controlFlowSpan(open_before, &lo, &hi)) return BracketHit::None;
        if (controlIsEmpty(open_before) &&
            removeControl(open_before, /*keep_content=*/false, cur)) {
            return BracketHit::Handled;
        }
        // 有内容，或者空控件被结构锁挡住：两种都退到开括号左面，不删任何东西。
        *at = lo;
        return BracketHit::MoveOnly;
    }

    FlowPos p = cur;
    for (int depth = 0; depth < 64; ++depth) {
        const int ci = controlEndingAt(p);
        if (ci < 0) break;
        if (controlIsEmpty(ci)) {
            if (removeControl(ci, /*keep_content=*/false, cur)) return BracketHit::Handled;
            FlowPos lo = 0;
            FlowPos hi = 0;
            if (!controlFlowSpan(ci, &lo, &hi)) return BracketHit::None;
            *at = lo;  // 删不掉（结构锁）：至少让光标退出控件，别吞掉这一下
            return BracketHit::MoveOnly;
        }
        p -= 1;  // 进入控件：光标落到闭括号左面，也就是内容末尾
    }
    if (p == cur) return BracketHit::None;
    *at = p;
    return BracketHit::MoveThenErase;
}

/**
 * 前删贴着控件括号时的四种情形，与退格左右对称。
 *
 * 光标在开括号左面：有内容 ⇒ 光标进到开括号右面，删掉右边那一项；空 ⇒ 删掉控件。
 * 光标在闭括号左面：有内容 ⇒ 光标跳到闭括号右面（跳出控件），删掉右边那一项；空 ⇒ 删掉控件。
 *
 * 跳出之后右边可能又是一枚闭括号（内层控件正好是外层的最后一项），那就继续往外跳 ——
 * 括号本身没有"删掉它"这个动作，能删的永远是内容。
 */
EditCommand::BracketHit EditCommand::bracketDeleteForward(FlowPos* at) {
    if (at == nullptr || !hasFlow() || controls().empty()) return BracketHit::None;
    *at = -1;
    const FlowPos cur = pos().selection().focus;

    // 闭括号左面：闭括号占 cur 那一格，于是这个控件的区间终点是 cur + 1。
    int ci = controlEndingAt(cur + 1);
    if (ci < 0) ci = controlStartingAt(cur);  // 开括号左面
    if (ci < 0) return BracketHit::None;
    if (controlIsEmpty(ci)) {
        if (removeControl(ci, /*keep_content=*/false, cur)) return BracketHit::Handled;
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!controlFlowSpan(ci, &lo, &hi)) return BracketHit::None;
        *at = lo;
        return BracketHit::MoveOnly;
    }

    FlowPos p = cur + 1;
    for (int depth = 0; depth < 64; ++depth) {
        const int inner = controlStartingAt(p);
        if (inner >= 0) {
            if (controlIsEmpty(inner)) {
                // 右边紧挨着一个空控件：删掉它（"删一个字符或空控件"里的后半句）。
                if (removeControl(inner, /*keep_content=*/false, cur)) return BracketHit::Handled;
                FlowPos lo = 0;
                FlowPos hi = 0;
                if (!controlFlowSpan(inner, &lo, &hi)) return BracketHit::None;
                *at = lo;
                return BracketHit::MoveOnly;
            }
            p += 1;  // 进入右边那个控件，删它的第一个字
            continue;
        }
        if (controlEndingAt(p + 1) >= 0) {
            p += 1;  // 右边是一枚闭括号：跳出去，接着往外找
            continue;
        }
        break;
    }
    *at = p;
    return BracketHit::MoveThenErase;
}

bool EditCommand::backspace() {
    if (blocks().empty()) return false;
    // 选区非空时退格是"删掉选区"，不再往前多删一个字 —— 这是最容易抄错的一条：
    //   抄成"先折叠选区再退格"的症状是用户删掉三个字，却连带把选区前面那个字也吃了。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (!plan.steps.empty()) return commitErase(plan, "退格");
    const FlowPos caret_before = pos().selection().focus;

    // 紧邻的是一枚原子就整枚删掉（见 eraseAtomBeside）。它排在括号规则之前：两者不会同时
    //   成立，而"左边那一格是原子"比"光标贴在一枚括号上"更贴近屏幕上看到的那个东西。
    if (eraseAtomBeside(caret_before, /*left=*/true)) return true;

    // 控件边界上的退格（四条规则见 bracketBackspace）。这一段必须排在"段首退格 = 并入
    //   上一段"之前：控件跨段时它的闭括号落在下一段的段首，从段首那条路走会并段，
    //   而不是按括号规则处理。
    FlowPos at = -1;
    const BracketHit hit = bracketBackspace(&at);
    if (hit == BracketHit::Handled) return true;
    if (hit == BracketHit::MoveOnly) {
        if (at >= 0) pos().setCaretAtFlow(at);
        return true;
    }
    if (hit == BracketHit::MoveThenErase && at >= 0) pos().setCaretAtFlow(at);

    TextPos p = pos().caret();
    if (p.para < 0 || p.para >= static_cast<int>(blocks().size())) p.para = 0;
    p.offset = pos().snapToCluster(p.para, p.offset);

    // 段首退格 = 并入上一段（跨段编辑）。策略拒绝时返回 false，与其它被拒编辑同一口径：
    //   宿主据此判断"没变"，光标也不会动。按括号规则搬过光标的那一下同样算变过。
    if (p.offset == 0)
        return mergeWithPreviousParagraph() || caret_before != pos().selection().focus;

    // 往回删一个簇，不是删一个码点。
    //   e + U+0301 会被一次删掉；只删 U+0301 会留下一个孤立的组合重音。
    const size_t at_cp = pos().snapToCluster(p.para, p.offset - 1);
    // 策略：整个删除区间必须落在同一个可编辑控件内。
    // 这一条同时挡掉两个方向 —— 从控件外往里删、从控件里往外删。
    if (!policyErase(TextPos{p.para, at_cp}, TextPos{p.para, p.offset}).allowed) {
        noteBlocked();
        // 光标已经搬过了（例如刚按括号规则进入控件）：搬了就是变了，别让宿主跳过重绘。
        return caret_before != pos().selection().focus;
    }
    // at_cp 与 p.offset 同段、且 at_cp < p.offset（簇吸附保证的），所以这是一个良构的
    //   删除区间。光标在右端：撤销时它要回到被删的那个字后面。
    return eraseOnTree(TextPos{p.para, at_cp}, TextPos{p.para, p.offset}, "退格",
                       /*caret_at_end=*/true);
}

bool EditCommand::deleteForward() {
    if (blocks().empty()) return false;
    // 与退格同一条：选区非空时只删选区，不再往后多删一个字。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (!plan.steps.empty()) return commitErase(plan, "前删");
    const FlowPos caret_before = pos().selection().focus;

    // 与退格左右对称：右边那一格是原子就整枚删掉。
    if (eraseAtomBeside(caret_before, /*left=*/false)) return true;

    FlowPos at = -1;
    const BracketHit hit = bracketDeleteForward(&at);
    if (hit == BracketHit::Handled) return true;
    if (hit == BracketHit::MoveOnly) {
        if (at >= 0) pos().setCaretAtFlow(at);
        return true;
    }
    if (hit == BracketHit::MoveThenErase && at >= 0) pos().setCaretAtFlow(at);

    TextPos p = pos().caret();
    if (p.para < 0 || p.para >= static_cast<int>(blocks().size())) p.para = 0;
    p.offset = pos().snapToCluster(p.para, p.offset);

    const size_t len = blocks()[static_cast<size_t>(p.para)].cp_text.size();
    // 段末 Delete = 把下一段并进来（与"段首退格并入上一段"对称）。
    if (p.offset >= len) return mergeWithNextParagraph() || caret_before != pos().selection().focus;

    size_t next = p.offset + 1;
    const size_t ceil_cp = pos().clusterCeilAt(p.para, p.offset);
    if (ceil_cp > p.offset) next = ceil_cp;
    if (next > len) next = len;

    if (!policyErase(TextPos{p.para, p.offset}, TextPos{p.para, next}).allowed) {
        noteBlocked();
        return caret_before != pos().selection().focus;
    }

    return eraseOnTree(TextPos{p.para, p.offset}, TextPos{p.para, next}, "前删");
}

/**
 * 回车：在光标处把这一段拆成两段（跨段编辑）。
 *
 * 新段的样式复制自原段（字体、字号、书写模式、行距、段后距、颜色、回退链、语言），
 * 只有 page_break_before 刻意清掉：段落分隔符不该附带一次翻页 ——
 * 否则在文档中间按一次回车会把后面整篇推到新页上。
 */
bool EditCommand::splitParagraph() {
    if (blocks().empty()) return false;
    // 选区非空时回车是"删掉选区，再在落点分段"。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    // 删完就往下走，而不是删完即止：回车与退格不同，它要的是"在这里断开"。
    // 已经删过之后，下面任何一步被拒都得报"变过"（consumed）—— 报假会让宿主跳过
    //   重绘，症状是"选中的内容已经没了，屏幕上还留着"。
    const bool consumed = !plan.steps.empty();
    if (consumed && !commitErase(plan, "分段")) return false;
    TextPos p = pos().caret();
    if (p.para < 0 || p.para >= static_cast<int>(blocks().size())) p.para = 0;
    p.offset = pos().snapToCluster(p.para, p.offset);

    // 分段 = 在断点处插入一个段落分隔符，所以判据与"在这里插字"完全相同。
    //   判定用的位置与真正插标记的位置必须是同一个，安全阀也只该有一处。
    if (!policyAt(p).allowed) {
        noteBlocked();
        return consumed;
    }

    // 新标记插在光标处，于是它结束的是前半段：段落属性整体沿用原段（新段必须与原段
    //   同格式），只有一条例外 —— 拆出来的后半段不该跟着强制分页。
    //   后半段由被替代的那枚标记结束，所以那一条例外要落到它身上；原段不带强制
    //   分页时不必动它（那样会平白多出一条内联覆盖）。
    const NodeId mark_a = (p.para < static_cast<int>(blockSrc().size()))
                              ? blockSrc()[static_cast<size_t>(p.para)].mark
                              : kInvalidNodeId;
    const rev::Node* mk = rev::findNode(doc(), mark_a);
    ParaFormatPatch patch;
    std::string style_ref;
    if (mk != nullptr) {
        if (const auto* mp = std::get_if<rev::MarkPayload>(&mk->payload)) {
            patch = mp->para;
            style_ref = mp->style;
        }
    }
    // 位置由 (偏移, 侧别) 给出，不从控件下标反推。
    //   反推在空控件处会猜错侧别，于是标记插到了别的控件里（"回车之后光标跑进第三个控件"）。
    const FlowPos at_flow = pos().flowPosAtSide(p.para, p.offset, pos().sideForPos(p));
    if (at_flow < 0) {
        noteBlocked();
        return consumed;
    }
    std::vector<Step> steps;
    pending_before_ = Range{};
    pending_after_ = Range{};
    pending_before_ = Range{at_flow, at_flow};
    pending_after_ = Range{at_flow + 1, at_flow + 1};
    InsertParaMark s;
    s.at = at_flow;
    s.para = patch;
    s.styleRef = style_ref;
    steps.push_back(Step{s});
    if (mk != nullptr && isParagraphMark(mk->kind) &&
        blocksFull()[static_cast<size_t>(p.para)].page_break_before) {
        SetParaMarkFormat clear_pbb;
        clear_pbb.id = mark_a;
        clear_pbb.para = patch;
        clear_pbb.para.page_break_before = false;
        clear_pbb.styleRef = style_ref;
        clear_pbb.before = patch;
        clear_pbb.beforeStyleRef = style_ref;
        steps.push_back(Step{clear_pbb});
    }
    if (!commit(std::move(steps), p.para)) {
        noteBlocked();
        return consumed;
    }
    // 新标记之后那一位 = 新段的第一个位置。段数变了，后缀复用随之作废（见 EditHost::commitEdit）。
    pos().setCaretAtFlow(at_flow + 1);
    return true;
}

/**
 * 合并的一般形式：把 head + 1 段并进 head 段。三种入口（段首退格 / 段末 Delete /
 * 显式调用）只是"从哪一端触发"不同，落到文档上的操作是同一个。
 */
bool EditCommand::mergeParagraphInto(int head) {
    if (head < 0 || head + 1 >= static_cast<int>(blocks().size())) return false;
    // 两块不在同一格：中间隔的是单元格边界，不是可删的段落标记（见 sameCellBlocks）。
    if (!sameCellBlocks(head, head + 1)) {
        noteBlocked();
        return false;
    }

    // 合并 = 删掉前一段的结束标记。两段的内容合成一段，而留下来的标记是后一段的 ——
    //   于是"合并之后那一段保留前一段的格式"这条约定不能白拿：要显式搬到后一段的标记上。
    const NodeId mark_a = blockSrc()[static_cast<size_t>(head)].mark;
    const NodeId mark_b = blockSrc()[static_cast<size_t>(head + 1)].mark;
    const rev::Node* ma = rev::findNode(doc(), mark_a);
    FlowPos at_a = 0;
    if (ma == nullptr || !flowIndex().intervalOf(mark_a, &at_a, nullptr)) return false;

    // 权限按"这枚段落标记能不能被删"判：区间 [at_a, at_a + 1) 就是标记自己那一位。
    //   判的是标记那一位可不可编辑，不是光标落在哪 —— 光标在字段外、标记在字段内时能合。
    //   安全阀（表单退化成文档模式）先过一遍，退化的情形该放行。
    if (!formDegraded() && !rev::canEditRange(doc(), mode(), Range{at_a, at_a + 1}).allowed) {
        noteBlocked();
        return false;
    }

    std::vector<Step> steps;
    pending_before_ = Range{};
    pending_after_ = Range{};
    pending_before_ = Range{at_a, at_a};
    pending_after_ = Range{at_a, at_a};

    const ParaMarkBuild rm = removeMarkStep(doc(), flowIndex(), at_a);
    if (!rm.ok) return false;
    const auto* ma_pl = std::get_if<rev::MarkPayload>(&ma->payload);
    if (ma_pl == nullptr) return false;

    if (mark_b != kInvalidNodeId) {
        const rev::Node* mb = rev::findNode(doc(), mark_b);
        if (mb == nullptr) return false;
        const auto* mb_pl = std::get_if<rev::MarkPayload>(&mb->payload);
        if (mb_pl == nullptr) return false;
        SetParaMarkFormat keep;
        keep.id = mark_b;
        keep.para = ma_pl->para;
        keep.styleRef = ma_pl->style;
        keep.before = mb_pl->para;
        keep.beforeStyleRef = mb_pl->style;
        steps.push_back(Step{keep});
        steps.push_back(rm.step);
    } else {
        // 后一段是末尾那一段、没有结束标记：删掉前一段的标记之后，合并出来的那一段就没有
        //   标记了 —— 它会按默认段落格式编译，前一段的格式静默丢掉。所以补一枚标记在末尾，
        //   带上被删那枚的格式。
        //   位置要按"删完之后"的坐标系给：删除让标记之后的一切前移一位。
        const FlowPos end_before = blockFlow()[static_cast<size_t>(head + 1)] +
                                   blockFlowSpan()[static_cast<size_t>(head + 1)];
        InsertParaMark ins;
        ins.at = mapPos(rm.step, end_before, Assoc::After);
        ins.para = ma_pl->para;
        ins.styleRef = ma_pl->style;
        steps.push_back(rm.step);
        steps.push_back(Step{ins});
    }
    if (!commit(std::move(steps), head)) {
        noteBlocked();
        return false;
    }
    pos().setCaretAtFlow(at_a);  // 被删掉的那枚段落标记的位置 = 合并点
    return true;
}

bool EditCommand::mergeWithPreviousParagraph() {
    if (blocks().empty()) return false;
    // 选区非空时它退化成"删掉选区"：它与段首退格本来就是同一件事的两面，
    //   而退格在选区非空时只删选区（见 backspace），这条必须跟着一致。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (!plan.steps.empty()) return commitErase(plan, "合并");
    TextPos p = pos().caret();
    if (p.para <= 0 || p.para >= static_cast<int>(blocks().size())) return false;
    // 只有光标确实在段首才合并（否则退格该删的是前一个簇）。
    if (pos().snapToCluster(p.para, p.offset) != 0) return false;
    return mergeParagraphInto(p.para - 1);
}

bool EditCommand::mergeWithNextParagraph() {
    if (blocks().empty()) return false;
    // 同上：选区非空时只删选区。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (!plan.steps.empty()) return commitErase(plan, "合并");
    TextPos p = pos().caret();
    if (p.para < 0 || p.para + 1 >= static_cast<int>(blocks().size())) return false;
    const size_t len = blocks()[static_cast<size_t>(p.para)].cp_text.size();
    if (pos().snapToCluster(p.para, p.offset) != len) return false;
    return mergeParagraphInto(p.para);
}

bool EditCommand::eraseOnTree(TextPos a, TextPos b, const char* label, bool caret_at_end) {
    if (a.para != b.para) return false;
    // 删除区间取"这段文本码点自己占的那一段"（括号不在里面，见 textSpanAt）。
    int rel_from = 0;
    int rel_to = 0;
    if (!pos().textSpanAt(a.para, a.offset, b.offset, &rel_from, &rel_to)) return false;
    const FlowPos base = blockFlow()[static_cast<size_t>(a.para)];
    const FlowPos from = base + rel_from;
    const FlowPos to = base + rel_to;
    if (from < 0 || to < 0 || from >= to) return false;
    // 按文本节点切分着删（见 makeDeleteTextSteps）：区间级那套"端点向外扩到结构单元边界"
    //   的规则在这里正好帮倒忙 —— 删控件里的一个字会被理解成删掉整个控件的内容。
    std::vector<Step> steps = deleteWholeRange(doc(), flowIndex(), from, to);
    if (steps.empty()) steps = deleteTextSteps(doc(), flowIndex(), from, to);
    if (steps.empty()) return false;
    pending_before_ = Range{};
    pending_after_ = Range{};
    // 撤销恢复的是"删除之前光标在哪"：退格与反缩进的光标在右端（被删的字在它左边），
    //   前删在左端。两处都写成左端的话，退格一次再撤销，光标会落到被恢复那个字的前面。
    const FlowPos caret_before = caret_at_end ? to : from;
    pending_before_ = Range{caret_before, caret_before};
    pending_after_ = Range{from, from};
    if (!commit(std::move(steps), a.para)) return false;
    // 落点 = 删除区间的起点，也就是被删掉那段文本所在的位置。这个流位置本来就算出来了，
    //   直接摆过去即可 —— 只留一个码点偏移、再由"最内层含端点"猜回侧别的话，
    //   删完一个字光标会落到相邻括号的另一侧（"跳了一格"）。
    pos().setCaretAtFlow(from);
    return true;
}

std::vector<Step> EditCommand::eraseFlowSteps(FlowPos from, FlowPos to) const {
    std::vector<Step> steps;
    if (from < 0 || to <= from) return steps;
    // 优先"整段并成一步"：区间里全是文本与段落标记时成立，而整篇删除与跨段删除都是这种。
    //   按节点切分要发几千步，每一步在提交时都要重建一次流索引（O(节点数)）——
    //   2000 段整篇删除实测几秒，其中一多半在这一项上。
    if (hasFlow()) {
        steps = deleteWholeRange(doc(), flowIndex(), from, to);
        if (!steps.empty()) return steps;
    }
    // 文本那一条：按"所属文本节点"切分，落在段落标记、容器边界与原子上的位置不产生步骤。
    steps = deleteTextSteps(doc(), flowIndex(), from, to);
    // 段落标记那一条：标记各占一个流位置，要么整覆盖要么不覆盖。
    //   它不能用 makeDeleteRange 顶替：那条按结构取区间，会把端点向外扩到控件边界
    //   （见 makeDeleteTextSteps 的说明），而这里要的正是"只删掉这一位"。
    //
    //   位置由块表直接给出，不逐位问 posAt：标记夹在两段之间，位置就是后一段的流起点减一。
    //   逐位问是 O(位置数 x 扇出)（posAt 要从根下降到最内层，每层线性扫孩子），
    //   2000 段文档整篇删除是亿级 —— 实测那一笔占了删除总耗时的几秒。
    if (hasFlow()) {
        for (size_t k = 1; k < blockFlow().size(); ++k) {
            const FlowPos p = blockFlow()[k] - 1;
            if (p < from || p >= to) continue;
            const ParaMarkBuild mb = removeMarkStep(doc(), flowIndex(), p);
            if (mb.ok) steps.push_back(mb.step);
        }
    }
    // 原子那一条：整枚移除（它没有可寻址的内部，这是它唯一的删法）。
    //   位置同样由表直接给出，不逐位问 posAt：第 p 段第 i 枚对象的流位置 =
    //   段起点 + 锚点 + i —— 它前面有 i 枚对象，每枚各占一格，与 textSpanAt 同一把尺子。
    //   表的第 i 项要按全部对象数（含控件边界）数，因为流位置把它们也算进去了；
    //   只对原子发步骤，所以下面按对象表整体遍历、只在原子处发。
    if (hasFlow()) {
        for (size_t p = 0; p < blocks().size(); ++p) {
            const FlowPos base = blockFlow()[p];
            const FlowPos span = blockFlowSpan()[p];
            if (base + span <= from || base >= to) continue;  // 这一段与区间不相交
            const std::vector<InlineObject>& objs = blocks()[p].objects;
            for (size_t i = 0; i < objs.size(); ++i) {
                if (objs[i].kind != InlineObjectKind::Atom) continue;
                const FlowPos f = base + static_cast<FlowPos>(objs[i].cp) + static_cast<FlowPos>(i);
                if (f < from || f >= to) continue;
                const NodeBuild nb = removeAtomStep(doc(), flowIndex(), f);
                if (nb.ok) steps.push_back(nb.step);
            }
        }
    }
    // 从后往前：删右边的不会移动左边的坐标，于是每步的坐标都是它自己那一刻的坐标。
    std::sort(steps.begin(), steps.end(),
              [](const Step& a, const Step& b) { return stepStartFlow(a) > stepStartFlow(b); });
    // 安全阀：两批步骤加起来必须刚好覆盖整个区间。差一位都说明区间里有东西是这套可编辑
    //   集合处理不了的（节边界、控件边界），此时宁可整条编辑被拒，也不要删掉半截区间 ——
    //   半截区间的症状是"选中的字还剩一个没删掉"，很难归因。
    FlowPos covered = 0;
    for (const Step& s : steps) {
        if (const auto* d = std::get_if<DeleteRange>(&s)) {
            covered += d->end - d->begin;
        } else if (std::get_if<RemoveParaMark>(&s) != nullptr) {
            covered += 1;
        } else if (std::get_if<RemoveNode>(&s) != nullptr) {
            covered += 1;  // 原子整枚占一格
        }
    }
    if (covered != to - from) return {};
    return steps;
}

EditCommand::SelectionErase EditCommand::planEraseSelection() {
    SelectionErase plan;
    if (!pos().hasSelection() || !hasFlow() || blockFlow().empty()) return plan;
    plan.before = pos().selection();
    plan.at = pos().selection().begin();
    const FlowPos from = pos().selection().begin();
    const FlowPos to = pos().selection().end();
    if (from >= to) return plan;
    // 判定与实际删除同一把尺子：判的是整个区间能不能编辑，删的是同一个区间。
    //   跨段的区间在这里第一次被放行 —— 档 1 的 policyErase 只管同段。
    if (!formDegraded() && !rev::canEditRange(doc(), mode(), Range{from, to}).allowed) {
        noteBlocked();
        plan.blocked = true;
        return plan;
    }
    // 整格选区（吸附到格边界的那一种）：逐格清空内容，格与格之间的结构不动。
    //   为什么不能按整条区间删：区间里夹着单元格边界，安全阀（覆盖必须刚好等于区间长）
    //   必然过不去，于是整条编辑被拒 —— 表现就是"选中了好几格，按 Del 没反应"。
    //   逐格清空还把"格"本身保住了：删完每格仍是空段落，表结构不变。
    if (pos().hasCellRange() && view_.tables && !view_.tables->empty()) {
        plan.steps = eraseCellContents(from, to);
        if (plan.steps.empty()) {
            noteBlocked();
            plan.blocked = true;
        }
        return plan;
    }
    // 整张被覆盖的表：整张删（与上面同一个理由，只是粒度是"表"）。
    plan.steps = eraseWholeTables(from, to);
    if (!plan.steps.empty()) return plan;
    plan.steps = eraseFlowSteps(from, to);
    if (plan.steps.empty()) {
        noteBlocked();
        plan.blocked = true;
    }
    return plan;
}

/**
 * 两个相邻块在同一个单元格里（或都不在表格里，见头文件）。
 *
 * 口径与引擎的 layoutCellAt 一致：包含这一段的最短块区间 = 最内层那一格。
 */
bool EditCommand::sameCellBlocks(int a, int b) const {
    if (!view_.tables) return true;
    const auto innermost = [this](int para) -> const LayoutCell* {
        const LayoutCell* best = nullptr;
        for (const LayoutTable& t : *view_.tables) {
            for (const LayoutCell& c : t.cells) {
                if (c.block_count <= 0) continue;
                if (para < c.first_block || para >= c.first_block + c.block_count) continue;
                if (best == nullptr || c.block_count < best->block_count) best = &c;
            }
        }
        return best;
    };
    const LayoutCell* ca = innermost(a);
    const LayoutCell* cb = innermost(b);
    if (ca == nullptr && cb == nullptr) return true;
    if (ca == nullptr || cb == nullptr) return false;
    return ca == cb;
}

/**
 * 整格删除：把选区覆盖到的每一格的内容各删一遍，合并成一批步骤。
 *
 * 按块区间删，不按流区间：一格的"内容范围"就是它占的那几段 —— 空格一个块、内容长度
 *   为 0，按流区间算出来是一个空区间，eraseFlowSteps 会当场拒绝。空格本来就没有内容
 *   可删，跳过它即可（格与格之间的结构一个都不动）。
 *
 * 嵌套表：外层格的块区间会把内层格整个包住，两层都删就会重复（坐标还对不上）。
 *   被别的块区间严格包含的那一份丢掉，只删最内层 —— 与 layoutCellAt 的取值口径一致。
 */
std::vector<Step> EditCommand::eraseCellContents(FlowPos from, FlowPos to) const {
    std::vector<Step> all;
    if (!hasFlow() || !view_.tables) return all;
    // 矩形选区给的是"选中了哪几格"，直接用它 —— 按包络取交集会把矩形两端那两行里没选中的
    //   格一起删掉（跨行拖选就是整行）。只有没有逐格信息时（全选那一路）才按包络求交。
    struct CellSpan {
        int first;
        int count;
    };
    std::vector<CellSpan> spans;
    if (!pos().cellBlocks().empty()) {
        for (const CaretResolver::CellBlocks& c : pos().cellBlocks())
            spans.push_back({c.first, c.count});
    } else {
        const auto span_of = [this](int first_b, int count, FlowPos* lo, FlowPos* hi) -> bool {
            const int last_b = first_b + count - 1;
            if (first_b < 0 || count <= 0 || last_b >= static_cast<int>(blockFlow().size()) ||
                last_b >= static_cast<int>(blockFlowSpan().size()))
                return false;
            *lo = blockFlow()[static_cast<size_t>(first_b)];
            *hi = blockFlow()[static_cast<size_t>(last_b)] +
                  blockFlowSpan()[static_cast<size_t>(last_b)];
            return true;
        };
        for (const LayoutTable& t : *view_.tables) {
            for (const LayoutCell& c : t.cells) {
                FlowPos lo = -1;
                FlowPos hi = -1;
                if (!span_of(c.first_block, c.block_count, &lo, &hi)) continue;
                // 交集判据按块看：这一格有没有段落在区间里。空格的段是零长的，
                //   按流位置判"hi <= from"会把它判成不相交 —— 它本来也不该被删。
                const int last_b = c.first_block + c.block_count - 1;
                const FlowPos last_end = blockFlow()[static_cast<size_t>(last_b)] +
                                         blockFlowSpan()[static_cast<size_t>(last_b)];
                if (last_end <= from || lo >= to) continue;
                spans.push_back({c.first_block, c.block_count});
            }
        }
    }
    // 去掉被严格包含的那一层（嵌套表的外层）。
    std::vector<CellSpan> inner;
    for (size_t i = 0; i < spans.size(); ++i) {
        bool contained = false;
        for (size_t j = 0; j < spans.size() && !contained; ++j) {
            if (i == j) continue;
            const int i_last = spans[i].first + spans[i].count;
            const int j_last = spans[j].first + spans[j].count;
            const bool strict = spans[j].first <= spans[i].first && j_last >= i_last &&
                                (spans[j].first < spans[i].first || j_last > i_last);
            contained = strict;
        }
        if (!contained) inner.push_back(spans[i]);
    }
    const auto flow_of = [this](int block) { return blockFlow()[static_cast<size_t>(block)]; };
    for (const CellSpan& sp : inner) {
        const int last_b = sp.first + sp.count - 1;
        const FlowPos lo = flow_of(sp.first);
        const FlowPos hi = flow_of(last_b) + blockFlowSpan()[static_cast<size_t>(last_b)];
        // 空格：没有内容可删，跳过（格本来就是空的，删它不需要任何步骤）。
        if (hi <= lo) continue;
        const std::vector<Step> part = eraseFlowSteps(lo, hi);
        if (part.empty()) return {};  // 有一格删不动就整条拒绝，不留半截
        all.insert(all.end(), part.begin(), part.end());
    }
    if (all.empty()) return {};  // 全是空格：没有可删的内容，交给调用方按"拒绝"处理
    // 从后往前：删右边的不会移动左边的坐标。
    std::sort(all.begin(), all.end(),
              [](const Step& a, const Step& b) { return stepStartFlow(a) > stepStartFlow(b); });
    return all;
}

/**
 * 被选区整张覆盖的表整张删（见头文件），表外那些纯文本段照常按区间删。
 *
 * "覆盖"还要求选区把它整个跨过去（起点在表首之前或终点在表尾之后）：两头都落在表内的
 *   文字选区只是"选了这张表里的字"，那时删表比用户选的多 —— 那种选区没有局部删结构的
 *   语义，交给调用方按原路拒绝。全选是例外，它本来就该把表一起清掉。
 *
 * 只算"最外层那一张"：内层表整个落在它的区间里，删外层时一起走，再给内层发一步就会
 *   撞上"节点已经不在了"。表与表之间的空隙按流区间删，于是全选得到的是"全篇清空"。
 */
std::vector<Step> EditCommand::eraseWholeTables(FlowPos from, FlowPos to) const {
    std::vector<Step> steps;
    if (!hasFlow() || !view_.tables || view_.tables->empty() || from >= to) return steps;
    if (blockFlow().empty() || blockFlowSpan().empty()) return steps;
    const FlowPos doc_begin = blockFlow()[0];
    const size_t last_block = blockFlow().size() - 1;
    const FlowPos doc_end = blockFlow()[last_block] + blockFlowSpan()[last_block];
    const bool whole_doc = (from <= doc_begin && to >= doc_end);
    struct Whole {
        NodeId id = kInvalidNodeId;
        FlowPos lo = 0;
        FlowPos hi = 0;
        bool inner = false;  // 被别的整表覆盖：不用单独发步骤
    };
    std::vector<Whole> wholes;
    for (const LayoutTable& t : *view_.tables) {
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!flowIndex().intervalOf(static_cast<NodeId>(t.handle), &lo, &hi)) continue;
        // 覆盖判据按块算，不按表的节点区间：表的结构位夹在块之间，而选区两端永远落在
        //   块内容上（全选也一样，它到不了最后那个段落标记）。按节点区间判，开头就是表格的
        //   文档永远不会被"整张覆盖"，全选删除照样被拒。
        int first_b = -1;
        int last_b = -1;
        for (const LayoutCell& c : t.cells) {
            if (c.block_count <= 0) continue;
            if (first_b < 0 || c.first_block < first_b) first_b = c.first_block;
            const int b = c.first_block + c.block_count - 1;
            if (b > last_b) last_b = b;
        }
        bool covered = (lo >= from && hi <= to);
        bool spans_over = covered;
        if (first_b >= 0 && last_b >= static_cast<int>(blockFlow().size())) continue;
        if (first_b >= 0) {
            const FlowPos blo = blockFlow()[static_cast<size_t>(first_b)];
            const FlowPos bhi = blockFlow()[static_cast<size_t>(last_b)] +
                                blockFlowSpan()[static_cast<size_t>(last_b)];
            covered = (blo >= from && bhi <= to);
            spans_over = covered && (from < blo || to > bhi || whole_doc);
        }
        if (!spans_over) continue;
        wholes.push_back(Whole{static_cast<NodeId>(t.handle), lo, hi, false});
    }
    if (wholes.empty()) return steps;
    for (Whole& w : wholes) {
        for (const Whole& o : wholes) {
            if (&o == &w) continue;
            if (o.lo <= w.lo && o.hi >= w.hi) w.inner = true;
        }
    }
    // 按表在流里的先后排：下面"表外的空隙"是顺着这个次序算出来的。
    std::sort(wholes.begin(), wholes.end(),
              [](const Whole& a, const Whole& b) { return a.lo < b.lo; });
    // 表外的空隙：从选区起点走到第一张整表之前、两张整表之间、最后一张之后。
    FlowPos cursor = from;
    for (const Whole& w : wholes) {
        if (w.inner) continue;
        if (w.lo > cursor) {
            const std::vector<Step> part = eraseFlowSteps(cursor, w.lo);
            if (part.empty()) return {};  // 空隙里有删不动的东西：整条按原路退回
            steps.insert(steps.end(), part.begin(), part.end());
        }
        int lo = -1;
        if (!flowIndex().intervalOf(w.id, &lo, nullptr)) return {};
        // 整块包住它必然被拒（与删控件同一条口径）：表单结构锁与只读都落在这里。
        if (!formDegraded() && !rev::canEditRange(doc(), mode(), Range{w.lo, w.hi}).allowed)
            return {};
        RemoveNode r;
        r.at_flow = lo;
        steps.push_back(Step{r});
        cursor = std::max(cursor, w.hi);
    }
    if (to > cursor) {
        const std::vector<Step> part = eraseFlowSteps(cursor, to);
        if (part.empty()) return {};
        steps.insert(steps.end(), part.begin(), part.end());
    }
    // 从后往前：删右边的不会移动左边的坐标。
    std::sort(steps.begin(), steps.end(),
              [](const Step& a, const Step& b) { return stepStartFlow(a) > stepStartFlow(b); });
    return steps;
}

bool EditCommand::commitErase(const SelectionErase& plan, const char* label) {
    if (plan.steps.empty()) return false;
    std::vector<Step> steps;
    pending_before_ = Range{};
    pending_after_ = Range{};
    pending_before_ = plan.before;
    pending_after_ = Range{plan.at, plan.at};
    steps = plan.steps;
    if (!commit(std::move(steps), pos().paraForFlow(plan.at))) {
        noteBlocked();
        return false;
    }
    pos().setCaretAtFlow(plan.at);
    return true;
}

/**
 * 用一段文本替换当前选区 —— 打字与缩进的共同落点。
 *
 * 删除与插入合并成一次事务：用户按下的是一下，撤销也该是一下。分成两个事务的话，
 * 第一次撤销只退掉那个字、选区还没回来，而"撤销回到编辑前两端"要求一次就回去。
 *
 * 插入的坐标直接用删除区间的起点 —— 那正是选区被删掉之后光标所在的位置（那些删除
 * 都在它右边，删右边不会移动左边的坐标，所以它在整批步骤里始终有效）。沿用的字符
 * 格式得在删除之前取：要的是"落点左边那个字"的格式，删完就取不到了。
 */
bool EditCommand::replaceSelectionWithText(const std::vector<uint32_t>& cps) {
    if (cps.empty()) return false;
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked || plan.steps.empty()) return false;

    InsertText s;
    s.at = plan.at;
    s.cps = cps;
    s.marks = marksAtFlow(s.at, &s.styleRef);

    const FlowPos after = plan.at + static_cast<FlowPos>(cps.size());
    std::vector<Step> steps;
    pending_before_ = Range{};
    pending_after_ = Range{};
    pending_before_ = plan.before;
    pending_after_ = Range{after, after};
    steps = plan.steps;
    steps.push_back(Step{s});

    if (!commit(std::move(steps), pos().paraForFlow(plan.at))) {
        noteBlocked();
        return false;
    }
    pos().setCaretAtFlow(after);
    return true;
}

CharFormat EditCommand::marksAtFlow(FlowPos at, std::string* style_ref) const {
    if (style_ref != nullptr) style_ref->clear();
    if (!hasFlow()) return CharFormat{};
    const Pos p = flowIndex().posAt(at);
    const rev::Node* n = rev::findNode(doc(), p.container);
    if (n == nullptr) return CharFormat{};
    if (isText(n->kind)) {
        const auto* run = std::get_if<rev::RunPayload>(&n->payload);
        if (run == nullptr) return CharFormat{};
        if (style_ref != nullptr) *style_ref = run->style;
        return run->marks;
    }
    // 落在容器里（含"文本之前"的空位置）：看前一个孩子是不是文本 —— 打字接在已有文字
    //   后面是最常见的一种，沿用它的标记才不会在粗体字后面敲出一个不粗的字。
    if (isContainer(n->kind) && p.offset > 0) {
        const rev::Ref& prev = n->children[static_cast<size_t>(p.offset) - 1];
        if (prev && isText(prev->kind)) {
            const auto* run = std::get_if<rev::RunPayload>(&prev->payload);
            if (run != nullptr) {
                if (style_ref != nullptr) *style_ref = run->style;
                return run->marks;
            }
        }
    }
    return CharFormat{};
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
    const rev::Node* c = rev::findNode(doc(), id);
    if (c == nullptr || c->kind != NodeKind::ContentControl) return false;
    const rev::Node* parent = rev::parentOf(doc(), id);
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
    if (!formDegraded() && !rev::canDoStructureOp(doc(), mode(), id).allowed) {
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
    if (!readPropValue(rev::findNode(doc(), id), prop, &before)) return PropResult::Failed;
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

CharFormat EditCommand::charFormatAtCaret() const {
    CharFormat out;
    if (!hasFlow()) return out;
    const FlowPos at = pos().selection().begin();
    const rev::FlowIndex& idx = flowIndex();
    bool found = false;
    std::function<void(const rev::Ref&)> walk = [&](const rev::Ref& n) {
        if (!n || found) return;
        if (n->kind == NodeKind::TextRun) {
            int f = 0;
            int e = 0;
            if (idx.intervalOf(n->id, &f, &e) && f <= at && at <= e) {
                out = std::get<rev::RunPayload>(n->payload).marks;
                found = true;
                return;
            }
        }
        for (const rev::Ref& c : n->children) walk(c);
    };
    walk(doc().root);
    return out;
}

ParaFormatPatch EditCommand::paraFormatAtCaret() const {
    ParaFormatPatch out;
    if (!hasFlow()) return out;
    const FlowPos at = pos().selection().begin();
    const rev::FlowIndex& idx = flowIndex();
    // 段落 = [上一枚标记之后, 本枚标记]：按流位置顺序找第一个盖住光标的标记。
    FlowPos seg_lo = 0;
    std::function<void(const rev::Ref&)> walk = [&](const rev::Ref& n) {
        if (!n) return;
        if (isParagraphMark(n->kind)) {
            int f = 0;
            int e = 0;
            if (idx.intervalOf(n->id, &f, &e)) {
                if (seg_lo <= at && at <= f) out = std::get<rev::MarkPayload>(n->payload).para;
                seg_lo = f + 1;
            }
        }
        for (const rev::Ref& c : n->children) walk(c);
    };
    walk(doc().root);
    return out;
}

/**
 * 对选区应用字符格式补丁。
 *
 * 作用面取"与选区相交的文本节点"，不是"整段"：字符格式本来就按节点存，
 *   把整段都改掉会让"只选中两个字加粗"变成整段加粗。
 */
bool EditCommand::applyCharFormat(const CharFormat& patch) {
    if (patch.empty() || !hasFlow() || !pos().hasSelection()) return false;
    const Range sel = pos().selection();
    const FlowPos from = sel.begin();
    const FlowPos to = sel.end();
    if (from >= to) return false;

    const rev::FlowIndex& idx = flowIndex();
    std::vector<Step> steps;
    // 走修订树而不是流索引的下标接口：这里要的是"节点号 + 标记"，而流索引只对外
    //   给位置换算与区间，节点对象是它的内部表示。
    std::function<void(const rev::Ref&)> walk = [&](const rev::Ref& n) {
        if (!n) return;
        if (n->kind == NodeKind::TextRun) {
            int f = 0;
            int e = 0;
            if (idx.intervalOf(n->id, &f, &e) && e > from && f < to) {
                // 修订层的节点把字符标记装在载荷里（与模型层的字段布局不同）。
                const auto& rp = std::get<rev::RunPayload>(n->payload);
                const CharFormat next = mergeCharFormat(rp.marks, patch);
                // 幂等：格式本来就等于目标值就不提交，撤销栈里不该出现什么都没做的记录。
                if (!(next == rp.marks)) {
                    SetRunMarks s;
                    s.id = n->id;
                    s.marks = next;
                    s.styleRef = rp.style;
                    s.before = rp.marks;
                    s.beforeStyleRef = rp.style;
                    steps.push_back(Step{s});
                }
            }
        }
        for (const rev::Ref& c : n->children) walk(c);
    };
    walk(doc().root);
    if (steps.empty()) return false;
    // 撤销之后选区要回来：改的是格式，用户还会接着改别的格式。
    pending_before_ = sel;
    pending_after_ = sel;
    return commit(std::move(steps), pos().caret().para);
}

/**
 * 对选区覆盖的段落应用段落格式补丁。
 *
 * 段落的流区间是"上一枚标记之后到本枚标记（含）"，于是选区与本段相交的判据就是
 *   区间相交 —— 这比按段号算少一处特例：选区停在下一段段首时，那一段的区间
 *   起点等于选区终点，开区间判定自然把它排除在外。
 */
bool EditCommand::applyParaFormat(const ParaFormatPatch& patch) {
    if (!hasFlow()) return false;
    const Range sel = pos().selection();
    const FlowPos lo = sel.begin();
    const FlowPos hi = sel.empty() ? sel.begin() + 1 : sel.end();

    const rev::FlowIndex& idx = flowIndex();
    struct MarkHit {
        FlowPos at = 0;
        NodeId id = kInvalidNodeId;
        ParaFormatPatch para;
        std::string styleRef;
    };
    std::vector<MarkHit> marks;
    std::function<void(const rev::Ref&)> walk = [&](const rev::Ref& n) {
        if (!n) return;
        if (isParagraphMark(n->kind)) {
            int f = 0;
            int e = 0;
            if (idx.intervalOf(n->id, &f, &e)) {
                const auto& mp = std::get<rev::MarkPayload>(n->payload);
                marks.push_back(MarkHit{f, n->id, mp.para, mp.style});
            }
        }
        for (const rev::Ref& c : n->children) walk(c);
    };
    walk(doc().root);
    std::sort(marks.begin(), marks.end(),
              [](const MarkHit& a, const MarkHit& b) { return a.at < b.at; });

    std::vector<Step> steps;
    FlowPos seg_lo = 0;
    for (const MarkHit& m : marks) {
        const FlowPos seg_hi = m.at + 1;
        if (seg_lo < hi && lo < seg_hi) {
            const ParaFormatPatch next = mergeParaFormatPatch(m.para, patch);
            if (!(next == m.para)) {
                SetParaMarkFormat s;
                s.id = m.id;
                s.para = next;
                s.styleRef = m.styleRef;
                s.before = m.para;
                s.beforeStyleRef = m.styleRef;
                steps.push_back(Step{s});
            }
        }
        seg_lo = seg_hi;
    }
    if (steps.empty()) return false;
    pending_before_ = sel;
    pending_after_ = sel;
    return commit(std::move(steps), pos().caret().para);
}

/**
 * Tab / Shift+Tab：缩进与反缩进
 * 语义先说清楚：Tab 是缩进，不是"跳到下一个控件"。
 * 表单字段之间用鼠标点、或者方向键（光标本来就会跨字段）。理由写在
 * EngineConfig::tab_spaces 那里。
 * 实现上刻意复用"敲字 / 退格"那两条路径：这样策略检查、区间伸缩、
 * 重排、重绘、记账全部自动一致 —— 缩进不该是一条特权通道，
 * 它必须和手打 4 个空格得到的结果逐字节相同（探针里有这条断言）。
 */
bool EditCommand::indent() {
    const int n = std::max(0, tabSpaces());
    if (n == 0) return false;
    std::string spaces(static_cast<size_t>(n), ' ');
    return typeTextUtf8(spaces);
}

bool EditCommand::outdent() {
    if (blocks().empty()) return false;
    // 反缩进归"删类"：选区非空时它要做的事就是删掉选区，删完即止 —— 与退格同一口径。
    //   删完再去找前面的空格是错的：光标这时停在选区起点，前面那一段空格跟用户选中的
    //   东西无关，退掉它等于多删了用户没选的内容。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (!plan.steps.empty()) return commitErase(plan, "反缩进");
    TextPos p = pos().caret();
    if (p.para < 0 || p.para >= static_cast<int>(blocks().size())) p.para = 0;
    p.offset = pos().snapToCluster(p.para, p.offset);

    const std::vector<uint32_t>& cps = blocks()[static_cast<size_t>(p.para)].cp_text;
    // 光标前连续空格的起点（最多 tab_spaces 个；只在本段内找，
    // 不跨段回退 —— 跨段删空格会把上一段的段尾吃掉，那是不对的）。
    size_t at = p.offset;
    int n = 0;
    while (at > 0 && n < std::max(0, tabSpaces()) && cps[at - 1] == 0x20u) {
        --at;
        ++n;
    }
    if (n == 0) return false;

    // 与 backspace 同一条策略：删掉的区间必须落在同一个可编辑控件内。
    if (!policyErase(TextPos{p.para, at}, TextPos{p.para, p.offset}).allowed) {
        noteBlocked();
        return false;
    }
    // 与退格同一档：光标在右端，撤销要回到被删掉的空格后面。
    return eraseOnTree(TextPos{p.para, at}, TextPos{p.para, p.offset}, "反缩进",
                       /*caret_at_end=*/true);
}

bool EditCommand::hasBlockLevel(const std::vector<NodeSpec>& nodes) {
    for (const NodeSpec& n : nodes) {
        if (n.kind == NodeKind::Table) return true;
    }
    return false;
}

bool EditCommand::startsParagraph(FlowPos at) const {
    if (!hasFlow()) return true;
    const Pos p = flowIndex().posAt(at);
    const rev::Node* n = rev::findNode(doc(), p.container);
    if (n == nullptr) return true;
    // 落在一个文本节点内部：它前面还有字，当然不是段首。
    if (isText(n->kind)) return p.offset == 0;
    if (!isContainer(n->kind)) return true;
    if (p.offset <= 0) return true;
    if (p.offset > static_cast<int>(n->children.size())) return true;
    return isParagraphMark(n->children[static_cast<size_t>(p.offset - 1)]->kind);
}

bool EditCommand::endsParagraph(FlowPos at) const {
    if (!hasFlow()) return true;
    const Pos p = flowIndex().posAt(at);
    const rev::Node* n = rev::findNode(doc(), p.container);
    if (n == nullptr) return true;
    if (isText(n->kind)) {
        // 文本节点内部：右边还有字。落点正好在末尾时按"容器里的下一个兄弟"判，
        // 那正是下面那一步要做的事，所以这里只区分"中间"与"末尾"。
        const auto* run = std::get_if<rev::RunPayload>(&n->payload);
        return run != nullptr && p.offset >= static_cast<int>(run->text.size());
    }
    if (!isContainer(n->kind)) return true;
    if (p.offset >= static_cast<int>(n->children.size())) return true;
    return isParagraphMark(n->children[static_cast<size_t>(p.offset)]->kind);
}

bool EditCommand::deleteSelection() {
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (plan.steps.empty()) return false;
    return commitErase(plan, "删除选区");
}

bool EditCommand::pasteNodes(const std::vector<NodeSpec>& nodes) {
    if (nodes.empty() || !hasFlow()) return false;
    // 落点判定与打字同一条：表单模式下控件之外落不进去，被拒时与"按下的那一下没发生"同一口径。
    if (!policyAt(pos().caret()).allowed) {
        noteBlocked();
        return false;
    }
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;

    // 没有选区时落点就是光标；有选区时 plan.at 已经是区间起点（删除之后光标也落在那里）。
    const TextPos caret = pos().caret();
    const FlowPos at = plan.steps.empty()
                           ? pos().flowPosAtSide(caret.para, caret.offset, pos().sideForPos(caret))
                           : plan.at;
    if (at < 0) {
        noteBlocked();
        return false;
    }

    std::vector<Step> steps = plan.steps;
    FlowPos cur = at;
    // 块级元素（表格）不在段落内部：它前面后面要是没有段落标记，就各补一枚。
    //   不补的话表格会插在一段文字中间 —— 编译与流索引对"段"的假设当场不成立。
    const bool block = hasBlockLevel(nodes);
    if (block && !startsParagraph(cur)) {
        InsertParaMark pm;
        pm.at = cur;
        steps.push_back(Step{pm});
        ++cur;
    }
    for (const NodeSpec& spec : nodes) {
        InsertNode in;
        in.at_flow = cur;
        in.at = flowIndex().posAt(cur);
        in.subtree = spec;
        steps.push_back(Step{in});
        cur += static_cast<FlowPos>(spec.span());
    }
    if (block && !endsParagraph(cur)) {
        InsertParaMark pm;
        pm.at = cur;
        steps.push_back(Step{pm});
        ++cur;
    }

    pending_before_ = plan.steps.empty() ? Range{at, at} : plan.before;
    pending_after_ = Range{cur, cur};
    if (!commit(std::move(steps), pos().paraForFlow(at))) {
        noteBlocked();
        return false;
    }
    // 落点 = 片段的末尾（与打字把光标摆在插入内容之后同一口径）。
    pos().setCaretAtFlow(cur);
    return true;
}

}  // namespace heditor
