/**
 * 光标与位置解析的实现。搬自 engine.cpp 与 engine_layout.cpp，一字未改语义 ——
 * 只把成员名换成视图（view_）与解析器自己的状态（sel_ 等），
 * 并把"移动光标之后要重排"那一笔交回引擎（见 EditorEngine 的同名转发层）。
 */
#include "core/engine/caret.h"
#include "core/engine/control_spans.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace heditor {

/**
 * 把光标摆到 (段, 段内码点偏移, 侧别)。这是"文本坐标 → 流位置"的唯一约定，
 * 只给没有侧别信息的入口用（宿主程序化定位、Home/End、Tab 的目标）；编辑路径一律走
 * setCaretAtFlow / setCaretRel —— 侧别由操作自己算出来。
 *
 * 只摆位置、不触发重排：聚焦档下"显示哪些括号"由光标位置决定，摆一次之后占位集合可能变、
 * 对象表跟着变，而 rel 是按对象表算出来的 —— 所以"摆两次"这件事（先摆让重排看到新位置、
 * 重排换表之后再摆一次）留在引擎侧（EditorEngine::projectCaret）。
 */
void CaretResolver::placeCaret(int para, size_t offset, int side, bool extend, int line_aff) {
    if (view_.blocks->empty()) {
        setCaretRel(0, 0, !extend);
        return;
    }
    if (para < 0) para = 0;
    if (para >= static_cast<int>(view_.blocks->size()))
        para = static_cast<int>(view_.blocks->size()) - 1;
    const size_t at = snapToCluster(para, offset);
    setCaretRel(para, relAt(para, at, (side < 0) ? defaultSideAt(para, at) : side), !extend,
                line_aff);
}

/**
 * 重排之后重新投影一次光标：位置的输入（段, 偏移, 侧别）没变，但排版换了簇表，
 * 簇吸附要按新表再来一遍 —— 这就是 EditorEngine::projectCaret 摆两次里的第二次。
 * 收敛：第二次不会再触发重排。
 */
int CaretResolver::controlIndexOfOwner(int owner) const {
    if (view_.owner_index == nullptr) return -1;
    const auto it = view_.owner_index->find(owner);
    return it == view_.owner_index->end() ? -1 : it->second;
}

int CaretResolver::slotOwnerOf(int control_index) const {
    if (control_index < 0) return control_index;
    if (view_.control_node_ids == nullptr) return -1;
    if (control_index >= static_cast<int>(view_.control_node_ids->size())) return -1;
    return static_cast<int>((*view_.control_node_ids)[static_cast<size_t>(control_index)]);
}

void CaretResolver::rebaseSelection(int anchor_para, int anchor_rel, int focus_para,
                                    int focus_rel) {
    setCaretRel(focus_para, focus_rel);
    sel_.anchor = flowForRel(anchor_para, anchor_rel);
}

void CaretResolver::reprojectCaret(bool extend) {
    if (view_.blocks->empty()) return;
    const TextPos p = caret();
    const size_t at = snapToCluster(p.para, p.offset);
    setCaretRel(p.para, relAt(p.para, at, caretSide()), !extend);
}

/**
 * 边界括号的统计。侧别与流位置都从它推出来 —— 两者是同一件事的两种说法：
 * 流位置 = 码点偏移 + 排在它前面的边界数 + 侧别。
 */
const std::vector<InlineObject>& CaretResolver::objectsFor(int para) const {
    static const std::vector<InlineObject> kNone;
    if (para < 0 || para >= static_cast<int>(view_.blocks_full->size())) return kNone;
    return (*view_.blocks_full)[static_cast<size_t>(para)].objects;
}

CaretResolver::BracketCounts CaretResolver::bracketCountsAt(int para, size_t cp) const {
    BracketCounts bc;
    for (const InlineObject& o : objectsFor(para)) {
        if (static_cast<size_t>(o.cp) < cp) {
            ++bc.before;
            continue;
        }
        if (static_cast<size_t>(o.cp) > cp) break;
        if (o.kind == InlineObjectKind::ControlStart) {
            ++bc.opens;
        } else {
            ++bc.closes;
        }
    }
    return bc;
}

int CaretResolver::relAt(int para, size_t cp, int side) const {
    const BracketCounts bc = bracketCountsAt(para, cp);
    // 侧别夹到本偏移实际有的枚数：位置还在、边界却换了一批时（例如那个空字段被删掉），
    //   旧侧别可能指向已经不存在的格子 —— 夹到最后一格，而不是给出一个越界的流位置。
    const int max_side = bc.closes + bc.opens;
    const int s = (side < 0) ? 0 : (side > max_side ? max_side : side);
    return static_cast<int>(cp) + bc.before + s;
}

/**
 * 文本码点区间 [a, b) 在流里占的那一段（段内相对，半开）。
 *
 * 括号不是文本：它们只占位置、不进这个区间。所以起点是"a 那个字自己的位置"（＝跨过这个
 * 偏移上所有括号之后），终点是"b 那个字自己的位置"。两端都不猜 —— 用"最内层含端点"猜
 * 侧别的话，删除区间会多含一枚括号：删"天"把 ]中 也圈进去，删完光标就落到括号的另一侧。
 *
 * 判定（policyErase）与实际删除（eraseOnTree）共用它：两处一旦分叉，就会出现
 * "判据说能删、真删却越界"这种只在边界上冒出来的错。
 */
bool CaretResolver::textSpanAt(int para, size_t a, size_t b, int* from, int* to) const {
    if (!view_.hasFlow() || a > b) return false;
    const BracketCounts ba = bracketCountsAt(para, a);
    const BracketCounts bb = bracketCountsAt(para, b);
    if (from) *from = static_cast<int>(a) + ba.before + ba.closes + ba.opens;
    if (to) *to = static_cast<int>(b) + bb.before;
    return true;
}

/**
 * 不带侧别时的默认侧别。
 *
 * 规则是"最内层含端点" —— 与编辑归属同一条：区间树里包含这个
 * 偏移的最深那个控件，同深度取下标小者。于是"程序化定位到字段末尾"落在 ] 之内、
 * "定位到字段开头"落在 [ 之后，相邻空字段则进到它自己那一对括号之间。
 *
 * 为什么不由括号排布直接算：同一个偏移上有几个位置，光看"有几枚括号"分不出该停哪个 ——
 * 归属那套（区间包含 + 嵌套深度 + 下标次序）才是既有判据钉住的答案，两条口径必须同源。
 * 控件集合为空时没有可归的地方，停在所有括号之前。
 */
int CaretResolver::defaultSideAt(int para, size_t cp) const {
    // 归属按文字位置判，不能按流位置判：同一个文字偏移上已经压着好几个插入点，
    //   靠里的那几格可能已经属于下一个控件（两个字段紧挨着时，],"["之后的第二格就在
    //   后一个字段里）。区间树回答的正是"这个文字偏移归谁"，流位置回答"这一格在哪"，
    //   两者是链的两端 —— 拿后者当默认侧别的输入会跳过字段边界。
    if (view_.controls != nullptr && !view_.controls->empty()) {
        const int ci = view_.controls->innermostAt(TextPos{para, cp}, /*include_end=*/true);
        if (ci >= 0) return sideFromHint(para, cp, ci);
    }
    return bracketCountsAt(para, cp).closes;
}

int CaretResolver::sideFromHint(int para, size_t cp, int hint) const {
    const BracketCounts bc = bracketCountsAt(para, cp);
    if (hint == LineBox::kInlineOffsetOuter) return bc.closes;
    if (hint < 0) return defaultSideAt(para, cp);
    const int owner = slotOwnerOf(hint);
    if (owner < 0) return bc.closes;
    int local = 0;
    for (const InlineObject& o : objectsFor(para)) {
        if (static_cast<size_t>(o.cp) < cp) continue;
        if (static_cast<size_t>(o.cp) > cp) break;
        if (o.owner != owner) {
            ++local;
            continue;
        }
        // 开括号：落点在它之后（内容从此开始）；闭括号：落点在它自己那一格（内容到此为止）。
        return (o.kind == InlineObjectKind::ControlStart) ? (local + 1) : local;
    }
    // 这个控件在这一处没有自己的边界（落点压在更内层的边界上）→ 位置在那些括号之外。
    return bc.closes;
}

/**
 * 某个位置（不是光标）的侧别：按"最内层含端点"归一次，归不到就是"所有控件之外"。
 * 删除区间的两个端点、诊断打印这类没有侧别信息的入口走它。
 */
int CaretResolver::sideForPos(TextPos p) const {
    if (p == caret()) return caretSide();
    return defaultSideAt(p.para, p.offset);
}

FlowPos CaretResolver::flowPosAtSide(int para, size_t cp_offset, int side) const {
    if (!view_.hasFlow() || para < 0 || para >= static_cast<int>(view_.block_flow->size()))
        return -1;
    if (cp_offset > (*view_.blocks_full)[static_cast<size_t>(para)].cp_text.size())
        cp_offset = (*view_.blocks_full)[static_cast<size_t>(para)].cp_text.size();
    return (*view_.block_flow)[static_cast<size_t>(para)] + relAt(para, cp_offset, side);
}

FlowPos CaretResolver::flowPosAt(int para, size_t cp_offset, int owner_hint) const {
    if (!view_.hasFlow() || para < 0 || para >= static_cast<int>(view_.block_flow->size()))
        return -1;
    if (cp_offset > (*view_.blocks_full)[static_cast<size_t>(para)].cp_text.size())
        cp_offset = (*view_.blocks_full)[static_cast<size_t>(para)].cp_text.size();
    return flowPosAtSide(para, cp_offset, sideFromHint(para, cp_offset, owner_hint));
}

int CaretResolver::cpOffsetInBlock(int para, int rel) const {
    const std::vector<InlineObject>& objs = objectsFor(para);
    int consumed = 0;
    for (size_t j = 0; j < objs.size(); ++j) {
        // 第 j 枚边界的块内流偏移：锚点 + 排在它前面的边界数。
        const int opos = objs[j].cp + static_cast<int>(j);
        if (rel == opos) return objs[j].cp;  // 正落在边界上：它对应的就是那个码点偏移
        if (rel < opos) break;
        ++consumed;
    }
    return rel - consumed;
}

bool CaretResolver::flowToTextPos(FlowPos flow, TextPos* out) const {
    if (!view_.hasFlow() || view_.block_flow->empty()) return false;
    if (flow < 0) flow = 0;
    const int n = static_cast<int>(view_.block_flow->size());
    for (int i = 0; i < n; ++i) {
        const FlowPos base = (*view_.block_flow)[static_cast<size_t>(i)];
        if (flow < base) {
            // 落在本块之前：那是段落标记或节边界，模型里没有"段间"的位置，归到本段段首。
            if (out != nullptr) *out = TextPos{i, 0};
            return true;
        }
        const int rel = flow - base;
        if (rel > (*view_.block_flow_span)[static_cast<size_t>(i)]) continue;
        if (out != nullptr) {
            *out = TextPos{i, static_cast<size_t>(cpOffsetInBlock(i, rel))};
        }
        return true;
    }
    const size_t last = view_.block_flow->size() - 1;
    if (out != nullptr)
        *out = TextPos{static_cast<int>(last), (*view_.blocks_full)[last].cp_text.size()};
    return true;
}

/**
 * 段内相对流位置 → 全局流位置（夹到本段跨度内）。
 *
 * 夹取只做范围收缩，不做簇吸附（簇表要排版之后才有，见 relayout 里的说明）。
 * 上界取 view_.block_flow_span（流索引给的权威段长）而不是显示名单筛过的那张对象表的长度：
 * 前者是树的口径，后者随聚焦档变化，拿它当上界会在换档时把光标往回拉。
 */
FlowPos CaretResolver::flowForRel(int para, int rel) const {
    if (view_.block_flow->empty()) return 0;
    if (para < 0) para = 0;
    if (para >= static_cast<int>(view_.block_flow->size()))
        para = static_cast<int>(view_.block_flow->size()) - 1;
    const int span = (*view_.block_flow_span)[static_cast<size_t>(para)];
    if (rel > span) rel = span;
    if (rel < 0) rel = 0;
    return (*view_.block_flow)[static_cast<size_t>(para)] + rel;
}

/**
 * 任意流位置落在哪一块（派生）。view_.block_flow 单调，二分取最后一个起点不超过它的块。
 * caretPara 是它在 focus 上的特例 —— 选区另一端的段号也走这里。
 */
int CaretResolver::paraForFlow(FlowPos f) const {
    if (view_.block_flow->empty()) return 0;
    const auto it = std::upper_bound(view_.block_flow->begin(), view_.block_flow->end(), f);
    const size_t i = static_cast<size_t>(it - view_.block_flow->begin());
    return i == 0 ? 0 : static_cast<int>(i - 1);
}

/**
 * 任意流位置的段内流位置（派生）= 它减本段起点。
 */
int CaretResolver::relForFlow(FlowPos f) const {
    if (view_.block_flow->empty()) return 0;
    const int para = paraForFlow(f);
    const int rel = f - (*view_.block_flow)[static_cast<size_t>(para)];
    return rel < 0 ? 0 : rel;
}

/**
 * 任意流位置的侧别（派生）：段内流位置减去"这个偏移在流里占的起点"就是跨过的边界枚数。
 * 没有"存着的侧别"可以失配，所以既不需要失效标记，也不需要拿边界枚数去卡它。
 */
int CaretResolver::sideForFlow(FlowPos f) const {
    const int para = paraForFlow(f);
    const int rel = relForFlow(f);
    int off = cpOffsetInBlock(para, rel);
    if (off < 0) off = 0;
    const BracketCounts bc = bracketCountsAt(para, off);
    const int side = rel - off - bc.before;
    return side < 0 ? 0 : side;
}

bool CaretResolver::clusterTableFresh(int para, const std::vector<uint32_t>& starts) const {
    if (starts.empty()) return false;
    if (para < 0 || para >= static_cast<int>(view_.blocks_full->size())) return false;
    // 表尾是排版那一刻这一段的码点数（见 LayoutResult::para_cluster_starts 的生成）。
    //   与当前长度对不上，就说明排版之后文档又改过 —— 表是旧尺子。
    return starts.back() == (*view_.blocks_full)[static_cast<size_t>(para)].cp_text.size();
}

size_t CaretResolver::snapToCluster(int para, size_t offset) const {
    if (para < 0 || para >= static_cast<int>(view_.blocks->size())) return 0;
    const size_t len = (*view_.blocks)[static_cast<size_t>(para)].cp_text.size();
    if (offset > len) offset = len;
    if (para < static_cast<int>(view_.layout->para_cluster_starts.size())) {
        const std::vector<uint32_t>& starts =
            view_.layout->para_cluster_starts[static_cast<size_t>(para)];
        // 旧表不吸附：按旧边界往下吸会吸到上一个字（表象是"第二个字插到第一个字前面"，
        //   或者落到控件外面）。不吸附最多是不合并组合字符，代价小于整体前移一个字。
        if (clusterTableFresh(para, starts)) return clusterFloor(starts, offset);
    }
    return offset;
}

size_t CaretResolver::clusterCeilAt(int para, size_t offset) const {
    if (para < 0 || para >= static_cast<int>(view_.layout->para_cluster_starts.size()))
        return offset;
    const std::vector<uint32_t>& starts =
        view_.layout->para_cluster_starts[static_cast<size_t>(para)];
    if (!clusterTableFresh(para, starts)) return offset;
    return clusterCeil(starts, offset);
}

/**
 * 光标侧别（派生）= 光标流位置的侧别，与 caret() 同源，不可能不同步。
 */
int CaretResolver::caretSide() const { return sideForFlow(caretFlow()); }

int CaretResolver::caretFieldHint() const {
    if (!view_.hasFlow() || view_.control_node_ids == nullptr || view_.control_node_ids->empty())
        return -1;
    const int ci = innermostControlAtFlow(view_.flowIndex(), *view_.control_node_ids, caretFlow());
    return ci >= 0 ? ci : LineBox::kInlineOffsetOuter;
}

/**
 * 唯一写入点：段内流位置。其余入口都只是把它换算成全局流位置再调这里。
 */
void CaretResolver::setCaretRel(int para, int rel, bool collapse, int line_aff) {
    sel_.focus = flowForRel(para, rel);
    // 定位与扩展都按文字选区记：整格粒度只属于一次整格动作。
    //   跨格拖选的那一份由引擎在扩展之后补上（见 EditorEngine::syncCellSelection）。
    clearCellRange();
    if (collapse) {
        sel_.anchor = sel_.focus;
        // 列位是一串连续上下键之间的事：任何别的定位都让它从头取。
        vertical_goal_valid_ = false;
    }
    // 行别：显式给了就听（点击落点挨着它点的那一行），否则按"贴着左边那一格所在的行"算。
    //   只按偏移投影的话，折行处那些位置（闭括号之后、段中最末一格的右沿）会一律
    //   画成上一行行尾 —— 键盘移过去看着像跳回上一行了。
    caret_line_aff_ = (line_aff >= 0) ? (line_aff > 0 ? 1 : 0) : lineAffinityAtFlow(sel_.focus);
}

int CaretResolver::lineAffinityAtFlow(FlowPos f) const {
    if (!view_.hasFlow() || view_.layout == nullptr || view_.layout->lines.empty()) return 0;
    const int para = paraForFlow(f);
    const FlowPos base = (*view_.block_flow)[static_cast<size_t>(para)];
    const int n = static_cast<int>(view_.layout->lines.size());
    int aff = 0;
    for (int i = 0; i < n; ++i) {
        const LineBox& lb = view_.layout->lines[static_cast<size_t>(i)];
        if (lb.paragraph < para) continue;
        if (lb.paragraph > para) break;
        // 段首那一行没有"上一行行尾"这回事：它前面就是段的开头。
        if (lb.first_cp == 0) continue;
        // 这一行从哪个流位置开始（行首若有括号占位，起点就是那枚括号的位置）。
        const FlowPos start = base + relAt(para, lb.first_cp, 0);
        if (f < start) break;
        // 正好压在行首那一格上 = 贴上一行行尾；越过它（括号之后）才是贴这一行。
        //   取"最后一行 start <= f"的答案，不是第一行 —— 折行点不止一处。
        aff = (f > start) ? 1 : 0;
    }
    return aff;
}

/**
 * 光标的流位置：整格选区钉在焦点那一格的末尾（见 cell_caret_），其余情形就是 focus。
 */
FlowPos CaretResolver::caretFlow() const {
    if (sel_cell_ && cell_caret_ >= 0) return cell_caret_;
    return sel_.focus;
}

/**
 * 光标那一段（派生）：focus 落在哪一块。
 *
 * 不能借 flowToTextPos：那个入口的职责是"把流位置翻成文本坐标"，段间的位置会被归到相邻段
 * 端点；这里要的是"focus 归属哪一块"，段尾那个位置（段落标记所在位）必须仍算本段。
 */
int CaretResolver::caretPara() const { return paraForFlow(caretFlow()); }

/**
 * 段内流位置（派生）= 光标流位置减本段起点。
 */
int CaretResolver::caretRel() const { return relForFlow(caretFlow()); }

/**
 * 光标位置（派生）：段号 + 段内码点偏移，由光标的流位置投影出来 —— 不是独立状态。
 */
TextPos CaretResolver::caret() const {
    int para = caretPara();
    if (!view_.blocks->empty() && para >= static_cast<int>(view_.blocks->size()))
        para = static_cast<int>(view_.blocks->size()) - 1;
    int off = cpOffsetInBlock(para, caretRel());
    if (off < 0) off = 0;
    return TextPos{para, static_cast<size_t>(off)};
}

/**
 * 按对外坐标（段, 段内码点偏移）写光标，侧别按"最内层含端点"投影一次。
 * 这是文本坐标投影到流位置的唯一约定，只给"没有侧别信息"的入口用（宿主程序化定位、
 * 排版收口）；编辑路径一律走 setCaretAtFlow / setCaretRel —— 侧别由操作自己算出来。
 */
void CaretResolver::setCaretByText(TextPos p) {
    if (view_.blocks->empty()) {
        setCaretRel(0, 0);
        return;
    }
    int para = p.para;
    if (para < 0) para = 0;
    if (para >= static_cast<int>(view_.blocks->size()))
        para = static_cast<int>(view_.blocks->size()) - 1;
    size_t off = p.offset;
    const size_t len = (*view_.blocks)[static_cast<size_t>(para)].cp_text.size();
    if (off > len) off = len;
    setCaretRel(para, relAt(para, off, defaultSideAt(para, off)));
}

/**
 * 把光标摆到一个流位置上（绝对）。位置与侧别一次算准 —— 不猜、不查区间。
 *
 * 编辑路径全用它：删除后的落点是删除区间的起点、插入后是插入区间的终点、Tab 的目标、
 * 撤销恢复的位置，它们都已经是流位置。只留下一个码点偏移、再由"最内层含端点"猜回侧别的话，
 * 猜错的表象就是"删一个字光标跳一格"：删完"天"，落点会被猜成 ]中 之前，
 * 而它该在 ]中 之后（＝被删那个字所在的位置）。
 */
bool CaretResolver::setCaretAtFlow(FlowPos flow) {
    if (!view_.hasFlow() || flow < 0) return false;
    TextPos p{};
    if (!flowToTextPos(flow, &p)) return false;
    if (p.para < 0 || p.para >= static_cast<int>(view_.block_flow->size())) return false;
    // 段内流位置 = 全局流位置 - 本段起点。位置与侧别由此一次算准（侧别是派生的）。
    setCaretRel(p.para, static_cast<int>(flow) - (*view_.block_flow)[static_cast<size_t>(p.para)]);
    return true;
}

/**
 * 程序化设选区（宿主恢复、探针摆位）。两端各自夹到文档流的合法范围内 —— 越界的那
 * 一端收到能到的最远处，不会让一个悬空端点把后面的几何全带偏。
 */
bool CaretResolver::setSelection(Range r) {
    if (!view_.hasFlow() || view_.block_flow->empty()) return false;
    const size_t last = view_.block_flow->size() - 1;
    const FlowPos hi = (*view_.block_flow)[last] + (*view_.block_flow_span)[last];
    const auto clamp_flow = [hi](FlowPos v) -> FlowPos {
        if (v < 0) return 0;
        return v > hi ? hi : v;
    };
    sel_.anchor = clamp_flow(r.anchor);
    sel_.focus = clamp_flow(r.focus);
    clearCellRange();
    vertical_goal_valid_ = false;
    // 行别同列位一个待遇：它是"点击落点"这一个动作的产物，程序化设选区不带它。
    caret_line_aff_ = 0;
    return true;
}

/**
 * 只搬锚端，焦点端与行别都不动。
 *
 * 不能借 setSelection 做这件事：那会把行别清成 0，折行处的焦点端会画到上一行行尾。
 */
bool CaretResolver::setAnchorAtFlow(FlowPos anchor) {
    if (!view_.hasFlow() || view_.block_flow->empty()) return false;
    const size_t last = view_.block_flow->size() - 1;
    const FlowPos hi = (*view_.block_flow)[last] + (*view_.block_flow_span)[last];
    sel_.anchor = anchor < 0 ? 0 : (anchor > hi ? hi : anchor);
    // 锚端换了，整格吸附要按新的两端重算（由引擎在扩展之后调 syncCellSelection）。
    clearCellRange();
    return true;
}

/**
 * 取消选区：anchor 收到 focus，焦点端不动 —— 光标留在原地。
 */
void CaretResolver::clearSelection() {
    sel_.anchor = sel_.focus;
    clearCellRange();
    vertical_goal_valid_ = false;
}

/**
 * 全选：文档首到文档末。
 *
 * 末端是"最后一块的段内末端"（块起点 + 本块占的位数），而不是下一块之前那个段落标记
 * 所在的位置 —— 段落标记不属于任何一段的内容，够不到它。
 */
bool CaretResolver::selectAll() {
    if (!view_.hasFlow() || view_.block_flow->empty()) return false;
    const size_t last = view_.block_flow->size() - 1;
    sel_.anchor = (*view_.block_flow)[0];
    sel_.focus = (*view_.block_flow)[last] + (*view_.block_flow_span)[last];
    // 全选按整格铺满（只影响表格里的格，正文行盒没有格范围，走文字那一支）。
    //   与 Word 同一条：Ctrl+A 之后表格看起来是整片反白，而不是只剩字上有颜色。
    //   端点不吸附：全篇本来就没有"格边界"可言（见 selection() 的那一支）。
    clearCellRange();
    sel_cell_ = true;
    vertical_goal_valid_ = false;
    return true;
}

/**
 * 整格选区：收的一串块区间（见 CellBlocks），流包络由它们现算。
 *
 * 包络只当摘要 —— 选的到底是哪几格，只能由块区间回答：空格的内容长度是 0，流区间
 * 退化成点，而"点"与下一格的第一段共用一个流位置，分不开。
 */
bool CaretResolver::setCellBlocks(std::vector<CellBlocks> cells, FlowPos caret_flow) {
    clearCellRange();
    if (!view_.hasFlow() || view_.block_flow->empty() || cells.empty()) return false;
    const size_t n_blocks = view_.block_flow->size();
    // 越界的块区间丢掉（表格结构刚变过时会出现），剩下的仍是原顺序。
    cells.erase(std::remove_if(cells.begin(), cells.end(),
                               [n_blocks](const CellBlocks& c) {
                                   return c.empty() || c.first < 0 ||
                                          static_cast<size_t>(c.first) >= n_blocks ||
                                          static_cast<size_t>(c.first + c.count) > n_blocks;
                               }),
                cells.end());
    if (cells.empty()) return false;
    std::sort(cells.begin(), cells.end(),
              [](const CellBlocks& a, const CellBlocks& b) { return a.first < b.first; });
    const size_t last = n_blocks - 1;
    const FlowPos end = (*view_.block_flow)[last] + (*view_.block_flow_span)[last];
    const auto flow_of = [this](int block) -> FlowPos {
        return (*view_.block_flow)[static_cast<size_t>(block)];
    };
    const auto flow_end_of = [this](int block) -> FlowPos {
        return (*view_.block_flow)[static_cast<size_t>(block)] +
               (*view_.block_flow_span)[static_cast<size_t>(block)];
    };
    // 包络：每格取"首块起点 / 末块终点"，再取 min/max。空格两头相等，包络因此可能退化
    //   —— 那是合法的（"选中了一个空格"这件事照样成立，只是没有内容可删）。
    cell_lo_ = flow_of(cells.front().first);
    cell_hi_ = flow_end_of(cells.front().first + cells.front().count - 1);
    for (const CellBlocks& c : cells) {
        cell_lo_ = std::min(cell_lo_, flow_of(c.first));
        cell_hi_ = std::max(cell_hi_, flow_end_of(c.first + c.count - 1));
    }
    // sel_ 一个字都不动：它是用户落下的两个原值，拖选每动一下都要靠它判
    //   "首尾还在不在同一格"（往回拖要能退回文字选区）。把它改写成包络，反向拖选
    //   就再也长不回去 —— 锚点被搬到了最早那一格的起点上。
    //   包络只当摘要交给 selection()（见头文件的这一段），端点吸附由调用方决定：
    //   单格那一路（selectCellAtPara）自己先 setSelection。
    cell_mask_.clear();
    for (const CellBlocks& c : cells) {
        const int lo = c.first;
        const int hi = c.first + c.count;
        if (!cell_mask_.empty() && lo <= cell_mask_.back().focus) {
            cell_mask_.back().focus = std::max(cell_mask_.back().focus, hi);
        } else {
            cell_mask_.push_back(Range{lo, hi});
        }
    }
    cell_blocks_ = std::move(cells);
    sel_cell_ = true;
    if (caret_flow < 0) {
        cell_caret_ = -1;
    } else {
        cell_caret_ = caret_flow > end ? end : caret_flow;
    }
    // 整格选的插入点永远不在折行那边界上（它在格末），行别与列位都归零。
    caret_line_aff_ = 0;
    vertical_goal_valid_ = false;
    return true;
}

void CaretResolver::clearCellRange() {
    cell_lo_ = -1;
    cell_hi_ = -1;
    cell_blocks_.clear();
    cell_mask_.clear();
    cell_caret_ = -1;
    sel_cell_ = false;
}

/**
 * 某个块是否落在某个被选中的格里。二分找"起点不超过它的最后一份"，再看它是否还在里面。
 * 没有逐格信息时恒真：全选那一路按包络铺满才是它的语义。
 */
bool CaretResolver::cellCoversPara(int para) const {
    if (cell_mask_.empty()) return true;
    size_t lo = 0;
    size_t hi = cell_mask_.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cell_mask_[mid].anchor <= para) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) return false;
    return para < cell_mask_[lo - 1].focus;
}

/**
 * 找到包含该位置的行盒。
 *
 * 折行处有两个候选：上一行的末尾与下一行的开头是同一个位置（偏移与侧别都一样），
 * aff 就是选哪一个 —— 0 取先遇到的那一枚（上一行），大于 0 取最后一枚（下一行）。
 * 不选的话点下一行行首会画到上一行行尾，连按两下 Down 也会"原地不动"（当前行取成
 * 上一行，Down 落到下一行，位置与光标一模一样）。
 */
const LineBox* CaretResolver::findLineFor(const TextPos& pos, int aff) const {
    const LineBox* best = nullptr;
    const LineBox* first_hit = nullptr;
    const LineBox* last_hit = nullptr;
    for (const LineBox& lb : view_.layout->lines) {
        if (lb.paragraph != pos.para) continue;
        if (pos.offset >= lb.first_cp && pos.offset <= lb.last_cp) {
            if (first_hit == nullptr) first_hit = &lb;
            last_hit = &lb;
        }
        if (lb.first_cp <= pos.offset)
            best = &lb;
        else if (!best)
            best = &lb;
    }
    if (aff > 0 && last_hit != nullptr) return last_hit;
    if (first_hit != nullptr) return first_hit;
    return best;
}

bool CaretResolver::caretRectFor(const TextPos& pos, int* page_out, Rect* out) const {
    // 行别只对"当前光标"这一处有意义（它是点画布时定下来的，见 setCaretAtSideLine）。
    //   选区另一端与诊断打印没有这个信息，按"贴上一行"算。
    const bool is_caret = (pos == caret());
    const LineBox* lb = findLineFor(pos, is_caret ? caret_line_aff_ : 0);
    if (!lb) return false;
    // 侧别只对"当前光标"这一处有意义（它是点画布时定下来的，见 setCaretAtSide）。
    //   其它位置（选区另一端、诊断打印）没有这个信息，按"最内侧"算。
    // 侧别直接就是排版侧的"跨过几枚簇"，不再经过控件下标 —— 括号占位各占一枚簇，
    //   两边说的是同一件事，中间那次翻译（下标 ↔ 节点 id）正是"画到另一侧去"的来源。
    const int side = is_caret ? caretSide() : defaultSideAt(pos.para, pos.offset);
    const float off = lb->offsetAtSide(static_cast<uint32_t>(pos.offset), side);
    const float t = caret_width_pt_;
    Rect r;
    if (!lb->vertical) {
        // 横排：光标是竖条，贴在行盒左/右，高度 = 行盒高
        r.x = lb->inline_origin_pt + off - t * 0.5f;
        r.y = lb->block_lo_pt;
        r.w = t;
        r.h = std::max(0.0f, lb->block_hi_pt - lb->block_lo_pt);
    } else {
        // 竖排（含侧倒）：光标是横条，贴在列盒上/下，宽度 = 列盒宽
        r.x = lb->block_lo_pt;
        r.y = lb->inline_origin_pt + off - t * 0.5f;
        r.w = std::max(0.0f, lb->block_hi_pt - lb->block_lo_pt);
        r.h = t;
    }
    if (page_out) *page_out = lb->page;
    if (out) *out = r;
    return true;
}

bool CaretResolver::caretRectPt(Rect* out) const {
    int page = 0;
    return caretRectFor(caret(), &page, out);
}

int CaretResolver::caretPage() const {
    int page = 0;
    Rect r{};
    if (!caretRectFor(caret(), &page, &r)) return -1;
    return page;
}

/**
 * 光标是否落在竖排的段里。宿主据此把方向键按物理方向映射 —— 竖排的行内轴是竖直的，
 * 上下走字、左右换列（见 keyToOp）。
 */
bool CaretResolver::caretVertical() const {
    const LineBox* lb = findLineFor(caret(), caret_line_aff_);
    return lb != nullptr && lb->vertical;
}

/**
 * 光标所在段的列序：中日竖排列自右向左（反向），蒙文竖排列自左向右。
 * 取自段自己的书写模式，不从行盒推 —— 行盒上的 sideways 只是"字形要不要转"，
 * 与列序是两件事，拿它代用会在将来加"侧倒但列序反向"的组合时静默出错。
 */
bool CaretResolver::caretBlockReversed() const {
    const int para = caretPara();
    if (para < 0 || para >= static_cast<int>(view_.blocks->size())) return false;
    return (*view_.blocks)[static_cast<size_t>(para)].wm.isBlockReversed();
}

/**
 * 选区 → 页面坐标矩形（跨行拆、跨页分段）。空选区是空的。
 *
 * 两端各取一次行内位置，中间就是矩形 —— 一行一个，所以跨行自然是多个矩形，跨页也一样
 * （行盒自己带页号）。端点那一侧用真端点的侧别（同一个码点偏移上可能压着好几枚括号，
 * 只有侧别分得开"控件之前 / 内容末尾"），被行或段截断的那一侧用该偏移的最外沿，
 * 免得按端点的侧别把矩形切在行中间。
 *
 * 空行给的是零宽矩形（调用方按宽高为 0 丢掉）：选中一个空段落时它下面那一行确实只有
 * 一个插入点，没有宽度可言。
 */
std::vector<PageRect> CaretResolver::selectionRects() const { return selectionRectsIn(0, 0); }

std::vector<PageRect> CaretResolver::selectionRectsIn(int first_page, int count) const {
    std::vector<PageRect> out;
    if (!view_.hasFlow() || view_.layout->lines.empty() || view_.block_flow->empty()) return out;
    // 空选区的两种来源要分开：真的什么都没有，与"选中了一个空格"。
    //   后者在流里也是一对相等的端点（格内容是空的），但它有整格反白要画 ——
    //   按 sel_.empty() 一律返回空，空格就永远没有一像素高亮。
    const bool cell_sel_span = sel_cell_ && !cell_blocks_.empty();
    if (sel_.empty() && !cell_sel_span) return out;
    // 整格反白按表的几何铺，不按行盒：跨页切开的一行里，内容已经在上一页结束的格
    //   在续排片段上没有行盒 —— 按行盒铺，那半截格子就一条高亮都没有。
    //   一格在每一页上的那一段各出一条矩形，于是跨页的格是两条、单页的格仍是一条。
    if (sel_cell_) {
        const int page_lo = (count > 0) ? first_page : 0;
        const int page_hi =
            (count > 0) ? (first_page + count - 1) : (view_.layout->stats.page_count - 1);
        for (const PlacedTable& pt : view_.layout->placed_tables) {
            for (const LayoutCell& lc : pt.cells) {
                if (!cellCoversPara(lc.first_block)) continue;
                // 表格排版只按横排算（列边界是一条条 x）：竖排的格不在这里换轴。
                for (int pg = page_lo; pg <= page_hi; ++pg) {
                    Rect r;
                    if (!pt.cellBoxOnPage(lc, pg, &r.x, &r.y, &r.w, &r.h)) continue;
                    out.push_back(PageRect{pg, r});
                }
            }
        }
    }

    // 渲染读的是"吸附之后"的那一对（整格时它是格边界），与编辑同源 ——
    //   两份读数各算一次，就会出现"反白到这里、删到那里"。
    const Range vis = selection();
    const FlowPos lo = vis.begin();
    const FlowPos hi = vis.end();
    // 段范围按块区间取，不按流包络：空格的包络是个点，按它算出来只有一个段，
    //   而矩形选中几行时包络里夹着没被选中的格（多算的那些由循环自己筛掉）。
    int pa_lo = paraForFlow(lo);
    int pa_hi = paraForFlow(hi);
    if (cell_sel_span) {
        pa_lo = cell_blocks_.front().first;
        pa_hi = cell_blocks_.back().first + cell_blocks_.back().count - 1;
    }
    const int n_para = static_cast<int>(view_.block_flow->size());

    // 文档末那一枚段落标记：它在流里占一位，却不属于任何一块的内容（块跨度只数内容，
    //   标记夹在两块之间），按流区间求交永远选不到它 —— 末段就成了"选中了、一条高亮
    //   都没有"。正向扩选到末位时它算被选中，与字处理软件同一条；反向选到末位时锚点
    //   停在标记之前，不补。
    const LineBox* tail_line = view_.layout->lines.empty() ? nullptr : &view_.layout->lines.back();
    bool tail_mark = false;
    if (tail_line != nullptr && !sel_.empty() && sel_.anchor < sel_.focus) {
        const size_t last_block = view_.block_flow->size() - 1;
        const FlowPos doc_end =
            (*view_.block_flow)[last_block] + (*view_.block_flow_span)[last_block];
        tail_mark = hi >= doc_end;
    }

    // 要看的行：限页时是窗口内各页的行，不限页时是全篇。
    //   按页取行读的是 LayoutResult::page_lines —— 行数组按流顺序排，表格行跨页之后
    //   它不再按页单调，按页号二分收窄会把窗口里另一页的行整段跳过（选区少画一截、
    //   整格反白漏一整列）。
    const int n_lines = static_cast<int>(view_.layout->lines.size());
    // 选区的粒度（见 selectionIsCell）：整格 / 文字。整格只在一次"点格内边距带"之后成立。
    const bool cell_sel = sel_cell_;
    std::vector<int> order;
    if (count > 0 && n_lines > 0) {
        for (int p = first_page; p < first_page + count; ++p) {
            const std::vector<int>& list = view_.layout->linesOnPage(p);
            order.insert(order.end(), list.begin(), list.end());
        }
    } else {
        order.resize(static_cast<size_t>(n_lines));
        for (int i = 0; i < n_lines; ++i) order[static_cast<size_t>(i)] = i;
    }

    for (int k : order) {
        const LineBox& lb = view_.layout->lines[static_cast<size_t>(k)];
        const int para = lb.paragraph;
        if (para < pa_lo || para > pa_hi) continue;
        if (para < 0 || para >= n_para) continue;

        // 整格选区只画选中的那几格：矩形在流里是断的，包络会多含两端行里没选中的格。
        //   文字选区不走这条判断（它本来就按行求交）。
        if (cell_sel && !cellCoversPara(para)) continue;

        // 表格里的行归上面那一趟管（整格按表的几何铺）。这一趟只剩正文的行：
        //   Ctrl+A 时正文照样要按流区间高亮，不能因为粒度是"整格"就整篇不画。
        if (cell_sel && lb.cell_box_hi_pt > lb.cell_box_lo_pt) continue;

        // 文字选区：本段落在选区里的那一段（整段被选中时就是 [0, 本段占的位数]）。
        const FlowPos base = (*view_.block_flow)[static_cast<size_t>(para)];
        const FlowPos seg_lo = std::max(lo, base);
        const FlowPos seg_hi =
            std::min(hi, base + (*view_.block_flow_span)[static_cast<size_t>(para)]);
        if (seg_hi <= seg_lo) {
            // 空段（只有一个段落标记、块跨度是 0）：标记本身落在选区里就要有一条高亮 ——
            //   否则"选中了空行"在屏幕上一个像素都没有。跨选到空行是最常见的那条路。
            //   文档末那一段的标记位在内容区间之外（见上面的 tail_mark），正向选到末位
            //   时由 tail_mark 补上这一条。
            if (lo <= base && (base < hi || (tail_mark && &lb == tail_line))) {
                Rect m;
                const float w = lb.size_pt * 0.35f;
                if (!lb.vertical) {
                    m.x = lb.inline_origin_pt;
                    m.w = w;
                    m.y = lb.block_lo_pt;
                    m.h = lb.block_hi_pt - lb.block_lo_pt;
                } else {
                    m.x = lb.block_lo_pt;
                    m.w = lb.block_hi_pt - lb.block_lo_pt;
                    m.y = lb.inline_origin_pt;
                    m.h = w;
                }
                out.push_back(PageRect{lb.page, m});
            }
            continue;
        }

        const int off_lo = cpOffsetInBlock(para, static_cast<int>(seg_lo - base));
        const int off_hi = cpOffsetInBlock(para, static_cast<int>(seg_hi - base));
        if (off_lo < 0 || off_hi < 0) continue;
        const uint32_t sel_lo = static_cast<uint32_t>(off_lo);
        const uint32_t sel_hi = static_cast<uint32_t>(off_hi);

        // 与这一行的段内码点区间 [first_cp, last_cp] 求交。
        const uint32_t l_lo = std::max(sel_lo, lb.first_cp);
        const uint32_t l_hi = std::min(sel_hi, lb.last_cp);
        if (l_hi < l_lo) continue;

        // 左端：真端点（选区确实起在这一处）用它的侧别；被本行截断时用"该偏移之前"。
        int side_lo = 0;
        if (l_lo == sel_lo && seg_lo == lo) side_lo = sideForFlow(lo);
        // 右端：真端点用它的侧别；被本行/本段截断时用"该偏移所有括号之后"。
        int side_hi = lb.bracketCountAt(l_hi);
        if (l_hi == sel_hi && seg_hi == hi) side_hi = sideForFlow(hi);

        const float a0 = lb.inline_origin_pt + lb.offsetAtSide(l_lo, side_lo);
        float a1 = lb.inline_origin_pt + lb.offsetAtSide(l_hi, side_hi);
        // 末段的段落标记并进这一条：另推一条会与它首尾相接甚至重叠，而半透明的选区
        //   叠两次就是一道深色缝。
        if (tail_mark && &lb == tail_line) a1 += lb.size_pt * 0.35f;
        // 这一行被四周型浮动图挖开时，一条横跨图的矩形会把图也涂上：按可用区间切段，
        //   每段各出一条。单段行（spans 为空）就是端点之间那一条。
        const float rel_lo = std::min(a0, a1) - lb.inline_origin_pt;
        const float rel_hi = std::max(a0, a1) - lb.inline_origin_pt;
        std::vector<std::pair<float, float>> segs;
        if (!(rel_hi > rel_lo)) {
            // 零宽：选中一个空段（或一段的段落标记）时，选的是一枚不占字符的位置，
            //   两端落在同一个偏移上 —— 按几何算就是零宽，而零宽的矩形在导出与合成时
            //   会被当作"没有选区"丢掉，于是"能选中、看不见"。给一个字符宽的窄条。
            //   零宽只是"两端同偏移"的一种结果，不是判据：同一个偏移上压着行内原子
            //   （图片）时，两端的侧别不同，宽度就是原子的盒宽。
            segs.emplace_back(rel_lo, rel_lo + lb.size_pt * 0.35f);
        } else if (lb.spans.empty()) {
            segs.emplace_back(rel_lo, rel_hi);
        } else {
            for (const LineBox::Span& sp : lb.spans) {
                const float lo = std::max(rel_lo, sp.lo_pt);
                const float hi = std::min(rel_hi, sp.hi_pt);
                if (hi > lo) segs.emplace_back(lo, hi);
            }
        }
        for (const std::pair<float, float>& seg : segs) {
            Rect r;
            if (!lb.vertical) {
                r.x = lb.inline_origin_pt + seg.first;
                r.w = seg.second - seg.first;
                r.y = lb.block_lo_pt;
                r.h = lb.block_hi_pt - lb.block_lo_pt;
            } else {
                r.x = lb.block_lo_pt;
                r.y = lb.inline_origin_pt + seg.first;
                r.w = lb.block_hi_pt - lb.block_lo_pt;
                r.h = seg.second - seg.first;
            }
            out.push_back(PageRect{lb.page, r});
        }
    }
    return out;
}

/**
 * 在某一行里按"沿行内轴的页面坐标"（横排 x / 竖排 y）命中插入点。
 *
 * 每个插入点的势力范围是它到相邻插入点的中点；落在某个簇的后半段时，落点是下一个簇的
 * 首码点 —— 这就是"字素不可切分"的正确语义（点 e+U+0301 的右半边，光标跑到它后面，
 * 而不是钻进它内部）。hitTestPt 与上下键共用它，免得两份实现分头长出不同的势力范围。
 */
void CaretResolver::hitTestInLine(const LineBox& lb, float inline_page_pt, uint32_t* cp_out,
                                  int* side_out) const {
    const float rel = inline_page_pt - lb.inline_origin_pt;
    const size_t n = lb.clusters.size();

    uint32_t cp = lb.last_cp;  // 默认：落在最后半个簇之后 → 行末
    // 命中的簇下标（= clusters.size() 表示落在最后一个簇之后）。落点是"这一簇之前"那个
    //   边界，侧别就是同一个偏移上排在它前面的簇数 —— 数格子，不需要认识括号。
    size_t hit_i = n;
    if (rel < 0.0f) {
        cp = n ? lb.clusters.front().cp : lb.first_cp;
        hit_i = n ? 0 : n;
    } else {
        for (size_t i = 0; i < n; ++i) {
            const float a = lb.clusters[i].inline_pt;
            const float b = (i + 1 < n) ? lb.clusters[i + 1].inline_pt : lb.inline_end_pt;
            if (rel < (a + b) * 0.5f) {
                cp = lb.clusters[i].cp;
                hit_i = i;
                break;
            }
        }
    }
    const size_t k0 = lb.firstClusterAt(cp);
    // 侧别数的是"位置上跨过了几个槽"，而提示文字那一枚槽占宽度不占位置 ——
    //   按簇下标直接相减会让它算进去：点空字段的提示文字会被判成"跨过两个槽"，
    //   于是落点跑到闭括号右边（差一个括号宽）。
    int side = 0;
    for (size_t i = k0; i < hit_i && i < n; ++i) {
        if (!lb.clusters[i].control_hint) ++side;
    }
    if (cp_out) *cp_out = cp;
    if (side_out) *side_out = side;
}

/**
 * 光标沿行内轴的页面坐标 —— 上下键的列位就是它。找不到行盒返回 0。
 */
float CaretResolver::caretInlineAxisPt() const {
    const TextPos p = caret();
    const LineBox* lb = findLineFor(p, caret_line_aff_);
    if (!lb) return 0.0f;
    const float off = lb->offsetAtSide(static_cast<uint32_t>(p.offset), caretSide());
    return lb->inline_origin_pt + off;
}

bool CaretResolver::moveCaretBy(int dcp, bool extend) {
    if (view_.blocks->empty() || dcp == 0) return false;

    int para = caretPara();
    if (para >= static_cast<int>(view_.blocks->size()))
        para = static_cast<int>(view_.blocks->size()) - 1;
    int rel = caretRel();
    // 本段最后一个流位置 = 段长 + 本段边界枚数（＝ relAt(len, 全部边界)）。
    const auto span_of = [&](int q) {
        const size_t len = (*view_.blocks)[static_cast<size_t>(q)].cp_text.size();
        const BracketCounts bc = bracketCountsAt(q, len);
        return static_cast<int>(len) + bc.before + bc.closes + bc.opens;
    };
    // 落点是不是字素簇边界：括号占位自己就是一枚簇，文本里的字素簇（e + 组合重音）
    //   占好几个码点，所以"一格"要一路挪到簇边界上才算数。
    const auto at_boundary = [&](int q, int r) {
        const size_t off = static_cast<size_t>(cpOffsetInBlock(q, r));
        return snapToCluster(q, off) == off;
    };

    // 一次一格 = 一次跨一个流位置。括号也在流里（各占一位），所以"一个字符一个字符地移动"
    //   不需要认识括号 —— 就是在流位置上加减一。按码点偏移走的话，括号那一格被整段跳过，
    //   "字与开括号之间""两个相邻控件之间"这些位置方向键就到不了。
    //   停步条件只有一个：前面没有下一格了（段首 / 段尾）。
    const int steps = std::abs(dcp);
    for (int k = 0; k < steps; ++k) {
        bool moved = false;
        for (;;) {
            if (dcp < 0) {
                if (rel > 0) {
                    --rel;
                } else if (para > 0) {
                    --para;
                    rel = span_of(para);
                } else {
                    break;
                }
            } else {
                if (rel < span_of(para)) {
                    ++rel;
                } else if (para + 1 < static_cast<int>(view_.blocks->size())) {
                    ++para;
                    rel = 0;
                } else {
                    break;
                }
            }
            moved = true;
            if (at_boundary(para, rel)) break;
        }
        if (!moved) break;
    }

    if (para == caretPara() && rel == caretRel()) return false;
    setCaretRel(para, rel, !extend);
    // 跨字段移动在聚焦档下会换占位集合、整行重排 —— 光标在 rel 上，重排不改变它。
    return true;
}

bool CaretResolver::moveCaretToLineStart(bool extend) {
    const LineBox* lb = findLineFor(caret(), caret_line_aff_);
    if (!lb) return false;
    // 行首就是行首，不再往字段边界上收：光标位置不受控件约束（见 setCaret）。
    const TextPos p{lb->paragraph, lb->first_cp};
    if (p == caret()) return false;
    placeCaret(p.para, p.offset, -1, extend);
    return true;
}

bool CaretResolver::moveCaretToLineEnd(bool extend) {
    const LineBox* lb = findLineFor(caret(), caret_line_aff_);
    if (!lb) return false;
    const TextPos p{lb->paragraph, lb->last_cp};
    if (p == caret()) return false;
    placeCaret(p.para, p.offset, -1, extend);
    return true;
}

/**
 * 到文档首 / 文档末（Ctrl+Home / Ctrl+End）。
 *
 * 落点用流位置给（块起点 / 块起点 + 本块占的位数），不走文本坐标 —— 段末那个位置在
 * 文本坐标里与"下一段段首"是同一个偏移，翻回来会跑到下一段去。
 */
bool CaretResolver::moveCaretToDocStart(bool extend) {
    if (view_.block_flow->empty()) return false;
    if (sel_.focus == (*view_.block_flow)[0]) return false;
    setCaretRel(0, 0, !extend);
    return true;
}

bool CaretResolver::moveCaretToDocEnd(bool extend) {
    if (view_.block_flow->empty()) return false;
    const size_t last = view_.block_flow->size() - 1;
    if (sel_.focus == (*view_.block_flow)[last] + (*view_.block_flow_span)[last]) return false;
    setCaretRel(static_cast<int>(last), (*view_.block_flow_span)[last], !extend);
    return true;
}

/**
 * 上下键：沿块轴走到相邻行，列位沿行内轴保持。
 *
 * 实现是"当前行盒在全局行数组里的下标 ±1 → 在目标行上按列位做一次行内命中"，不新增
 * 排版接口 —— 行盒数组本来就是按块轴排好的。到文档首/末时停在原地（返回假），但列位
 * 留着，于是"按到头上、再按回来"落的是同一个列位。
 */
bool CaretResolver::moveCaretVertical(int dir, bool extend) {
    if (view_.layout->lines.empty() || dir == 0) return false;
    const int n = static_cast<int>(view_.layout->lines.size());
    const LineBox* cur_lb = findLineFor(caret(), caret_line_aff_);
    if (!cur_lb) return false;
    int cur_i = -1;
    for (int i = 0; i < n; ++i) {
        if (&view_.layout->lines[static_cast<size_t>(i)] == cur_lb) {
            cur_i = i;
            break;
        }
    }
    if (cur_i < 0) return false;

    // 列位：第一次按取自光标当前的行内坐标，之后连续按沿用（这就是"跨短行不丢列"）。
    if (!vertical_goal_valid_) {
        vertical_goal_pt_ = caretInlineAxisPt();
        vertical_goal_valid_ = true;
    }
    const float goal = vertical_goal_pt_;

    const int dst_i = cur_i + (dir < 0 ? -1 : 1);
    if (dst_i < 0 || dst_i >= n) return false;  // 到头了：不动，列位留着

    const LineBox& dst = view_.layout->lines[static_cast<size_t>(dst_i)];
    uint32_t cp = 0;
    int side = 0;
    hitTestInLine(dst, goal, &cp, &side);
    const int rel = relAt(dst.paragraph, cp, side);
    if (dst.paragraph == caretPara() && rel == caretRel()) return false;

    setCaretRel(dst.paragraph, rel, !extend);
    // collapse 那一路会把列位清掉（它就是"别的定位"），这里再摆回去 —— 连续按要同一列。
    vertical_goal_pt_ = goal;
    vertical_goal_valid_ = true;
    return true;
}
}  // namespace heditor
