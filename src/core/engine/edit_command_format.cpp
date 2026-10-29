/**
 * 命令层的格式应用：把一份补丁落到选区或落点，以及缩进、删除选区、粘贴节点这几条。
 *
 * 补丁是"只发变化量"的那一份，算不算一步由它自己判（没变化就不提交）。
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
    return insertNodesAtFlow(at, plan.steps, plan.before, nodes);
}

bool EditCommand::insertNodesAtFlow(FlowPos at, const std::vector<Step>& erase, const Range& before,
                                    const std::vector<NodeSpec>& nodes) {
    std::vector<Step> steps = erase;
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

    pending_before_ = erase.empty() ? Range{at, at} : before;
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
