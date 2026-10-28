/**
 * 命令层的实现：一次编辑怎么变成一条事务（声明与边界见 edit_command.h）。
 *
 * 插入与删除先过落点判定（policyAt / policyErase），再按字素簇吸附；跨段编辑（分段、合并）
 * 与结构操作（删控件、改控件属性）走同一条"造事务 → 交出去"的路 —— 于是撤销、重排、重绘、
 * 记账自动一致，不必另开一条特权通道。
 */
#include "core/engine/edit_command.h"

#include "core/doc/subtree.h"
#include "core/doc/table_grid.h"
#include "core/engine/control_spans.h"
#include "core/engine/structure_index.h"
#include "core/utils/utf8.h"

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
 * 区间里整块被覆盖的控件：摘掉这棵子树就等于删掉它占的全部流位置（边界与内容）。
 *
 * 控件在流里占两位边界加内容，而文本删除步只能覆盖内容 —— 混着文字一起选时，
 * 边界那两位没有对应步骤，覆盖校验必然对不上，整条编辑会被拒。所以整块覆盖的控件
 * 单独走 RemoveNode，其余区间才按文本 / 段落标记 / 原子发步。
 */
struct WholeControl {
    /**
     * 控件节点身份。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 它在流里的区间（含两枚边界）。
     */
    FlowPos lo = 0;
    FlowPos hi = 0;
};

/**
 * 读控件的运行时属性。四个分居两处：hidden 在节点上（呈现态的流位置换算要看它），
 * 其余三个在控件数据里。
 */
bool readPropValue(const doc::Node* n, ControlProp p, bool* out) {
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
NodeBuild removeAtomStep(const doc::Doc& doc, const doc::FlowIndex& idx, FlowPos at) {
    NodeBuild out;
    if (at < 0 || at >= idx.size()) {
        out.error = "删除原子：流位置越界";
        return out;
    }
    const Pos p = idx.posAt(at);
    const doc::Node* n = doc::findNode(doc, p.container);
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
ParaMarkBuild removeMarkStep(const doc::Doc& doc, const doc::FlowIndex& idx, FlowPos at) {
    ParaMarkBuild out;
    if (at < 0 || at >= idx.size()) {
        out.error = "删除段落标记：流位置越界";
        return out;
    }
    const Pos p = idx.posAt(at);
    const doc::Node* n = doc::findNode(doc, p.container);
    if (n == nullptr || !isParagraphMark(n->kind) || p.offset != 0) {
        out.error = "删除段落标记：该位置上没有段落标记";
        return out;
    }
    const auto* mp = std::get_if<doc::MarkPayload>(&n->payload);
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
std::vector<Step> deleteWholeRange(const doc::Doc& doc, const doc::FlowIndex& idx, FlowPos from,
                                   FlowPos to) {
    if (from >= to) return {};
    if (from < 0 || to > idx.size()) return {};
    const Pos b = idx.posAt(from);
    const doc::Node* bn = doc::findNode(doc, b.container);
    if (bn == nullptr) return {};
    const doc::Node* parent = isContainer(bn->kind) ? bn : doc::parentOf(doc, bn->id);
    if (parent == nullptr) return {};

    // 覆盖总和必须恰好等于区间长：跨层（区间伸出了这一层的孩子表）或中间有洞时
    //   不满足，宁可返回空让调用方另走结构步骤，也不要发出半截区间 —— 那一类症状是
    //   "选中了整张表，只删掉第一格的字"。
    int covered = 0;
    for (const doc::Ref& c : parent->children) {
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
std::vector<Step> deleteTextSteps(const doc::Doc& doc, const doc::FlowIndex& idx, FlowPos from,
                                  FlowPos to) {
    std::vector<Step> out;
    if (from < 0 || to <= from || to > idx.size()) return out;
    std::vector<std::pair<FlowPos, FlowPos>> segs;
    FlowPos p = from;
    while (p < to) {
        const Pos np = idx.posAt(p);
        const doc::Node* n = doc::findNode(doc, np.container);
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
doc::EditCheck EditCommand::policyAt(TextPos p) const {
    // 侧别再往下传了：位置自己归一次（见 flowAt）。不从控件下标反推 ——
    //   同一个偏移上可能压着好几枚括号，反推会猜错。
    const FlowPos f = pos().flowAt(p);
    if (f < 0) return doc::EditCheck{false, "落点不在文档流里"};
    return doc::canEditAt(doc(), mode(), f);
}

doc::EditCheck EditCommand::policyErase(TextPos a, TextPos b) const {
    // 档 1 的删除不跨段；段落标记的删除走合并那条路（另有判定）。
    if (a.para != b.para) return doc::EditCheck{false, "删除区间跨段"};
    // 区间 = "这段文本码点自己占的那一段"，与实际删除（eraseOnTree）同一把尺子。
    //   端点取错侧就会被判成越出控件而拒掉，表象是"删到只剩一个字就删不动了"；
    //   取宽了又会把相邻的括号圈进来，表象是"删完光标跳一格"。
    int rel_from = 0;
    int rel_to = 0;
    if (!pos().textSpanAt(a.para, a.offset, b.offset, &rel_from, &rel_to))
        return doc::EditCheck{false, "删除区间不在文档流里"};
    const FlowPos base = blockFlow()[static_cast<size_t>(a.para)];
    return doc::canEditRange(doc(), mode(), Range{base + rel_from, base + rel_to});
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
    const FlowPos at_flow = pos().flowAt(ins);
    if (at_flow < 0) {
        noteBlocked();
        return false;
    }
    // 落点判定过了，再看这个控件收不收这些字：数值字段把非数字挡在入口，
    //   一个都没留下时按"这一下没成立"返回（不静默地吞掉整次输入）。
    std::vector<uint32_t> typed;
    if (!filterTypedText(at_flow, cps, &typed)) {
        noteBlocked();
        return false;
    }
    InsertText s;
    s.at = at_flow;
    s.cps = typed;
    // 沿用落点左侧的字符格式：否则在粗体字中间敲出来的字不是粗的。
    s.marks = marksAtFlow(at_flow, &s.styleRef);
    std::vector<Step> steps;
    pending_before_ = Range{};
    pending_after_ = Range{};
    pending_before_ = Range{at_flow, at_flow};
    const FlowPos after = at_flow + static_cast<FlowPos>(typed.size());
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
    if (const ControlIndex* idx = controlIndex(); idx != nullptr && !idx->empty()) {
        return idx->bracketSpan(index, lo, hi);
    }
    if (index < 0 || index >= static_cast<int>(controlNodeIds().size())) return false;
    return controlSpanOf(flowIndex(), controlNodeIds()[static_cast<size_t>(index)], lo, hi);
}

int EditCommand::innermostControl(FlowPos f, FlowPos* lo, FlowPos* hi) const {
    if (!hasFlow()) return -1;
    if (const ControlIndex* idx = controlIndex(); idx != nullptr && !idx->empty()) {
        return idx->innermost(f, lo, hi);
    }
    return innermostControlAtFlow(flowIndex(), controlNodeIds(), f, lo, hi);
}

bool EditCommand::controlIsEmpty(int index) const {
    FlowPos lo = 0;
    FlowPos hi = 0;
    if (!controlFlowSpan(index, &lo, &hi)) return false;
    // 容器的两枚边界各占一格，中间什么都没有时区间恰好两格宽。
    return hi - lo == 2;
}

int EditCommand::controlEndingAt(FlowPos p) const {
    if (const ControlIndex* idx = controlIndex(); idx != nullptr && !idx->empty()) {
        return idx->endingAt(p);
    }
    for (int i = 0; i < static_cast<int>(controlNodeIds().size()); ++i) {
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!controlFlowSpan(i, &lo, &hi)) continue;
        if (hi == p) return i;
    }
    return -1;
}

int EditCommand::controlStartingAt(FlowPos p) const {
    if (const ControlIndex* idx = controlIndex(); idx != nullptr && !idx->empty()) {
        return idx->startingAt(p);
    }
    for (int i = 0; i < static_cast<int>(controlNodeIds().size()); ++i) {
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!controlFlowSpan(i, &lo, &hi)) continue;
        if (lo == p) return i;
    }
    return -1;
}

/**
 * 退格贴着控件括号时的四种情形（从左到右的书写方向）。只用于文档模式 —— 表单模式走
 * eraseInControl，那里一条都不许越过控件边界。
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
    const doc::Node* n = doc::findNode(doc(), p.container);
    if (n == nullptr || !isAtom(n->kind) || p.offset != 0) return false;
    if (!doc::canEditRange(doc(), mode(), Range{f, f + 1}).allowed) {
        noteBlocked();
        return false;
    }
    // 表单模式再加一道：整枚原子也必须落在光标所在的那个控件里（相邻字段的情形同
    //   eraseInsideCaretControl 的理由）。
    if (mode() == EditMode::Form && !eraseInsideCaretControl(f, f + 1)) {
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

bool EditCommand::controlIsWholeUnit(int index) const {
    if (index < 0 || index >= static_cast<int>(controlNodeIds().size())) return false;
    const doc::Node* n = doc::findNode(doc(), controlNodeIds()[static_cast<size_t>(index)]);
    if (n == nullptr) return false;
    const auto* spec = std::get_if<ControlSpec>(&n->payload);
    if (spec == nullptr) return false;
    // 判据是"内容不是作者打的字"：值在记录上（选择类、下拉、日期），内容就是它的投影，
    //   删一半再也重建不回来。文本与数值控件的值就在内容里，逐字删是正常编辑。
    //   这条与光标屏障用的不是同一个开关：屏障只管"光标能不能停"，而删除要覆盖
    //   全部生成内容 —— 下拉与日期没有屏障，但同样不该被删成半截。
    return spec->valueShape != ValueShape::Text;
}

bool EditCommand::eraseAtom(NodeId id) {
    if (!hasFlow()) return false;
    FlowPos at = 0;
    FlowPos end = 0;
    if (!flowIndex().intervalOf(id, &at, &end) || end <= at) return false;
    const doc::Node* n = doc::findNode(doc(), id);
    if (n == nullptr || !isAtom(n->kind)) return false;
    if (!doc::canEditRange(doc(), mode(), Range{at, at + 1}).allowed) {
        noteBlocked();
        return false;
    }
    const NodeBuild nb = removeAtomStep(doc(), flowIndex(), at);
    if (!nb.ok) return false;
    std::vector<Step> steps;
    steps.push_back(nb.step);
    // 撤销回到按键之前那一拍的位置（与 eraseAtomBeside 同一条）：原子没了之后
    //   那一格指向的是它右边的内容，与按键前的光标不是一回事。
    pending_before_ = pos().selection();
    pending_after_ = Range{at, at};
    if (!commit(std::move(steps), pos().paraForFlow(at))) {
        noteBlocked();
        return false;
    }
    pos().setCaretAtFlow(at);
    return true;
}

bool EditCommand::eraseWholeUnitBeside(FlowPos cur, bool left) {
    if (!hasFlow() || cur < 0) return false;
    // 三种相邻：左边那一块的右端就是我、右边那一块的左端就是我、光标本来就在某一块里
    //   （只剩程序化摆位能造出第三种，照样按整块处理，不给"进去删字"留门）。
    int ci = left ? controlEndingAt(cur) : controlStartingAt(cur);
    if (ci < 0) {
        FlowPos lo = 0;
        FlowPos hi = 0;
        const int inner = innermostControl(cur, &lo, &hi);
        if (inner >= 0) ci = inner;
    }
    if (ci < 0 || !controlIsWholeUnit(ci)) return false;
    // 有值的下拉 / 日期：这一下先清值，再按一次才删控件。填表的人按退格想删的是
    //   "刚才选的那一项"，一下把整个字段删掉等于丢了一格表单结构。
    if (clearUnitValue(ci)) return true;
    // 删不掉（只读、或者被结构锁挡住）也算"这一下处理过了"：落回后面的逐字删除
    //   就等于把字从一块不可编辑的内容里抠出来。
    removeControl(ci, /*keep_content=*/false, cur);
    return true;
}

bool EditCommand::clearUnitValue(int index) {
    if (index < 0 || index >= static_cast<int>(controlNodeIds().size())) return false;
    const doc::Node* n = doc::findNode(doc(), controlNodeIds()[static_cast<size_t>(index)]);
    if (n == nullptr) return false;
    const auto* spec = std::get_if<ControlSpec>(&n->payload);
    if (spec == nullptr) return false;
    // 只对"值是投影"的两类生效：选择类的记号与标签是作者写的内容，退格该整块删。
    if (dropdownOf(*spec) == nullptr && dateOf(*spec) == nullptr) return false;
    if (spec->value.text.empty()) return false;
    return setControlValue(index, ControlValue{});
}

int EditCommand::soleWholeUnitIn(FlowPos from, FlowPos to) const {
    if (to <= from) return -1;
    for (int i = 0; i < static_cast<int>(controlNodeIds().size()); ++i) {
        if (!controlIsWholeUnit(i)) continue;
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!controlFlowSpan(i, &lo, &hi)) continue;
        if (lo == from && hi == to) return i;
    }
    return -1;
}

void EditCommand::expandWholeUnits(FlowPos* from, FlowPos* to) const {
    if (from == nullptr || to == nullptr || *to <= *from) return;
    // 反复扫到不再变化：扩出一块可能又与另一块相交（嵌套或紧邻）。
    for (int guard = 0; guard < 64; ++guard) {
        bool grew = false;
        for (int i = 0; i < static_cast<int>(controlNodeIds().size()); ++i) {
            if (!controlIsWholeUnit(i)) continue;
            FlowPos lo = 0;
            FlowPos hi = 0;
            if (!controlFlowSpan(i, &lo, &hi)) continue;
            if (*to <= lo || *from >= hi) continue;  // 不相交
            if (*from > lo) {
                *from = lo;
                grew = true;
            }
            if (*to < hi) {
                *to = hi;
                grew = true;
            }
        }
        if (!grew) return;
    }
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

/**
 * 表单模式的退格 / 前删：只在光标所在控件的内容里动，一格也不越过控件边界。
 *
 * 三条后果必须一起成立，缺一条就会在边界上露出另一种形状：不删控件本身（控件的括号不是
 *   内容）、不把光标带到括号外面（键盘删除不兼职移动）、删到内容尽头就到此为止（不再
 *   退化成合并那一段）。合并仍然放行 —— 控件跨段时那枚段落标记落在内容里，删它是内容
 *   删除；控件之外的那一枚自然被能力表挡掉。
 *
 * @param left true = 退格（删左边），false = 前删（删右边）
 * @return true = 删了
 */
bool EditCommand::eraseInControl(bool left) {
    const FlowPos cur = pos().selection().focus;
    if (!doc::canEditAt(doc(), mode(), cur).allowed) {
        noteBlocked();
        return false;
    }
    // 紧邻的是一枚原子就整枚删掉：原子是内容，与一个字符同类。
    if (eraseAtomBeside(cur, left)) return true;

    TextPos p = pos().caret();
    if (p.para < 0 || p.para >= static_cast<int>(blocks().size())) return false;
    p.offset = pos().snapToCluster(p.para, p.offset);
    const size_t len = blocks()[static_cast<size_t>(p.para)].cp_text.size();

    if (left && p.offset == 0) return mergeWithPreviousParagraph();
    if (!left && p.offset >= len) return mergeWithNextParagraph();

    const TextPos a = left ? TextPos{p.para, pos().snapToCluster(p.para, p.offset - 1)} : p;
    TextPos b = p;
    if (!left) {
        size_t next = p.offset + 1;
        const size_t ceil_cp = pos().clusterCeilAt(p.para, p.offset);
        if (ceil_cp > p.offset) next = ceil_cp;
        if (next > len) next = len;
        b.offset = next;
    }
    if (!policyErase(a, b).allowed) {
        noteBlocked();
        return false;
    }
    // 端点可编辑还不够：两个字段紧挨着时，"光标在 c 的内容起点"与"删的是 b 的最后一个字"
    //   是同一个偏移上的两件事 —— 判据必须落在光标所在的那个控件里，否则退格会从下一个
    //   字段里偷字。
    int rel_from = 0;
    int rel_to = 0;
    if (!pos().textSpanAt(a.para, a.offset, b.offset, &rel_from, &rel_to)) {
        noteBlocked();
        return false;
    }
    const FlowPos base = blockFlow()[static_cast<size_t>(a.para)];
    if (!eraseInsideCaretControl(base + rel_from, base + rel_to)) {
        noteBlocked();
        return false;
    }
    return eraseOnTree(a, b, left ? "退格" : "前删", /*caret_at_end=*/left);
}

/**
 * 表单模式：删除区间 [from, to) 必须落在光标所在的那个控件里。
 *
 * 只判"区间里没有括号"是不够的：两个字段紧挨着时，前一个字段的最后一个字与后一个字段的
 * 第一个字落在同一条流上，span 各归各的，但光标在前者的末尾时退格删的是前者（对），
 * 光标在后者的开头时退格删的仍是前者（错 —— 那是从别的字段里偷字）。
 *
 * @param from 区间起点（含）
 * @param to   区间终点（不含）
 * @return true = 区间在光标所在控件的内容里
 */
bool EditCommand::eraseInsideCaretControl(FlowPos from, FlowPos to) const {
    if (!hasFlow()) return false;
    // "光标在哪个控件里"与落点判据同一个查询面（ControlSpans）—— 两处各写一遍遍历的话，
    //   嵌套时"取最内层"的规则迟早会有一边写反。
    const int ci = innermostControl(pos().selection().focus);
    if (ci < 0) return false;
    FlowPos lo = 0;
    FlowPos hi = 0;
    if (!controlSpanOf(flowIndex(), controlNodeIds()[static_cast<size_t>(ci)], &lo, &hi))
        return false;
    // 内容槽是 (lo, hi)：两枚括号各占一格，删除区间只要碰到它们就已经越界。
    return from > lo && to < hi;
}

/**
 * 选区是不是整个落在同一个控件的内容里（表单模式的选区删除判据）。
 *
 * 判据不是"两端都可编辑"：两端在同一个控件里、中间跨过另一个控件的括号时，按流区间删会
 *   把那对括号一起带走 —— 那是删掉了控件本身，比越过边界更严重。所以逐个控件看"与区间
 *   相交就必须整个包住区间"，嵌套的那些自然也被这一条挡住。
 *
 * @param from 区间起点（含）
 * @param to   区间终点（不含）
 * @return true = 区间没有碰到任何一枚括号
 */
bool EditCommand::rangeWithinOneControl(FlowPos from, FlowPos to) const {
    for (int i = 0; i < static_cast<int>(controlNodeIds().size()); ++i) {
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!controlFlowSpan(i, &lo, &hi)) continue;
        if (hi <= from || lo >= to) continue;
        if (!(lo < from && to < hi)) return false;
    }
    return true;
}

bool EditCommand::backspace() {
    if (blocks().empty()) return false;
    // 选区非空时退格是"删掉选区"，不再往前多删一个字 —— 这是最容易抄错的一条：
    //   抄成"先折叠选区再退格"的症状是用户删掉三个字，却连带把选区前面那个字也吃了。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (plan.handled) return true;
    if (!plan.steps.empty()) return commitErase(plan, "退格");
    const FlowPos caret_before = pos().selection().focus;

    // 表单模式：一次删除只能发生在光标所在控件的内容里（见 eraseInControl）。
    //   它必须排在括号那四条规则之前 —— 那四条规则的"退到开括号左面""删掉空控件"在
    //   文档模式里成立，在填表时却是把用户带出字段。
    if (mode() == EditMode::Form) return eraseInControl(/*left=*/true);

    // 紧邻的是一枚原子就整枚删掉（见 eraseAtomBeside）。它排在括号规则之前：两者不会同时
    //   成立，而"左边那一格是原子"比"光标贴在一枚括号上"更贴近屏幕上看到的那个东西。
    if (eraseAtomBeside(caret_before, /*left=*/true)) return true;

    // 紧邻的是一整块屏障控件就整块删（见 eraseWholeUnitBeside）。它必须排在括号规则之前：
    //   那四条里有"光标进到内容末尾再删左边那一项"，对选择类就是"进去删字"。
    if (eraseWholeUnitBeside(caret_before, /*left=*/true)) return true;

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
    if (plan.handled) return true;
    if (!plan.steps.empty()) return commitErase(plan, "前删");
    const FlowPos caret_before = pos().selection().focus;

    // 与退格同一条：表单模式的删除不越过控件边界。
    if (mode() == EditMode::Form) return eraseInControl(/*left=*/false);

    // 与退格左右对称：右边那一格是原子就整枚删掉。
    if (eraseAtomBeside(caret_before, /*left=*/false)) return true;

    // 与退格左右对称：右边是一整块屏障控件就整块删。
    if (eraseWholeUnitBeside(caret_before, /*left=*/false)) return true;

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
    const doc::Node* mk = doc::findNode(doc(), mark_a);
    ParaFormatPatch patch;
    std::string style_ref;
    CharFormat mark_marks;
    if (mk != nullptr) {
        if (const auto* mp = std::get_if<doc::MarkPayload>(&mk->payload)) {
            patch = mp->para;
            style_ref = mp->style;
            mark_marks = mp->marks;
        }
    }
    // 位置由 (偏移, 侧别) 给出，不从控件下标反推。
    //   反推在空控件处会猜错侧别，于是标记插到了别的控件里（"回车之后光标跑进第三个控件"）。
    const FlowPos at_flow = pos().flowAt(p);
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
    s.marks = mark_marks;
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
        clear_pbb.marks = mark_marks;
        clear_pbb.beforeMarks = mark_marks;
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
    const doc::Node* ma = doc::findNode(doc(), mark_a);
    FlowPos at_a = 0;
    if (ma == nullptr || !flowIndex().intervalOf(mark_a, &at_a, nullptr)) return false;

    // 权限按"这枚段落标记能不能被删"判：区间 [at_a, at_a + 1) 就是标记自己那一位。
    //   判的是标记那一位可不可编辑，不是光标落在哪 —— 光标在字段外、标记在字段内时能合。
    if (!doc::canEditRange(doc(), mode(), Range{at_a, at_a + 1}).allowed) {
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
    const auto* ma_pl = std::get_if<doc::MarkPayload>(&ma->payload);
    if (ma_pl == nullptr) return false;

    if (mark_b != kInvalidNodeId) {
        const doc::Node* mb = doc::findNode(doc(), mark_b);
        if (mb == nullptr) return false;
        const auto* mb_pl = std::get_if<doc::MarkPayload>(&mb->payload);
        if (mb_pl == nullptr) return false;
        SetParaMarkFormat keep;
        keep.id = mark_b;
        keep.para = ma_pl->para;
        keep.styleRef = ma_pl->style;
        keep.before = mb_pl->para;
        keep.beforeStyleRef = mb_pl->style;
        keep.marks = ma_pl->marks;
        keep.beforeMarks = mb_pl->marks;
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
        ins.marks = ma_pl->marks;
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
    // 整块覆盖的控件先挑出来：它们摘掉整个节点，剩下的区间才按文本 / 标记 / 原子发步。
    //   不分开就会两边都发（文本步与整块摘除步各算一份长度），覆盖校验当场对不上。
    std::vector<WholeControl> units;
    if (hasFlow()) {
        for (int i = 0; i < static_cast<int>(controlNodeIds().size()); ++i) {
            FlowPos lo = 0;
            FlowPos hi = 0;
            if (!controlFlowSpan(i, &lo, &hi)) continue;
            // 只认整块落在区间里的：端点压在选区边界上时那一块是"只选中一部分"，删一半
            //   的控件再也重建不回来 —— 那种情形由 expandWholeUnits 事先把区间扩到整块。
            if (lo < from || hi > to) continue;
            units.push_back(WholeControl{controlNodeIds()[static_cast<size_t>(i)], lo, hi});
        }
        // 嵌套：外层整块被覆盖时内层不必再发一步（摘掉外层，内层的节点就不在了）。
        //   按 lo 升序之后，落在前一块区间里的那几项丢掉即可。
        std::sort(units.begin(), units.end(),
                  [](const WholeControl& a, const WholeControl& b) { return a.lo < b.lo; });
        std::vector<WholeControl> outer;
        for (const WholeControl& u : units) {
            if (!outer.empty() && u.hi <= outer.back().hi) continue;
            outer.push_back(u);
        }
        units.swap(outer);
    }
    // 结构操作的门与删单个控件同一把尺子：表单模式默认锁住，文档模式放行。
    for (const WholeControl& u : units) {
        if (!doc::canDoStructureOp(doc(), mode(), u.id).allowed) {
            noteBlocked();
            return {};
        }
    }
    const auto insideUnit = [&units](FlowPos f) {
        for (const WholeControl& u : units) {
            if (f >= u.lo && f < u.hi) return true;
        }
        return false;
    };
    // 优先"整段并成一步"：区间里全是文本与段落标记时成立，而整篇删除与跨段删除都是这种。
    //   按节点切分要发几千步，每一步在提交时都要重建一次流索引（O(节点数)）——
    //   2000 段整篇删除实测几秒，其中一多半在这一项上。
    if (units.empty() && hasFlow()) {
        steps = deleteWholeRange(doc(), flowIndex(), from, to);
        if (!steps.empty()) return steps;
    }
    // 文本那一条：按"所属文本节点"切分，落在段落标记、容器边界、原子上的位置不产生步骤。
    //   整块控件占的那几段先抠掉 —— 它们的内容随节点一起走。
    if (hasFlow()) {
        FlowPos cursor = from;
        for (const WholeControl& u : units) {
            if (u.lo > cursor) {
                const std::vector<Step> part = deleteTextSteps(doc(), flowIndex(), cursor, u.lo);
                steps.insert(steps.end(), part.begin(), part.end());
            }
            if (u.hi > cursor) cursor = u.hi;
        }
        if (cursor < to) {
            const std::vector<Step> part = deleteTextSteps(doc(), flowIndex(), cursor, to);
            steps.insert(steps.end(), part.begin(), part.end());
        }
    }
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
            if (insideUnit(p)) continue;
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
                if (insideUnit(f)) continue;  // 控件里的原子随子树一起走
                const NodeBuild nb = removeAtomStep(doc(), flowIndex(), f);
                if (nb.ok) steps.push_back(nb.step);
            }
        }
    }
    // 控件那一条：整块摘除。位置用流坐标，应用时由它当场重算槽位。
    for (const WholeControl& u : units) {
        RemoveNode r;
        r.at_flow = u.lo;
        steps.push_back(Step{r});
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
        } else if (const auto* n = std::get_if<RemoveNode>(&s)) {
            // 控件整块占它自己的区间；原子整枚占一格。
            FlowPos unit_span = 0;
            for (const WholeControl& u : units) {
                if (u.lo == n->at_flow) {
                    unit_span = u.hi - u.lo;
                    break;
                }
            }
            covered += (unit_span > 0) ? unit_span : 1;
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
    FlowPos from = pos().selection().begin();
    FlowPos to = pos().selection().end();
    if (from >= to) return plan;
    // 只盖住屏障控件一部分时向外扩成整块：删一半的选项表再也重建不回来。
    expandWholeUnits(&from, &to);
    plan.at = from;
    // 扩完正好等于单独一块屏障控件：整块删掉那个节点，而不是把它删成空壳。
    if (const int only = soleWholeUnitIn(from, to); only >= 0) {
        plan.handled = true;
        removeControl(only, /*keep_content=*/false, plan.before.focus);
        return plan;
    }
    // 判定与实际删除同一把尺子：判的是整个区间能不能编辑，删的是同一个区间。
    //   跨段的区间在这里第一次被放行 —— 档 1 的 policyErase 只管同段。
    if (!doc::canEditRange(doc(), mode(), Range{from, to}).allowed) {
        noteBlocked();
        plan.blocked = true;
        return plan;
    }
    // 表单模式还有一道更严的门：选区删除不许碰到任何一枚控件括号（见 rangeWithinOneControl）。
    //   能力表只管"两端能不能编辑"，管不到"中间包住了谁的边界"。
    if (mode() == EditMode::Form && !rangeWithinOneControl(from, to)) {
        noteBlocked();
        plan.blocked = true;
        return plan;
    }
    // 整格选区（吸附到格边界的那一种）：盖满整张表就整张删，否则逐格清空内容，
    //   格与格之间的结构不动。
    //
    //   为什么不能按整条区间删：区间里夹着单元格边界，安全阀（覆盖必须刚好等于区间长）
    //   必然过不去，于是整条编辑被拒 —— 表现就是"选中了好几格，按 Del 没反应"。
    //   逐格清空还把"格"本身保住了：删完每格仍是空段落，表结构不变。
    //
    //   盖满整张表是另一件事：用户选的就是这张表（拖选整张表 = 全选它的格），删该连
    //   表框一起走 —— 只清内容会留下一张空表，看着像"没删掉"。
    if (pos().hasCellRange() && view_.tables && !view_.tables->empty()) {
        // 整表删：光标落回被删表的起点，不取选区起点 —— 整格选区的起点在表格内部
        //   （第一个被选中的格），那个流位置随表一起消失，后面一整段内容会顶上来，
        //   落点因此不在表格所在的那一处。
        FlowPos table_at = -1;
        plan.steps = eraseTablesCoveredByCells(&table_at);
        if (!plan.steps.empty()) {
            if (table_at >= 0) plan.at = table_at;
            return plan;
        }
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
    // 跨过控件括号：按控件边界切开（完整包住的整块删，只包住一半的只删内容）。
    //   写在按流区间删之前 —— 那一条碰到括号必然被安全阀拒掉。
    plan.steps = eraseAcrossControls(from, to);
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
    if (view_.structure == nullptr) return true;
    const CellRef ca = view_.structure->cellAt(a);
    const CellRef cb = view_.structure->cellAt(b);
    if (ca.table < 0 && cb.table < 0) return true;
    if (ca.table < 0 || cb.table < 0) return false;
    return ca.table == cb.table && ca.cell == cb.cell;
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
    if (!hasFlow() || !view_.tables || view_.structure == nullptr) return all;
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
        const auto add_cell = [&](int first_b, int count) {
            FlowPos lo = -1;
            FlowPos hi = -1;
            if (!span_of(first_b, count, &lo, &hi)) return;
            // 交集判据按块看：这一格有没有段落在区间里。空格的段是零长的，
            //   按流位置判"hi <= from"会把它判成不相交 —— 它本来也不该被删。
            const int last_b = first_b + count - 1;
            const FlowPos last_end = blockFlow()[static_cast<size_t>(last_b)] +
                                     blockFlowSpan()[static_cast<size_t>(last_b)];
            if (last_end <= from || lo >= to) return;
            spans.push_back({first_b, count});
        };
        for (const CellNode& c : view_.structure->cells_index.nodes) {
            add_cell(c.first_block, c.block_count);
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
    if (!hasFlow() || !view_.tables || view_.tables->empty() || view_.structure == nullptr ||
        from >= to)
        return steps;
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
    for (int ti = 0; ti < static_cast<int>(view_.tables->size()); ++ti) {
        const LayoutTable& t = (*view_.tables)[static_cast<size_t>(ti)];
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!flowIndex().intervalOf(static_cast<NodeId>(t.handle), &lo, &hi)) continue;
        // 覆盖判据按块算，不按表的节点区间：表的结构位夹在块之间，而选区两端永远落在
        //   块内容上（全选也一样，它到不了最后那个段落标记）。按节点区间判，开头就是表格的
        //   文档永远不会被"整张覆盖"，全选删除照样被拒。
        const TableSpan& span = view_.structure->tableSpan(ti);
        const int first_b = span.first_block;
        const int last_b = span.last_block;
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
        if (!doc::canEditRange(doc(), mode(), Range{w.lo, w.hi}).allowed) return {};
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

/**
 * 选区跨过控件括号时的删除步骤（见头文件）。
 *
 * 三件事按次序做：先把"完整包住"的控件整块摘掉（内层随外层一起走），再把剩下的区间
 *   按括号位置剪开（括号那一位不许删），最后逐段走 eraseFlowSteps。任何一段删不动就
 *   整条拒绝 —— 与其它删除路径同一条"要么整段，要么不动"。
 */
std::vector<Step> EditCommand::eraseAcrossControls(FlowPos from, FlowPos to) const {
    std::vector<Step> steps;
    if (!hasFlow() || from >= to || controlNodeIds().empty()) return steps;

    struct Unit {
        int index = -1;
        FlowPos lo = 0;
        FlowPos hi = 0;
        bool inner = false;  // 落在另一个整块控件里：删外层时一起走
    };
    std::vector<Unit> wholes;
    std::vector<FlowPos> brackets;
    bool crossed = false;
    for (int i = 0; i < static_cast<int>(controlNodeIds().size()); ++i) {
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!controlFlowSpan(i, &lo, &hi) || hi <= lo) continue;
        if (hi <= from || lo >= to) continue;
        crossed = true;
        if (lo >= from && hi <= to) {
            wholes.push_back(Unit{i, lo, hi, false});
            continue;
        }
        // 只盖住一部分的两个边：这两枚括号不能跟着区间走。
        if (lo >= from) brackets.push_back(lo);
        if (hi - 1 < to) brackets.push_back(hi - 1);
    }
    if (!crossed) return steps;
    for (Unit& w : wholes) {
        for (const Unit& o : wholes) {
            if (&o == &w) continue;
            if (o.lo <= w.lo && o.hi >= w.hi) w.inner = true;
        }
    }
    std::sort(wholes.begin(), wholes.end(),
              [](const Unit& a, const Unit& b) { return a.lo < b.lo; });
    std::sort(brackets.begin(), brackets.end());
    brackets.erase(std::unique(brackets.begin(), brackets.end()), brackets.end());

    // 一段可删区间：先把落在它里面的括号位置挖掉，再逐段删。
    auto emit = [&](FlowPos a, FlowPos b) -> bool {
        FlowPos p = a;
        for (FlowPos c : brackets) {
            if (c < p || c >= b) continue;
            if (c > p) {
                const std::vector<Step> part = eraseFlowSteps(p, c);
                if (part.empty()) return false;
                steps.insert(steps.end(), part.begin(), part.end());
            }
            p = c + 1;  // 括号自己留着
        }
        if (b > p) {
            const std::vector<Step> part = eraseFlowSteps(p, b);
            if (part.empty()) return false;
            steps.insert(steps.end(), part.begin(), part.end());
        }
        return true;
    };

    FlowPos cursor = from;
    for (const Unit& w : wholes) {
        if (w.inner) continue;
        if (w.lo > cursor && !emit(cursor, w.lo)) return {};
        FlowPos at = 0;
        if (!flowIndex().intervalOf(controlNodeIds()[static_cast<size_t>(w.index)], &at, nullptr)) {
            return {};
        }
        // 整块包住它必然被拒（只读、表单结构锁）：与删控件同一条口径。
        if (!doc::canEditRange(doc(), mode(), Range{w.lo, w.hi}).allowed) return {};
        RemoveNode r;
        r.at_flow = at;
        steps.push_back(Step{r});
        cursor = std::max(cursor, w.hi);
    }
    if (to > cursor && !emit(cursor, to)) return {};
    std::sort(steps.begin(), steps.end(),
              [](const Step& a, const Step& b) { return stepStartFlow(a) > stepStartFlow(b); });
    return steps;
}

/**
 * 整格选区盖满了某张表的全部格位 ⇒ 整张删（见头文件）。
 *
 * 逐格比块区间，不比流区间：一格的身份是它占的那几段（空格也行），而流区间在空格上
 *   退化成点、与下一格的第一段共用一个位置。拖选整张表拿到的就是一串格区间，按它判
 *   "这张表的每一格都在选区里"才是用户那句话的意思。
 */
std::vector<Step> EditCommand::eraseTablesCoveredByCells(FlowPos* anchor) const {
    std::vector<Step> steps;
    if (!hasFlow() || !view_.tables || view_.tables->empty() || view_.structure == nullptr)
        return steps;
    if (!pos().hasCellRange()) return steps;
    const std::vector<CaretResolver::CellBlocks>& picked = pos().cellBlocks();
    const auto picked_has = [&picked](int first, int count) {
        for (const CaretResolver::CellBlocks& c : picked) {
            if (c.first == first && c.count == count) return true;
        }
        return false;
    };
    struct Whole {
        NodeId id = kInvalidNodeId;
        FlowPos lo = 0;
        FlowPos hi = 0;
        bool inner = false;  // 落在另一张整表里：删外层时一起走
    };
    std::vector<Whole> wholes;
    for (int ti = 0; ti < static_cast<int>(view_.tables->size()); ++ti) {
        const LayoutTable& t = (*view_.tables)[static_cast<size_t>(ti)];
        if (t.cells.empty()) continue;
        const TableSpan& span = view_.structure->tableSpan(ti);
        bool all = true;
        for (int k = 0; k < span.cell_count; ++k) {
            const CellNode& c =
                view_.structure->cells_index.nodes[static_cast<size_t>(span.first_cell + k)];
            if (!picked_has(c.first_block, c.block_count)) {
                all = false;
                break;
            }
        }
        if (!all || span.first_block < 0) continue;
        const int first_b = span.first_block;
        const int last_b = span.last_block;
        // 表是文档里唯一的内容时不动结构：删完一个块都不剩，光标没有能停的地方。
        //   这一种退回逐格清空 —— 内容照样没了，只是表框还留着。
        if (first_b <= 0 && last_b >= static_cast<int>(blocks().size()) - 1) continue;
        FlowPos lo = 0;
        FlowPos hi = 0;
        const NodeId id = static_cast<NodeId>(t.handle);
        if (!flowIndex().intervalOf(id, &lo, &hi) || hi <= lo) continue;
        // 整块包住它必然被拒（只读、表单结构锁），与 removeTableAtCaret 同一条口径。
        if (!doc::canEditRange(doc(), mode(), Range{lo, hi}).allowed) continue;
        wholes.push_back(Whole{id, lo, hi, false});
    }
    if (wholes.empty()) return steps;
    // 嵌套表：外层整张被删，内层随之消失 —— 再给内层发一步会撞上"节点已经不在了"。
    for (Whole& w : wholes) {
        for (const Whole& o : wholes) {
            if (&o == &w) continue;
            if (o.lo <= w.lo && o.hi >= w.hi) w.inner = true;
        }
    }
    for (const Whole& w : wholes) {
        if (w.inner) continue;
        RemoveNode r;
        r.at_flow = w.lo;
        steps.push_back(Step{r});
        // 光标落点取最靠前的那张表的起点：它在所有步骤的左边，删完仍然有效，
        //   而且正是表格占住的那一处。
        if (anchor != nullptr && (*anchor < 0 || w.lo < *anchor)) *anchor = w.lo;
    }
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
 * 格式得在删除之前取：删完就取不到了。
 */
bool EditCommand::replaceSelectionWithText(const std::vector<uint32_t>& cps) {
    if (cps.empty()) return false;
    // 用"选区第一个字"的格式，不是左边的邻居：选中加粗的一段再敲，敲出来的仍是加粗的
    //   （按左邻居取的话，紧接在普通文字后面的加粗段一改就变普通了）。
    const Range sel = pos().selection();
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked || plan.steps.empty()) return false;

    InsertText s;
    s.at = plan.at;
    s.cps = cps;
    s.marks = marksAtSelectionStart(sel.begin(), sel.end(), &s.styleRef);

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

CharFormat EditCommand::charFormatAtCaret() const {
    if (!hasFlow()) return CharFormat{};
    const Range sel = pos().selection();
    const FlowPos from = sel.begin();
    const FlowPos to = sel.end();
    // 空选区问"落点处接着写会用什么格式" —— 与打字同一个函数，回显与行为才不可能分叉
    //   （逐个节点取"盖住光标的那些"再求交集的话，压在两段边界上会得到两段的交集，
    //   而敲字用的是左边那一段）。
    if (from >= to) return marksAtFlow(from);
    const doc::FlowIndex& idx = flowIndex();
    bool found = false;
    CharFormat out;
    bool mark_only = false;
    std::function<void(const doc::Ref&)> walk = [&](const doc::Ref& n) {
        if (!n) return;
        if (n->kind == NodeKind::TextRun) {
            int f = 0;
            int e = 0;
            if (idx.intervalOf(n->id, &f, &e)) {
                const bool hit = (e > from && f < to);
                if (hit) {
                    const CharFormat& m = std::get<doc::RunPayload>(n->payload).marks;
                    if (found)
                        out = intersectCharFormat(out, m);
                    else
                        out = m;
                    found = true;
                }
            }
        }
        // 选区里一个文本节点都没碰到（整段是空单元格）：报段落标记上那一份，
        //   它就是"接着敲字会用到的格式"。
        if (!found && !mark_only && isParagraphMark(n->kind)) {
            int f = 0;
            int e = 0;
            if (idx.intervalOf(n->id, &f, &e) && e > from && f < to) {
                out = std::get<doc::MarkPayload>(n->payload).marks;
                mark_only = true;
            }
        }
        for (const doc::Ref& c : n->children) walk(c);
    };
    walk(doc().root);
    return out;
}

ParaFormatPatch EditCommand::paraFormatAtCaret() const {
    ParaFormatPatch out;
    if (!hasFlow()) return out;
    const FlowPos at = pos().selection().begin();
    const doc::FlowIndex& idx = flowIndex();
    // 段落 = [上一枚标记之后, 本枚标记]：按流位置顺序找第一个盖住光标的标记。
    FlowPos seg_lo = 0;
    std::function<void(const doc::Ref&)> walk = [&](const doc::Ref& n) {
        if (!n) return;
        if (isParagraphMark(n->kind)) {
            int f = 0;
            int e = 0;
            if (idx.intervalOf(n->id, &f, &e)) {
                if (seg_lo <= at && at <= f) out = std::get<doc::MarkPayload>(n->payload).para;
                seg_lo = f + 1;
            }
        }
        for (const doc::Ref& c : n->children) walk(c);
    };
    walk(doc().root);
    return out;
}

/**
 * 造一段"待插入的文本节点"的描述。
 * @param text 正文
 * @param marks 字符标记
 * @param style 字符样式引用
 * @param hidden 可见性（切分时要跟着原节点走，否则隐藏控件里的文字会露出来）
 * @return 子树描述（种类是文本节点）
 */
static NodeSpec runSpecOf(std::vector<uint32_t> text, const CharFormat& marks,
                          const std::string& style, bool hidden) {
    NodeSpec spec;
    spec.kind = NodeKind::TextRun;
    spec.hidden = hidden;
    spec.text = std::move(text);
    spec.marks = marks;
    spec.styleRef = style;
    return spec;
}

/**
 * 对选区应用字符格式补丁。
 *
 * 字符标记挂在文本节点上，一个节点只有一套标记。所以"只把选中的两个字加粗"落不到
 *   一个节点上 —— 得先把节点按选区的两端切成一串：留下的那一段、要改的那一段、
 *   被切出去的那一段。切分不改总长度，于是同一批里其它节点的流位置仍然有效。
 *
 * 切出来的是新节点，身份不保留（doc 层按描述物化）；撤销拿回的是整份旧修订，
 *   所以"撤销之后那三个节点合成回一个"这件事由快照负责，不需要逆操作去拼。
 */
bool EditCommand::applyCharFormat(const CharFormat& patch) {
    if (patch.empty() || !hasFlow() || !pos().hasSelection()) return false;
    const Range sel = pos().selection();
    const FlowPos from = sel.begin();
    const FlowPos to = sel.end();
    if (from >= to) return false;

    const doc::FlowIndex& idx = flowIndex();
    std::vector<Step> steps;
    // 走修订树而不是流索引的下标接口：这里要的是"节点号 + 正文 + 标记"，而流索引只对外
    //   给位置换算与区间，节点对象是它的内部表示。
    std::function<void(const doc::Ref&)> walk = [&](const doc::Ref& n) {
        if (!n) return;
        // 段落标记：空段落里没有文本节点，字符格式只能落在标记上 —— 否则选中一个空
        //   单元格改字重时一个节点都碰不到，命令返回"没变"，接着敲的字还是默认样式。
        if (isParagraphMark(n->kind)) {
            int f = 0;
            int e = 0;
            if (idx.intervalOf(n->id, &f, &e) && e > from && f < to) {
                const auto& mp = std::get<doc::MarkPayload>(n->payload);
                const CharFormat next = mergeCharFormat(mp.marks, patch);
                if (!(next == mp.marks)) {
                    SetParaMarkFormat s;
                    s.id = n->id;
                    s.para = mp.para;
                    s.styleRef = mp.style;
                    s.before = mp.para;
                    s.beforeStyleRef = mp.style;
                    s.marks = next;
                    s.beforeMarks = mp.marks;
                    steps.push_back(Step{s});
                }
            }
        }
        if (n->kind == NodeKind::TextRun) {
            int f = 0;
            int e = 0;
            if (idx.intervalOf(n->id, &f, &e) && e > from && f < to) {
                // 修订层的节点把正文与标记装在载荷里（与模型层的字段布局不同）。
                const auto& rp = std::get<doc::RunPayload>(n->payload);
                const CharFormat next = mergeCharFormat(rp.marks, patch);
                // 幂等：这段正文本来就等于目标值就不动它，撤销栈里不该出现空记录。
                if (!(next == rp.marks)) {
                    const int len = static_cast<int>(rp.text.size());
                    const int lo = std::min(std::max(0, static_cast<int>(from) - f), len);
                    const int hi = std::min(std::max(0, static_cast<int>(to) - f), len);
                    if (hi > lo) {
                        const std::vector<uint32_t>& t = rp.text;
                        // 原节点只留下"选区之后"的那一段；选区盖到底（hi == len）时它
                        //   直接持改好的那一段，省掉一次插入。
                        const int keep = (hi < len) ? hi : lo;
                        SetRun s;
                        s.id = n->id;
                        s.text.assign(t.begin() + keep, t.end());
                        s.marks = (hi < len) ? rp.marks : next;
                        s.styleRef = rp.style;
                        s.beforeText = rp.text;
                        s.before = rp.marks;
                        s.beforeStyleRef = rp.style;
                        steps.push_back(Step{s});
                        // 选区之前的那一段：原正文、原标记，插在原节点前面。
                        if (lo > 0) {
                            InsertNode head;
                            head.at_flow = f;
                            head.subtree =
                                runSpecOf(std::vector<uint32_t>(t.begin(), t.begin() + lo),
                                          rp.marks, rp.style, n->hidden);
                            steps.push_back(Step{head});
                        }
                        // 选区那一段：新标记。hi == len 时它已经由原节点自己担着。
                        if (hi < len) {
                            InsertNode mid;
                            mid.at_flow = f + lo;
                            mid.subtree =
                                runSpecOf(std::vector<uint32_t>(t.begin() + lo, t.begin() + hi),
                                          next, rp.style, n->hidden);
                            steps.push_back(Step{mid});
                        }
                    }
                }
            }
        }
        for (const doc::Ref& c : n->children) walk(c);
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

    const doc::FlowIndex& idx = flowIndex();
    struct MarkHit {
        FlowPos at = 0;
        NodeId id = kInvalidNodeId;
        ParaFormatPatch para;
        std::string styleRef;
        CharFormat marks;
    };
    std::vector<MarkHit> marks;
    std::function<void(const doc::Ref&)> walk = [&](const doc::Ref& n) {
        if (!n) return;
        if (isParagraphMark(n->kind)) {
            int f = 0;
            int e = 0;
            if (idx.intervalOf(n->id, &f, &e)) {
                const auto& mp = std::get<doc::MarkPayload>(n->payload);
                marks.push_back(MarkHit{f, n->id, mp.para, mp.style, mp.marks});
            }
        }
        for (const doc::Ref& c : n->children) walk(c);
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
                // 段落格式与字符格式同一枚标记，改一个必须原样带回另一个 ——
                //   否则"调行距"会静默清掉空单元格上的字重。
                s.marks = m.marks;
                s.beforeMarks = m.marks;
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
    const doc::Node* n = doc::findNode(doc(), p.container);
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
    const doc::Node* n = doc::findNode(doc(), p.container);
    if (n == nullptr) return true;
    if (isText(n->kind)) {
        // 文本节点内部：右边还有字。落点正好在末尾时按"容器里的下一个兄弟"判，
        // 那正是下面那一步要做的事，所以这里只区分"中间"与"末尾"。
        const auto* run = std::get_if<doc::RunPayload>(&n->payload);
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
    const FlowPos at = plan.steps.empty() ? pos().flowAt(caret) : plan.at;
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

bool EditCommand::currentTableCell(doc::CellRef* out, int* row, int* col) const {
    if (!hasFlow() || out == nullptr) return false;
    const int para = pos().caret().para;
    const CellNode* node =
        (view_.structure == nullptr) ? nullptr : view_.structure->cells_index.nodeAt(para);
    if (node == nullptr) return false;
    const CellSlot& slot = node->slot;
    if (!doc::cellRefAt(doc(), slot.table, slot.row, slot.cell, out)) return false;
    if (row != nullptr || col != nullptr) {
        int r = 0;
        int c = 0;
        if (!doc::cellOrigin(doc(), out->table, out->cell, &r, &c)) return false;
        if (row != nullptr) *row = r;
        if (col != nullptr) *col = c;
    }
    return true;
}

bool EditCommand::caretIntoCell(NodeId table, int row, int col) {
    TableGrid grid;
    std::string why;
    if (!doc::tableGrid(doc(), table, &grid, &why)) return false;
    if (grid.rows <= 0 || grid.cols <= 0) return false;
    const int r = std::max(0, std::min(row, grid.rows - 1));
    const int c = std::max(0, std::min(col, grid.cols - 1));
    const GridCell* slot = grid.at(r, c);
    if (slot == nullptr) return false;
    // 落在合并区域里时给的是原点格 —— 光标要落进那一格的内容，不是那个格位。
    doc::CellRef target;
    if (!doc::cellRefOf(doc(), table, slot->row, slot->col, &target)) return false;
    if (view_.structure == nullptr) return false;
    for (const CellNode& n : view_.structure->cells_index.nodes) {
        if (n.slot.table != static_cast<uint64_t>(target.table)) continue;
        if (n.slot.row != target.row_index || n.slot.cell != target.cell_index) continue;
        if (n.first_block < 0 || n.first_block >= static_cast<int>(blockFlow().size())) {
            return false;
        }
        pos().setCaretAtFlow(blockFlow()[static_cast<size_t>(n.first_block)]);
        return true;
    }
    return false;
}

bool EditCommand::insertTableRow(bool below) {
    if (!hasFlow()) return false;
    // 改表结构是设计动作：表单模式在填值、只读模式不许改。
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    int row = 0;
    int col = 0;
    if (!currentTableCell(&cell, &row, &col)) {
        noteBlocked();
        return false;
    }
    doc::TableSteps batch = doc::insertRow(doc(), flowIndex(), cell, below);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    // 光标进新行的那一列：插完就接着填，是"插入行"最常见的下一步。
    caretIntoCell(cell.table, below ? row + 1 : row, col);
    return true;
}

bool EditCommand::removeTableRow() {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    int row = 0;
    int col = 0;
    if (!currentTableCell(&cell, &row, &col)) {
        noteBlocked();
        return false;
    }
    doc::TableSteps batch = doc::removeRow(doc(), flowIndex(), cell);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    // 目标行号夹在删完之后的范围里：删的是最后一行时落到新的最后一行。
    caretIntoCell(cell.table, row, col);
    return true;
}

bool EditCommand::insertTableColumn(bool right) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    int row = 0;
    int col = 0;
    if (!currentTableCell(&cell, &row, &col)) {
        noteBlocked();
        return false;
    }
    doc::TableSteps batch = doc::insertColumn(doc(), flowIndex(), cell, right);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    // 插在左边时这一格整体右移一列，光标跟着它走（落在原内容上，不落到新插的空列里）。
    caretIntoCell(cell.table, row, right ? col : col + 1);
    return true;
}

bool EditCommand::removeTableColumn() {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    int row = 0;
    int col = 0;
    if (!currentTableCell(&cell, &row, &col)) {
        noteBlocked();
        return false;
    }
    doc::TableSteps batch = doc::removeColumn(doc(), flowIndex(), cell);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    // 这一列没了：光标落到顶替它的那一格（同列号，夹到新的列数里）。
    caretIntoCell(cell.table, row, col);
    return true;
}

bool EditCommand::mergeTableCells() {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const std::vector<CaretResolver::CellBlocks>& blocks_of_cells = pos().cellBlocks();
    if (blocks_of_cells.size() < 2) {
        // 没有整格选区就没有"要合并的那一片"：不替用户猜"和右边那一格并"。
        noteBlocked();
        return false;
    }
    std::vector<doc::CellRef> cells;
    for (const CaretResolver::CellBlocks& cb : blocks_of_cells) {
        if (cb.empty() || cb.first < 0) {
            noteBlocked();
            return false;
        }
        const CellNode* node =
            (view_.structure == nullptr) ? nullptr : view_.structure->cells_index.nodeAt(cb.first);
        if (node == nullptr) {
            noteBlocked();
            return false;
        }
        const CellSlot& slot = node->slot;
        doc::CellRef ref;
        if (!doc::cellRefAt(doc(), slot.table, slot.row, slot.cell, &ref)) {
            noteBlocked();
            return false;
        }
        cells.push_back(ref);
    }
    const NodeId table = cells.front().table;
    doc::TableSteps batch = doc::mergeCells(doc(), flowIndex(), cells);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    // 合并之后的落点是左上角那一格：整格选区随之收成"光标在合并后的那一格里"。
    int lo_row = 0;
    int lo_col = 0;
    {
        bool found = false;
        for (const doc::CellRef& c : cells) {
            int r = 0;
            int cc = 0;
            if (!doc::cellOrigin(doc(), table, c.cell, &r, &cc)) continue;
            if (!found || r < lo_row || (r == lo_row && cc < lo_col)) {
                lo_row = r;
                lo_col = cc;
                found = true;
            }
        }
        if (!found) {
            noteBlocked();
            return false;
        }
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    caretIntoCell(table, lo_row, lo_col);
    return true;
}

bool EditCommand::splitTableCell() {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    int row = 0;
    int col = 0;
    if (!currentTableCell(&cell, &row, &col)) {
        noteBlocked();
        return false;
    }
    doc::TableSteps batch = doc::splitCell(doc(), flowIndex(), cell);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    // 拆完留在左上角那一格：内容在它身上，用户的注意力也在它身上。
    caretIntoCell(cell.table, row, col);
    return true;
}

NodeId EditCommand::tableAtPara(int para) const {
    if (!view_.tables || view_.structure == nullptr) return kInvalidNodeId;
    const CellRef ref = view_.structure->cellAt(para);
    if (ref.table < 0 || ref.table >= static_cast<int>(view_.tables->size())) {
        return kInvalidNodeId;
    }
    return static_cast<NodeId>((*view_.tables)[static_cast<size_t>(ref.table)].handle);
}

bool EditCommand::removeTableAtCaret() {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const NodeId id = tableAtPara(pos().caret().para);
    if (id == kInvalidNodeId) {
        noteBlocked();
        return false;
    }
    FlowPos lo = 0;
    FlowPos hi = 0;
    if (!flowIndex().intervalOf(id, &lo, &hi) || hi <= lo) {
        noteBlocked();
        return false;
    }
    // 表是文档里唯一的内容时拒绝：删完一个块都不剩，光标没有能停的地方。
    int first_b = -1;
    int last_b = -1;
    const CellRef ref = view_.structure->cellAt(pos().caret().para);
    if (ref.table >= 0) {
        const TableSpan& span = view_.structure->tableSpan(ref.table);
        first_b = span.first_block;
        last_b = span.last_block;
    }
    if (first_b <= 0 && last_b >= static_cast<int>(blocks().size()) - 1) {
        noteBlocked();
        return false;
    }
    if (!doc::canEditRange(doc(), mode(), Range{lo, hi}).allowed) {
        noteBlocked();
        return false;
    }
    RemoveNode r;
    r.at_flow = lo;
    std::vector<Step> steps;
    steps.push_back(Step{r});
    pending_before_ = pos().selection();
    pending_after_ = Range{lo, lo};
    const int dirty = pos().caret().para;
    if (!commit(std::move(steps), dirty)) {
        noteBlocked();
        return false;
    }
    pos().setCaretAtFlow(lo);
    return true;
}

bool EditCommand::setCellSpec(const CellPatch& patch) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    if (!currentTableCell(&cell, nullptr, nullptr)) {
        noteBlocked();
        return false;
    }
    const doc::Node* n = doc::findNode(doc(), cell.cell);
    if (n == nullptr || n->kind != NodeKind::TableCell) return false;
    const auto* cur = std::get_if<CellSpec>(&n->payload);
    if (cur == nullptr) return false;

    CellSpec next = *cur;
    if (patch.v_align.has_value()) next.v_align = *patch.v_align;
    if (next == *cur) return true;  // 幂等：不提交、不进撤销栈

    SetCellSpec s;
    s.id = cell.cell;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    // 改记录不动光标：留着 null Range 会被当成"文档开头"。
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    const int dirty = pos().caret().para;
    if (!commit(std::move(steps), dirty)) {
        noteBlocked();
        return false;
    }
    return true;
}

bool EditCommand::setRowSpec(const RowPatch& patch) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    if (!currentTableCell(&cell, nullptr, nullptr)) {
        noteBlocked();
        return false;
    }
    const doc::Node* n = doc::findNode(doc(), cell.row);
    if (n == nullptr || n->kind != NodeKind::TableRow) return false;
    const auto* cur = std::get_if<RowSpec>(&n->payload);
    if (cur == nullptr) return false;

    RowSpec next = *cur;
    if (patch.is_header.has_value()) next.is_header = *patch.is_header;
    if (patch.can_split_by_page.has_value()) next.can_split_by_page = *patch.can_split_by_page;
    if (patch.page_break_before.has_value()) next.page_break_before = *patch.page_break_before;
    if (patch.height_pt.has_value()) {
        const float pt = *patch.height_pt;
        next.height = pt > 0.0f ? Length::abs(ptToEmu(pt)) : Length{};
    }
    if (patch.height_rule.has_value()) next.height_rule = *patch.height_rule;
    if (next == *cur) return true;

    SetRowSpec s;
    s.id = cell.row;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    const int dirty = pos().caret().para;
    if (!commit(std::move(steps), dirty)) {
        noteBlocked();
        return false;
    }
    return true;
}

bool EditCommand::setTableSpec(const TablePatch& patch) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    if (!currentTableCell(&cell, nullptr, nullptr)) {
        noteBlocked();
        return false;
    }
    const doc::Node* n = doc::findNode(doc(), cell.table);
    if (n == nullptr || n->kind != NodeKind::Table) return false;
    const auto* cur = std::get_if<TableSpec>(&n->payload);
    if (cur == nullptr) return false;

    TableSpec next = *cur;
    if (patch.width_pt.has_value()) {
        const float pt = *patch.width_pt;
        next.width = pt > 0.0f ? Length::abs(ptToEmu(pt)) : Length{};
    }
    if (patch.align.has_value()) next.align = *patch.align;
    if (next == *cur) return true;

    SetTableSpec s;
    s.id = cell.table;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    const int dirty = pos().caret().para;
    if (!commit(std::move(steps), dirty)) {
        noteBlocked();
        return false;
    }
    return true;
}

/**
 * 把一张表的全部列宽钉成显式值。宽度 <= 0 的那一列保持 Auto（与拖拽无关的列原样保留）。
 */
bool EditCommand::setTableGridCols(uint64_t handle, const std::vector<float>& widths) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const NodeId id = static_cast<NodeId>(handle);
    const doc::Node* n = doc::findNode(doc(), id);
    if (n == nullptr || n->kind != NodeKind::Table) return false;
    const auto* cur = std::get_if<TableSpec>(&n->payload);
    if (cur == nullptr) return false;

    TableSpec next = *cur;
    next.grid_cols.clear();
    next.grid_cols.reserve(widths.size());
    float sum = 0.0f;
    for (const float w : widths) {
        next.grid_cols.push_back(w > 0.0f ? Length::abs(ptToEmu(w)) : Length{});
        if (w > 0.0f) sum += w;
    }
    // 表宽一起钉住。不钉的话表还是"撑满版心"：各列之和超过它时整体按比例缩回去，
    //   拖右边框那一下看起来什么都没发生（列宽写入成功、读回的值也对，画面没动）。
    if (sum > 0.0f) next.width = Length::abs(ptToEmu(sum));
    if (next == *cur) return true;

    SetTableSpec s;
    s.id = id;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    if (!commit(std::move(steps), pos().caret().para)) {
        noteBlocked();
        return false;
    }
    return true;
}

/**
 * 改一行的高度。落成"不小于"：拖高之后内容多了仍能长，这是 Word 的行为。
 */
bool EditCommand::setTableRowHeight(uint64_t handle, int row, float h_pt) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef ref{};
    if (!doc::cellRefAt(doc(), handle, row, 0, &ref)) return false;
    const doc::Node* n = doc::findNode(doc(), ref.row);
    if (n == nullptr || n->kind != NodeKind::TableRow) return false;
    const auto* cur = std::get_if<RowSpec>(&n->payload);
    if (cur == nullptr) return false;

    RowSpec next = *cur;
    if (h_pt > 0.0f) {
        next.height = Length::abs(ptToEmu(h_pt));
        next.height_rule = HeightRule::AtLeast;
    } else {
        next.height = Length{};
        next.height_rule = HeightRule::Auto;
    }
    if (next == *cur) return true;

    SetRowSpec s;
    s.id = ref.row;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    if (!commit(std::move(steps), pos().caret().para)) {
        noteBlocked();
        return false;
    }
    return true;
}

/**
 * 把一段内容搬到 dest。删除与插入是同一批步骤：撤销只有一步，位置映射也按同一批算。
 */
bool EditCommand::moveRange(FlowPos lo, FlowPos hi, FlowPos dest,
                            const std::vector<NodeSpec>& nodes) {
    if (!hasFlow() || nodes.empty()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    if (!(hi > lo)) return false;
    // 目标落在源区间内部：搬过去再删掉，等于原地不动 —— 拒绝比"看起来什么都没发生"好。
    if (dest >= lo && dest <= hi) return false;
    // 块级元素（表格）不搬：整张表插进段落中间没有语义，而拆开它要另一套结构操作。
    if (hasBlockLevel(nodes)) return false;

    const FlowPos len = hi - lo;
    const FlowPos at = (dest >= hi) ? dest - len : dest;
    std::vector<Step> steps = eraseFlowSteps(lo, hi);
    if (steps.empty()) return false;
    FlowPos cur = at;
    for (const NodeSpec& spec : nodes) {
        InsertNode in;
        in.at_flow = cur;
        in.at = flowIndex().posAt(cur);
        in.subtree = spec;
        steps.push_back(Step{in});
        cur += static_cast<FlowPos>(spec.span());
    }
    pending_before_ = pos().selection();
    pending_after_ = Range{cur, cur};
    if (!commit(std::move(steps), pos().paraForFlow(at))) {
        noteBlocked();
        return false;
    }
    pos().setCaretAtFlow(cur);
    return true;
}

/**
 * 把一张图片搬到 dest。图片是原子，在流里占一格；搬它的位置就是"删一格 + 插一格"。
 */
bool EditCommand::moveImage(NodeId id, const ImageSpec& spec, FlowPos dest) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const doc::Node* parent = doc::parentOf(doc(), id);
    if (parent == nullptr) return false;
    int slot = -1;
    for (size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i]->id == id) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) return false;
    const FlowPos at = flowIndex().flowAt(Pos{parent->id, slot});
    if (at < 0) return false;
    // 原地：落点紧挨着图片（它前面或它后面）都不算移动。
    if (dest == at || dest == at + 1) return false;

    const FlowPos insert_at = (dest > at) ? dest - 1 : dest;
    NodeSpec sub;
    sub.kind = NodeKind::Image;
    sub.image = spec;

    std::vector<Step> steps;
    RemoveNode r;
    r.at_flow = at;
    steps.push_back(Step{r});
    InsertNode in;
    in.at_flow = insert_at;
    in.at = flowIndex().posAt(insert_at);
    in.subtree = sub;
    steps.push_back(Step{in});

    pending_before_ = pos().selection();
    pending_after_ = Range{insert_at + 1, insert_at + 1};
    if (!commit(std::move(steps), pos().paraForFlow(insert_at))) {
        noteBlocked();
        return false;
    }
    pos().setCaretAtFlow(insert_at + 1);
    return true;
}

/**
 * 换一张图的整份记录。一次一步撤销，与改尺寸同一档。
 */
bool EditCommand::setImageSpec(NodeId id, const ImageSpec& spec) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const doc::Node* n = doc::findNode(doc(), id);
    if (n == nullptr || n->kind != NodeKind::Image) return false;
    const auto* cur = std::get_if<ImageSpec>(&n->payload);
    if (cur == nullptr) return false;
    if (*cur == spec) return true;

    SetImageSpec s;
    s.id = id;
    s.spec = spec;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    if (!commit(std::move(steps), pos().caret().para)) {
        noteBlocked();
        return false;
    }
    return true;
}

/**
 * 改第一节的水印。脏段取第 0 段：水印画在每一页上，没有更窄的影响面。
 */
bool EditCommand::setWatermark(const WatermarkSpec& w) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const doc::Doc& d = doc();
    if (!d.root || d.root->children.empty()) return false;
    const doc::Node* sec = d.root->children.front().get();
    if (sec == nullptr || sec->kind != NodeKind::Section) return false;
    const auto* cur = std::get_if<SectionSpec>(&sec->payload);
    if (cur == nullptr) return false;
    if (cur->watermark == w) return true;

    SectionSpec next = *cur;
    next.watermark = w;
    SetSectionSpec s;
    s.id = sec->id;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    if (!commit(std::move(steps), 0)) {
        noteBlocked();
        return false;
    }
    return true;
}

}  // namespace heditor
