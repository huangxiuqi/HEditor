/**
 * 命令层的实现：一次编辑怎么变成一条事务（声明与边界见 edit_command.h）。
 *
 * 插入与删除先过落点判定（policyAt / policyErase），再按字素簇吸附；跨段编辑（分段、合并）
 * 与结构操作（删控件、改控件属性）走同一条"造事务 → 交出去"的路 —— 于是撤销、重排、重绘、
 * 记账自动一致，不必另开一条特权通道。
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

using namespace detail;

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
    // 换行不能进段文本：段落边界只由段落标记表达，塞进一个 U+000A 会让段号出现两套算法
    //   （按块取与按分隔符切），同一份文档给出不同的段数。吃字符串的两条公开入口已经在
    //   含换行时折成节点，走到这里的换行只剩非法调用，整批拒绝。
    if (std::any_of(cps.begin(), cps.end(), [](uint32_t cp) { return cp == '\n' || cp == '\r'; })) {
        noteBlocked();
        return false;
    }
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
    if (!hasLineBreak(utf8)) return insertCodepoints(para, offset, utf8ToCodepoints(utf8));
    const TextPos at{para, offset};
    const std::vector<NodeSpec> nodes = nodesFromPlainText(utf8);
    // 落点正好是光标且有选区：这一次输入是"替换选中内容"，与不含换行时同一条分派。
    if (at == pos().caret() && pos().hasSelection()) return pasteNodes(nodes);
    return insertNodesAt(at, nodes);
}

bool EditCommand::typeTextUtf8(const std::string& utf8) {
    if (!hasLineBreak(utf8)) {
        return insertCodepoints(pos().caret().para, pos().caret().offset, utf8ToCodepoints(utf8));
    }
    // 落点就是光标，先把选区删掉再插 —— 与打字替换选中内容同一口径。
    return pasteNodes(nodesFromPlainText(utf8));
}

bool EditCommand::insertNodesAt(const TextPos& at, const std::vector<NodeSpec>& nodes) {
    if (nodes.empty() || !hasFlow()) return false;
    if (!policyAt(at).allowed) {
        noteBlocked();
        return false;
    }
    const FlowPos af = pos().flowAt(at);
    if (af < 0) {
        noteBlocked();
        return false;
    }
    return insertNodesAtFlow(af, {}, Range{}, nodes);
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

}  // namespace heditor
