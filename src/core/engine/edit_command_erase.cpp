/**
 * 命令层的选区删除：一段区间怎么变成"整块控件摘除 + 文本删除 + 段落标记合并"那批步骤。
 *
 * 只做规划与落步，收尾统一交给 commitErase —— 撤销、重排、记账与别的编辑同一条路。
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

}  // namespace heditor
