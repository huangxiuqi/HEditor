/**
 * 引擎门面与编辑会话。
 *
 * 这里放三样东西：生命周期与文档装载（init / setConfig / setDocument）、
 * 把排版结果暴露给宿主的查询（帧、光标、命中、控件）、以及编辑入口（插入删除、分段合并、
 * 缩进）。
 * 排版调度在 engine_layout.cpp、呈现与光栅增量在 engine_paint.cpp、审计判据在
 * engine_audit.cpp、诊断输出在 engine_diag.cpp —— 拆开是因为它们的变更理由不同。
 *
 * 编辑入口这一段将来会搬到命令层（见 docs/正式化架构与迁移路线.md 阶段 2），
 * 所以现在不为它做进一步的职责拆分。
 */

#include "core/engine/engine.h"

#include "core/text/writing_mode.h"
#include "core/utils/hash.h"
#include "core/utils/time.h"
#include "core/utils/units.h"

#include <hb.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

#include "core/text/shaper.h"

namespace heditor {

namespace {

/**
 * 业务类型字符串 → 旧表示的三种控件种类。
 *
 * 两者层次不同：controlType 是开放的（宿主可以注册一种新的），ControlKind 是封闭的三档 ——
 * 只够表达"值怎么校验、怎么显示"这一层，界面上的差别（复选、下拉、日期浮层）要等控件类型
 * 注册表落地。认不出来的类型回落到文本：宁可当一个普通文本框，也不要让一个没登记的类型
 * 变成一个"打不进去"的字段。空串同理（没写类型的控件就是文本框）。
 */
/**
 * 这棵子树里有没有段落标记。
 *
 * 增量编译要判"这一段的内容区间是不是干净的"：区间里若嵌着段落标记，那些标记本身
 * 就切出别的段，这一段的产物于是依赖区间之外的段边界 —— 那种情况退回全量。
 */
bool subtreeHasParagraphMark(const Node* n) {
    if (isParagraphMark(n->kind)) return true;
    for (const Node* c : n->children) {
        if (subtreeHasParagraphMark(c)) return true;
    }
    return false;
}

ControlKind controlKindFromBusinessType(const std::string& type) {
    if (type == "number") return ControlKind::NumberField;
    if (type == "date") return ControlKind::DateField;
    return ControlKind::TextField;
}

}  // namespace

/**
 * 生命周期：字体加载与引擎初始化。
 */

bool EditorEngine::init(const std::vector<std::string>& fontPaths) {
    for (const auto& p : fontPaths) {
        if (fonts_.addFace(p) < 0) return false;
    }
    if (fonts_.faceCount() <= 0) return false;
    base_valid_ = false;

    // 帧缓冲与底图共享同一份字形缓存：同一个字形在两块图上光栅出来的
    //   mask 一模一样，各存一份纯属浪费。而这份缓存正对着绘制成本的大头 ——
    //   实测 4 页文档 @1.5× 的 paint 里有 91.5% 花在字形光栅上（121 µs/字形）。
    if (!glyph_cache_) glyph_cache_ = std::make_shared<GlyphCache>();
    bitmap_.setGlyphCache(glyph_cache_);
    base_.setGlyphCache(glyph_cache_);
    return true;
}

void EditorEngine::disableGlyphCache() {
    bitmap_.setGlyphCache(nullptr);
    base_.setGlyphCache(nullptr);
    glyph_cache_.reset();
    base_valid_ = false;  // 底图要按"无缓存"的路径重建一遍
}

void EditorEngine::setConfig(const EngineConfig& cfg) {
    // 改"显示括号的范围"会改变占位（括号进文档流）→ 必须重排。
    //   不重排的话括号会按新档画、正文却按旧档排 —— 画的用的还是旧的簇坐标，
    //   表象是括号盖住字/槽里空一条，而且看不出来是哪一边错了。
    //   （其它配置项不动排版结构：zoom / 页度量只影响几何，底图作废就够了 ——
    //     宿主改那两项之后本来就会立刻重排重绘。）
    const bool plan_changed = (cfg.form_brackets != cfg_.form_brackets);
    // 页度量改了整篇的断行位置都会变，没有可定位的影响面 → 整篇标脏。
    const bool page_changed = (cfg.page != cfg_.page);
    cfg_ = cfg;
    if (cfg_.zoom < 0.25f) cfg_.zoom = 0.25f;
    if (cfg_.zoom > 8.0f) cfg_.zoom = 8.0f;
    if (cfg_.caret_width_pt < 0.4f) cfg_.caret_width_pt = 0.4f;
    base_valid_ = false;  // 缩放/页度量变了，底图作废
    caret_rect_px_prev_ = Rect{};
    if (plan_changed) markLayoutDirtyForBrackets();
    if (page_changed) markAllLayoutDirty();
    if (plan_changed || page_changed) relayout();
}

/**
 * 文档装载：模型树那条路（第十四节的接缝）。
 */
void EditorEngine::setDocument(const NodeTree& tree, const StyleSheet& sheet) {
    tree_.assignFrom(tree);
    sheet_ = sheet;
    loaded_ = true;
    // 撤销栈在建栈时就绑定了 tree_ 的地址，而 assignFrom 只换内容、不换对象 —— 所以
    //   换文档时旧栈必须整个丢掉：它记的步骤属于上一份文档，坐标在新文档上毫无意义。
    undo_ = std::make_unique<UndoStack>(tree_);
    setCaretByText(TextPos{0, 0});
    caret_rect_px_prev_ = Rect{};
    base_valid_ = false;
    rebuildFromTree();
    // 整份文档换了：段数与内容都可能全变，没有可复用的前缀。
    markAllLayoutDirty();
    // 控件集合随之变了 → 帧上的标记与上一帧不是一回事，下一次呈现整帧重贴。
    frame_needs_full_refresh_ = true;
}

FlowVisibility EditorEngine::flowVisibility() const {
    // 隐藏控件的可见性口径与编译同一处取值：文档模式是设计态（隐藏的照常参与流，
    //   否则作者没法把它选中改回来），表单与只读是呈现态（整棵摘除、不占流位置）。
    return (mode_ == EditMode::Document) ? FlowVisibility::Design : FlowVisibility::Display;
}

void EditorEngine::rebuildFromTree() {
    const FlowVisibility vis = flowVisibility();
    const auto t_compile0 = Clock::now();
    CompileResult res = compileDocument(tree_, vis, sheet_);
    blocks_full_ = std::move(res.blocks);
    block_src_ = std::move(res.sources);
    edit_stats_.rebuild_compile_ms = msSince(t_compile0);
    finishRebuildFromTree(vis);
}

bool EditorEngine::rebuildParagraphFromTree(int para) {
    // 条件先判完再动手：任何一条不满足就原样返回 false，由调用方退回全量 ——
    //   半途改一半再失败会留下"这一段按新树、别处按旧树"的产物，那种错最难查。
    if (para < 0) return false;
    if (para >= static_cast<int>(block_src_.size())) return false;
    if (para >= static_cast<int>(blocks_full_.size())) return false;
    const Node* mark = tree_.find(block_src_[static_cast<size_t>(para)].mark);
    if (mark == nullptr || !isParagraphMark(mark->kind) || mark->parent == nullptr) return false;
    const Node* container = mark->parent;
    int mark_idx = -1;
    for (size_t i = 0; i < container->children.size(); ++i) {
        if (container->children[i]->id == mark->id) {
            mark_idx = static_cast<int>(i);
            break;
        }
    }
    if (mark_idx < 0) return false;

    // 内容区间 = 上一枚段落标记之后 到 本段标记。区间里出现节边界或嵌套的段落标记时，
    //   这一段的产物依赖区间之外的状态（段数、分节强制分页），退回全量。
    int begin = 0;
    for (int i = mark_idx - 1; i >= 0; --i) {
        const Node* sib = container->children[static_cast<size_t>(i)];
        if (isParagraphMark(sib->kind)) {
            begin = i + 1;
            break;
        }
        if (sib->kind == NodeKind::Section || subtreeHasParagraphMark(sib)) return false;
    }
    // 分节强制分页是"进入第二节及以后"时才落在那一段上的一位，区间编译看不到上一节。
    if (begin == 0 && container->kind == NodeKind::Section &&
        container->id != tree_.firstSectionId()) {
        return false;
    }

    // 这一段的内容起点必须与它的结束标记同在一个容器里，且落在这段子节点区间内。
    //
    //   为什么非判不可：在控件内容里回车时，段落标记就落在控件内部（跨段控件是正常形态），
    //   而这一段的内容其实起自控件之外。只编"控件内那一截子节点"，产物就成了半个段落 ——
    //   前半段的字与控件自己的边界一起丢掉；更糟的是 block_src_ / block_flow_ 会被这半个
    //   产物覆盖，之后每一次编辑都按错的坐标算、控件集合也空掉。界面上看到的就是退格删一个
    //   字、前面一串字连同所有控件一起不见。这种形状退回全量编译。
    //   表格单元格里的段落不受影响：它的起点就在同一个单元格内，下标落在 [begin, mark_idx]。
    int first_idx = -1;
    const Node* fn = tree_.find(block_src_[static_cast<size_t>(para)].first);
    while (fn != nullptr && fn->parent != container) fn = fn->parent;
    if (fn != nullptr) {
        for (size_t i = 0; i < container->children.size(); ++i) {
            if (container->children[i] == fn) {
                first_idx = static_cast<int>(i);
                break;
            }
        }
    }
    if (first_idx < begin || first_idx > mark_idx) return false;

    const FlowVisibility vis = flowVisibility();
    const auto t_compile0 = Clock::now();
    CompileResult part = compileChildRange(tree_, container->id, begin, mark_idx, vis, sheet_);
    if (part.blocks.size() != 1 || part.sources.size() != 1) return false;
    blocks_full_[static_cast<size_t>(para)] = std::move(part.blocks[0]);
    block_src_[static_cast<size_t>(para)] = part.sources[0];
    edit_stats_.rebuild_compile_ms = msSince(t_compile0);
    finishRebuildFromTree(vis);
    return true;
}

void EditorEngine::finishRebuildFromTree(FlowVisibility vis) {
    const auto t_index0 = Clock::now();
    // 流索引与块起点一起重建：两者是同一份文档的两种说法，分两次建就会出现
    //   "块按新树、流位置按旧树"的错配，而那种错配只表现为光标落偏一个字，很难查。
    flow_.emplace(tree_, vis);
    edit_stats_.rebuild_flow_ms = msSince(t_index0);
    const auto t_pos0 = Clock::now();

    const size_t n = blocks_full_.size();
    block_flow_.assign(n, 0);
    block_flow_span_.assign(n, 0);
    // 段与段之间由段落标记分隔，所以段的流区间只该由标记位置推：段 i 的内容是
    //   [段 i 起点, 标记位置)，段 i+1 的起点 = 标记之后那一位。标记位置是权威
    //   （FlowIndex 给的），递推出来的区间于是与流严丝合缝。
    //
    // 不能拿"首个内容元素所属节点的区间起点"当段起点：段落被控件从中间切开时
    //   （在控件内容里回车），那个节点是控件本身，而控件的区间起点在上一段里 ——
    //   算出来偏前若干位，于是 setCaretAtFlow 拿到一个偏小的段起点、rel 偏大、
    //   侧别落到别的控件上。表象就是"回车之后光标跑进了下一个控件"。
    FlowPos start = 0;
    // 段 0 的起点不能硬编 0：它前面可能有节边界（节有起止两位），所以首段第一个
    //   内容元素的区间起点才是权威。段 0 的 first 一定落在本段内（首段没有"从中间
    //   开始"的情形），所以它的区间起点就是段起点。
    if (n > 0 && !block_src_.empty()) {
        int first = -1;
        if (flow_->intervalOf(block_src_[0].first, &first, nullptr) && first >= 0) start = first;
    }
    for (size_t i = 0; i < n; ++i) {
        int mark_first = -1;
        int mark_end = -1;
        if (i < block_src_.size() &&
            flow_->intervalOf(block_src_[i].mark, &mark_first, &mark_end) && mark_first >= start) {
            block_flow_[i] = start;
            block_flow_span_[i] = mark_first - start;
            start = mark_first + 1;
            continue;
        }
        // 标记查不到（不该发生）：退回推定值，至少保证区间单调不重叠。
        block_flow_[i] = start;
        block_flow_span_[i] =
            static_cast<int>(blocks_full_[i].cp_text.size() + blocks_full_[i].objects.size());
        start = block_flow_[i] + block_flow_span_[i] + 1;
    }

    edit_stats_.rebuild_flowpos_ms = msSince(t_pos0);
    const auto t_copy0 = Clock::now();
    blocks_ = blocks_full_;
    edit_stats_.rebuild_copy_ms = msSince(t_copy0);
    const auto t_ctl0 = Clock::now();
    exportControlsFromBlocks();
    edit_stats_.rebuild_controls_ms = msSince(t_ctl0);
}

/**
 * 边界括号的统计。侧别与流位置都从它推出来 —— 两者是同一件事的两种说法：
 * 流位置 = 码点偏移 + 排在它前面的边界数 + 侧别。
 */
const std::vector<InlineObject>& EditorEngine::objectsFor(int para) const {
    static const std::vector<InlineObject> kNone;
    if (para < 0 || para >= static_cast<int>(blocks_full_.size())) return kNone;
    return blocks_full_[static_cast<size_t>(para)].objects;
}

EditorEngine::BracketCounts EditorEngine::bracketCountsAt(int para, size_t cp) const {
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

int EditorEngine::relAt(int para, size_t cp, int side) const {
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
bool EditorEngine::textSpanAt(int para, size_t a, size_t b, int* from, int* to) const {
    if (!flow_.has_value() || a > b) return false;
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
int EditorEngine::defaultSideAt(int para, size_t cp) const {
    if (!controls_.empty()) {
        const int ci = controls_.innermostAt(TextPos{para, cp}, /*include_end=*/true);
        if (ci >= 0) return sideFromHint(para, cp, ci);
    }
    return bracketCountsAt(para, cp).closes;
}

int EditorEngine::sideFromHint(int para, size_t cp, int hint) const {
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

int EditorEngine::hintFromRel(int para, int rel) const {
    const std::vector<InlineObject>& objs = objectsFor(para);
    // 控件的内容区间是 (起始边界锚点, 结束边界锚点]：起始边界属于"控件之前"、
    //   结束边界属于"内容末尾"（见 flow.h 开头那段）。取起始锚点最大的那个 = 最内层。
    int best = LineBox::kInlineOffsetOuter;
    int best_start = -1;
    for (size_t j = 0; j < objs.size(); ++j) {
        if (objs[j].kind != InlineObjectKind::ControlEnd) continue;
        const int end = objs[j].cp + static_cast<int>(j);
        if (rel > end) continue;
        for (size_t i = 0; i < j; ++i) {
            if (objs[i].kind != InlineObjectKind::ControlStart) continue;
            if (objs[i].owner != objs[j].owner) continue;
            const int start = objs[i].cp + static_cast<int>(i);
            if (start < rel && start > best_start) {
                best_start = start;
                best = controlIndexOfOwner(static_cast<int>(objs[j].owner));
            }
            break;
        }
    }
    if (best != LineBox::kInlineOffsetOuter) return best;
    // 段内找不到配对的开括号：这个偏移上只有一枚"从上一段跨进来"的结束边界。
    //   见 hintFromCrossSegment —— 不兜的话，界面会把当前字段显示成紧挨着的下一个控件。
    return hintFromCrossSegment(para, rel);
}

/**
 * 跨段控件的归属补判：段内对象表里只有本段那一半边界，所以控件从上一段跨进来时
 * 找不到配对的开括号，hintFromRel 会判成"在所有控件之外"。这里用控件集合的跨段区间补上：
 * 把段内相对流位置换成全局流位置，与每个控件的内容区间 (起始边界, 结束边界] 比，
 * 取起点最晚的那个。
 *
 * 为什么要兜：段落被控件从中间切开之后（在控件内容里回车），光标落在"这个控件在本段的
 * 那一半"里，而它在段内看起来只是孤零零一枚结束边界 —— 不兜的话，"当前字段"会显示成
 * 紧挨着的下一个控件，界面显示的就成了"回车之后光标跑进第三个控件"。
 */
int EditorEngine::hintFromCrossSegment(int para, int rel) const {
    if (controls_.empty() || !flow_.has_value()) return LineBox::kInlineOffsetOuter;
    if (para < 0 || para >= static_cast<int>(block_flow_.size()))
        return LineBox::kInlineOffsetOuter;
    const FlowPos f = block_flow_[static_cast<size_t>(para)] + rel;
    int pick = -1;
    int pick_start = -1;
    for (int i = 0; i < controls_.size(); ++i) {
        const Control& c = controls_.at(i);
        // 两枚边界各自"自己那一格"的流位置：开括号取内容起点（括号之后），闭括号取内容末尾。
        const FlowPos cs = flowPosAtSide(c.range.start.para, c.range.start.offset,
                                         sideFromHint(c.range.start.para, c.range.start.offset, i));
        const FlowPos ce = flowPosAtSide(c.range.end.para, c.range.end.offset,
                                         sideFromHint(c.range.end.para, c.range.end.offset, i));
        if (cs < 0 || ce < 0) continue;
        // 内容区间是 (起始边界, 结束边界]：起始边界属于"控件之前"，结束边界属于内容末尾。
        if (f <= cs || f > ce) continue;
        if (cs > pick_start) {
            pick_start = cs;
            pick = i;
        }
    }
    return pick >= 0 ? pick : LineBox::kInlineOffsetOuter;
}

int EditorEngine::caretSide() const {
    // 派生：rel 减去"这个偏移在流里占的起点"就是跨过的边界枚数。没有"存着的侧别"
    //   可以失配，所以既不需要失效标记，也不需要拿边界枚数去卡它。
    const TextPos p = caret();
    const BracketCounts bc = bracketCountsAt(p.para, p.offset);
    const int side = caret_rel_ - static_cast<int>(p.offset) - bc.before;
    return side < 0 ? 0 : side;
}

int EditorEngine::caretFieldHint() const {
    if (blocks_.empty()) return -1;
    return hintFromRel(caret_para_, caret_rel_);
}

/**
 * 某个位置（不是光标）的侧别：按"最内层含端点"归一次，归不到就是"所有控件之外"。
 * 删除区间的两个端点、诊断打印这类没有侧别信息的入口走它。
 */
int EditorEngine::sideForPos(TextPos p) const {
    if (p == caret()) return caretSide();
    const BracketCounts bc = bracketCountsAt(p.para, p.offset);
    if (controls_.empty()) return bc.closes;
    const int ci = controls_.innermostAt(p, /*include_end=*/true);
    if (ci < 0) return bc.closes;
    return sideFromHint(p.para, p.offset, ci);
}

FlowPos EditorEngine::flowPosAtSide(int para, size_t cp_offset, int side) const {
    if (!flow_.has_value() || para < 0 || para >= static_cast<int>(block_flow_.size())) return -1;
    if (cp_offset > blocks_full_[static_cast<size_t>(para)].cp_text.size())
        cp_offset = blocks_full_[static_cast<size_t>(para)].cp_text.size();
    return block_flow_[static_cast<size_t>(para)] + relAt(para, cp_offset, side);
}

FlowPos EditorEngine::flowPosAt(int para, size_t cp_offset, int owner_hint) const {
    if (!flow_.has_value() || para < 0 || para >= static_cast<int>(block_flow_.size())) return -1;
    if (cp_offset > blocks_full_[static_cast<size_t>(para)].cp_text.size())
        cp_offset = blocks_full_[static_cast<size_t>(para)].cp_text.size();
    return flowPosAtSide(para, cp_offset, sideFromHint(para, cp_offset, owner_hint));
}

int EditorEngine::cpOffsetInBlock(int para, int rel) const {
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

bool EditorEngine::flowToTextPos(FlowPos flow, TextPos* out) const {
    if (!flow_.has_value() || block_flow_.empty()) return false;
    if (flow < 0) flow = 0;
    const int n = static_cast<int>(block_flow_.size());
    for (int i = 0; i < n; ++i) {
        const FlowPos base = block_flow_[static_cast<size_t>(i)];
        if (flow < base) {
            // 落在本块之前：那是段落标记或节边界，模型里没有"段间"的位置，归到本段段首。
            if (out != nullptr) *out = TextPos{i, 0};
            return true;
        }
        const int rel = flow - base;
        if (rel > block_flow_span_[static_cast<size_t>(i)]) continue;
        if (out != nullptr) {
            *out = TextPos{i, static_cast<size_t>(cpOffsetInBlock(i, rel))};
        }
        return true;
    }
    const size_t last = block_flow_.size() - 1;
    if (out != nullptr) *out = TextPos{static_cast<int>(last), blocks_full_[last].cp_text.size()};
    return true;
}

void EditorEngine::exportControlsFromBlocks() {
    controls_.clear();
    owner_index_.clear();
    control_node_ids_.clear();

    struct Boundary {
        TextPos start{};
        TextPos end{};
        bool has_start = false;
        bool has_end = false;
        bool read_only = false;
    };
    std::unordered_map<int, Boundary> acc;
    std::vector<int> order;  // owner 首见顺序 = 流顺序（对象按锚点单调不减地排在产物里）
    for (size_t i = 0; i < blocks_.size(); ++i) {
        const int para = static_cast<int>(i);
        for (const InlineObject& o : blocks_[i].objects) {
            auto it = acc.find(o.owner);
            if (it == acc.end()) {
                order.push_back(o.owner);
                it = acc.emplace(o.owner, Boundary{}).first;
            }
            Boundary& b = it->second;
            const TextPos at{para, static_cast<size_t>(o.cp)};
            if (o.kind == InlineObjectKind::ControlStart) {
                b.start = at;
                b.has_start = true;
            } else {
                b.end = at;
                b.has_end = true;
            }
            // 两枚边界的只读位同源（编译一次读定），取或只是让"配不成对"的那一半也能留下痕迹。
            b.read_only = b.read_only || o.read_only;
        }
    }

    for (int owner : order) {
        const Boundary& b = acc[owner];
        // 两枚边界必须成对：编译产的是一个开一个闭。少了任何一枚都说明这棵子树有一半
        //   没进产物（例如被一个只记诊断的元素包着），那样的区间只会误导策略 ——
        //   整对丢掉，宁可少一个控件，也不要一个只有半截的区间。
        if (!b.has_start || !b.has_end) continue;
        Control c;
        c.range.start = b.start;
        c.range.end = b.end;
        c.read_only = b.read_only;
        // 业务标识与种类是节点上的数据，不在边界里 —— 从树取。
        //   标签刻意不填：第七节里标签是内容（它自己是文本节点），渲染时从内容取；
        //   旧表示的 label 是调用方自填的显示名，编译产物里没有对应物。
        const Node* n = tree_.find(static_cast<NodeId>(owner));
        if (n != nullptr) {
            c.id = n->control.key;
            c.kind = controlKindFromBusinessType(n->control.controlType);
        }
        const int index = controls_.add(c);
        owner_index_[owner] = index;
        control_node_ids_.push_back(static_cast<NodeId>(owner));
    }
    controls_.normalize();
}

bool EditorEngine::commitOnTree(Transaction tx, int dirty_para, bool para_count_changed) {
    if (!loaded_) return false;  // 没装载过文档：没有树可依，也就没有可提交的事务
    const StepResult r = undo_->commit(std::move(tx));
    if (!r.ok) return false;

    // 树变了：块流、流索引、块起点、来源身份、控件集合一次重建。分两次建就会留下
    //   "块按新树、位置按旧树"的错配，而那种错配只表现为光标落偏一个字。
    //
    // 段数没变时只重编译被改的那一段（增量）：整树编译是 O(文档)，3000 段实测约 1.8ms，
    //   而那是每次按键都要付的。段数变了就没有"第 k 段还是同一段"这回事，走全量。
    if (para_count_changed) {
        rebuildFromTree();
    } else if (!rebuildParagraphFromTree(dirty_para)) {
        rebuildFromTree();
    }
    markLayoutDirty(dirty_para);
    return true;
}

bool EditorEngine::eraseOnTree(TextPos a, TextPos b, const char* label) {
    if (a.para != b.para) return false;
    // 删除区间取"这段文本码点自己占的那一段"（括号不在里面，见 textSpanAt）。
    int rel_from = 0;
    int rel_to = 0;
    if (!textSpanAt(a.para, a.offset, b.offset, &rel_from, &rel_to)) return false;
    const FlowPos base = block_flow_[static_cast<size_t>(a.para)];
    const FlowPos from = base + rel_from;
    const FlowPos to = base + rel_to;
    if (from < 0 || to < 0 || from >= to) return false;
    // 按文本节点切分着删（见 makeDeleteTextSteps）：区间级那套"端点向外扩到结构单元边界"
    //   的规则在这里正好帮倒忙 —— 删控件里的一个字会被理解成删掉整个控件的内容。
    std::vector<Step> steps = makeDeleteTextSteps(tree_, from, to);
    if (steps.empty()) return false;
    Transaction tx;
    tx.label = label;
    tx.selection_before = Range{from, from};
    tx.selection_after = Range{from, from};
    tx.steps = std::move(steps);
    if (!commitOnTree(std::move(tx), a.para, false)) return false;
    // 落点 = 删除区间的起点，也就是被删掉那段文本所在的位置。这个流位置本来就算出来了，
    //   直接摆过去即可 —— 只留一个码点偏移、再由"最内层含端点"猜回侧别的话，
    //   删完一个字光标会落到相邻括号的另一侧（"跳了一格"）。
    setCaretAtFlow(from);
    return true;
}

CharFormat EditorEngine::marksAtFlow(FlowPos at, std::string* style_ref) const {
    if (style_ref != nullptr) style_ref->clear();
    if (!flow_.has_value()) return CharFormat{};
    const Pos p = flow_->posAt(at);
    const Node* n = tree_.find(p.container);
    if (n == nullptr) return CharFormat{};
    if (isText(n->kind)) {
        if (style_ref != nullptr) *style_ref = n->styleRef;
        return n->marks;
    }
    // 落在容器里（含"文本之前"的空位置）：看前一个孩子是不是文本 —— 打字接在已有文字
    //   后面是最常见的一种，沿用它的标记才不会在粗体字后面敲出一个不粗的字。
    if (isContainer(n->kind) && p.offset > 0) {
        const Node* prev = n->children[static_cast<size_t>(p.offset) - 1];
        if (prev != nullptr && isText(prev->kind)) {
            if (style_ref != nullptr) *style_ref = prev->styleRef;
            return prev->marks;
        }
    }
    return CharFormat{};
}

bool EditorEngine::undo() {
    if (!loaded_) return false;  // 没装载过文档：没有事务栈
    Range restore;
    const StepResult r = undo_->undo(&restore);
    if (!r.ok) return false;
    rebuildFromTree();
    // 撤销会改到哪一段是不可定位的（事务可能跨段），所以整篇重排 —— 与"编辑只影响一段"
    //   不同，撤销的单位是用户按下去的那一下，不是文档位置。
    markAllLayoutDirty();
    setCaretAtFlow(restore.focus);
    frame_needs_full_refresh_ = true;
    return true;
}

bool EditorEngine::redo() {
    if (!loaded_) return false;  // 同上
    Range restore;
    const StepResult r = undo_->redo(&restore);
    if (!r.ok) return false;
    rebuildFromTree();
    markAllLayoutDirty();
    setCaretAtFlow(restore.focus);
    frame_needs_full_refresh_ = true;
    return true;
}

bool EditorEngine::undoAndRender() {
    if (!undo()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::redoAndRender() {
    if (!redo()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

size_t EditorEngine::frameBytes() const { return bitmap_.byteSize(); }

size_t EditorEngine::documentCodepoints() const {
    size_t n = 0;
    for (const BlockSpec& b : blocks_) n += b.cp_text.size();
    return n;
}

/**
 * 光标：位置设置、字素簇吸附与光标矩形查询。
 */

size_t EditorEngine::snapToCluster(int para, size_t offset) const {
    if (para < 0 || para >= static_cast<int>(blocks_.size())) return 0;
    const size_t len = blocks_[static_cast<size_t>(para)].cp_text.size();
    if (offset > len) offset = len;
    if (para < static_cast<int>(layout_.para_cluster_starts.size())) {
        const std::vector<uint32_t>& starts =
            layout_.para_cluster_starts[static_cast<size_t>(para)];
        if (!starts.empty()) return clusterFloor(starts, offset);
    }
    return offset;
}

const LineBox* EditorEngine::findLineFor(const TextPos& pos) const {
    const LineBox* best = nullptr;
    for (const LineBox& lb : layout_.lines) {
        if (lb.paragraph != pos.para) continue;
        if (pos.offset >= lb.first_cp && pos.offset <= lb.last_cp) return &lb;
        if (lb.first_cp <= pos.offset)
            best = &lb;
        else if (!best)
            best = &lb;
    }
    return best;
}

bool EditorEngine::caretRectFor(const TextPos& pos, int* page_out, Rect* out) const {
    const LineBox* lb = findLineFor(pos);
    if (!lb) return false;
    // 侧别只对"当前光标"这一处有意义（它是点画布时定下来的，见 setCaretAtSide）。
    //   其它位置（选区另一端、诊断打印）没有这个信息，按"最内侧"算。
    // 侧别直接就是排版侧的"跨过几枚簇"，不再经过控件下标 —— 括号占位各占一枚簇，
    //   两边说的是同一件事，中间那次翻译（下标 ↔ 节点 id）正是"画到另一侧去"的来源。
    const int side = (pos == caret()) ? caretSide() : defaultSideAt(pos.para, pos.offset);
    const float off = lb->offsetAtSide(static_cast<uint32_t>(pos.offset), side);
    const float t = cfg_.caret_width_pt;
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

bool EditorEngine::caretRectPt(Rect* out) const {
    int page = 0;
    return caretRectFor(caret(), &page, out);
}

Rect EditorEngine::caretRectPx() const {
    int page = 0;
    Rect r{};
    if (!caretRectFor(caret(), &page, &r)) return Rect{};
    const float zoom = cfg_.zoom;
    const int page_px_h = pagePxHeight();
    const int gap_px = pageGapPx();
    const float off_y = static_cast<float>((page - base_first_page_) * (page_px_h + gap_px));
    Rect px;
    px.x = std::floor(r.x * zoom);
    px.y = std::floor(r.y * zoom + off_y);
    px.w = std::max(1.0f, std::ceil(r.w * zoom));
    px.h = std::max(1.0f, std::ceil(r.h * zoom));
    return px;
}

int EditorEngine::caretPage() const {
    int page = 0;
    Rect r{};
    if (!caretRectFor(caret(), &page, &r)) return -1;
    return page;
}

/**
 * 唯一写入点：段内流位置。其余入口都只是把它换算成 rel 再调这里。
 */
void EditorEngine::setCaretRel(int para, int rel) {
    caret_para_ = para;
    caret_rel_ = rel;
}

/**
 * 光标位置（派生）：段号 + 段内码点偏移，由 caret_rel_ 算出来 —— 不再是独立状态。
 */
TextPos EditorEngine::caret() const {
    int para = caret_para_;
    if (para < 0) para = 0;
    if (!blocks_.empty() && para >= static_cast<int>(blocks_.size()))
        para = static_cast<int>(blocks_.size()) - 1;
    int off = cpOffsetInBlock(para, caret_rel_);
    if (off < 0) off = 0;
    return TextPos{para, static_cast<size_t>(off)};
}

/**
 * 按对外坐标（段, 段内码点偏移）写光标，侧别按"最内层含端点"投影一次。
 * 这是文本坐标投影到流位置的唯一约定，只给"没有侧别信息"的入口用（宿主程序化定位、
 * 排版收口）；编辑路径一律走 setCaretAtFlow / setCaretRel —— 侧别由操作自己算出来。
 */
void EditorEngine::setCaretByText(TextPos p) {
    if (blocks_.empty()) {
        setCaretRel(0, 0);
        return;
    }
    int para = p.para;
    if (para < 0) para = 0;
    if (para >= static_cast<int>(blocks_.size())) para = static_cast<int>(blocks_.size()) - 1;
    size_t off = p.offset;
    const size_t len = blocks_[static_cast<size_t>(para)].cp_text.size();
    if (off > len) off = len;
    setCaretRel(para, relAt(para, off, defaultSideAt(para, off)));
}

/**
 * 把"文本坐标 + 侧别"投影成 rel 并摆好，含重排。
 *
 * 为什么摆两次：聚焦档下"显示哪些括号"由光标位置决定（见 bracketedControls），
 * 而 rel 是按对象表算出来的 —— 先摆一次让重排看到光标的新位置，重排换表之后再摆一次，
 * rel 才是按最终那张表算的。只摆一次的话，rel 描述的是重排前那一批格子。
 * 两次之间输入（文本坐标 + 侧别）不变，所以收敛：第二次不会再触发重排。
 *
 * side < 0 = 没有侧别信息，按"最内层含端点"投影一次（见 setCaretByText）。
 */
void EditorEngine::projectCaret(int para, size_t offset, int side) {
    if (blocks_.empty()) {
        setCaretRel(0, 0);
        return;
    }
    if (para < 0) para = 0;
    if (para >= static_cast<int>(blocks_.size())) para = static_cast<int>(blocks_.size()) - 1;
    const auto place = [&]() {
        const size_t at = snapToCluster(para, offset);
        setCaretRel(para, relAt(para, at, (side < 0) ? defaultSideAt(para, at) : side));
    };
    const int plan_before = bracketPlanId();
    place();
    relayoutIfBracketPlanChanged(plan_before);
    place();
}

void EditorEngine::setCaret(int para, size_t offset) {
    // 这个入口是程序化定位（宿主恢复光标、测试摆位），不是用户的一次点击 —— 落点限制
    //   不在这里生效。限制在"用户真的能点到哪"的那三个入口上：点击（setCaretAt）、
    //   方向键（moveCaretBy）、Home/End 与 Tab。编辑被拒仍由 policyAt 兜住，
    //   所以"摆到集合外再打字"得到的仍然是拒绝，不会静默越界。
    projectCaret(para, offset, -1);
}

/**
 * 带侧别的定位（点画布）。
 *
 * 为什么偏移本身不够：同一个偏移上可能压着好几枚括号，"控件之前 / 进入控件 / 内容末尾 /
 * 控件之后"是互不相同的插入点。只存偏移，落点只能按"最内层、同深度取下标小者"来定，
 * 于是点后一个字段的开头时字会落进前一个字段；前一个字段只读时更直接 —— 编辑被拒，
 * 看起来就像点不进去。相邻的空字段（][][ ）更是整段位置都点不进去。
 *
 * 侧别就是被丢掉的那个分量，浏览器点画布时量得到：占位簇各占一枚，数格子即可。
 * 宿主把 hitTestPt 给的侧别原样带回来，这里与偏移合成 rel（唯一真源）。
 */
void EditorEngine::setCaretAtSide(int para, size_t offset, int side) {
    // 光标位置不受控件约束：字段外、只读字段里都能停。约束编辑的是能力表
    //   （插 / 删 / 分段 / 合并），不是光标 —— 两者混起来会得到"字段外的正文连点都点不进去"
    //   这种与编辑边界无关的怪现象。所以这里除了吸附到字素簇之外不改写任何东西。
    projectCaret(para, offset, side);
}

/**
 * 宿主与探针的说话方式：它们只知道控件下标，分不出共享边界的两侧，于是用
 * LineBox::kInlineOffsetOuter 说"贴外侧"。这里把它翻成侧别。
 */
void EditorEngine::setCaretAt(int para, size_t offset, int field) {
    if (blocks_.empty()) {
        setCaretAtSide(para, offset, -1);
        return;
    }
    int p = para;
    if (p < 0) p = 0;
    if (p >= static_cast<int>(blocks_.size())) p = static_cast<int>(blocks_.size()) - 1;
    const size_t at = snapToCluster(p, offset);
    setCaretAtSide(p, at, sideFromHint(p, at, field));
}

/**
 * 把光标摆到一个流位置上（绝对）。位置与侧别一次算准 —— 不猜、不查区间。
 *
 * 编辑路径全用它：删除后的落点是删除区间的起点、插入后是插入区间的终点、Tab 的目标、
 * 撤销恢复的位置，它们都已经是流位置。只留下一个码点偏移、再由"最内层含端点"猜回侧别的话，
 * 猜错的表象就是"删一个字光标跳一格"：删完"天"，落点会被猜成 ]中 之前，
 * 而它该在 ]中 之后（＝被删那个字所在的位置）。
 */
bool EditorEngine::setCaretAtFlow(FlowPos flow) {
    if (!flow_.has_value() || flow < 0) return false;
    TextPos p{};
    if (!flowToTextPos(flow, &p)) return false;
    if (p.para < 0 || p.para >= static_cast<int>(block_flow_.size())) return false;
    // 段内流位置 = 全局流位置 - 本段起点。位置与侧别由此一次算准（侧别是派生的）。
    setCaretRel(p.para, static_cast<int>(flow) - block_flow_[static_cast<size_t>(p.para)]);
    return true;
}

/**
 * 落点判定：走模型层的能力表（树 + 流位置）。
 * 六个编辑入口与四个光标入口都调它，免得十来处各写一遍。
 */
EditCheck EditorEngine::policyAt(TextPos p) const {
    if (formDegraded()) return EditCheck{true, ""};
    // 侧别由位置自己推（光标这一处用光标自己的侧别，见 sideForPos）——
    //   不从控件下标反推：同一个偏移上可能压着好几枚括号，反推会猜错。
    const FlowPos f = flowPosAtSide(p.para, p.offset, sideForPos(p));
    if (f < 0) return EditCheck{false, "落点不在文档流里"};
    return canEditAt(tree_, mode_, f);
}

EditCheck EditorEngine::policyErase(TextPos a, TextPos b) const {
    // 档 1 的删除不跨段；段落标记的删除走合并那条路（另有判定）。
    if (a.para != b.para) return EditCheck{false, "删除区间跨段"};
    if (formDegraded()) return EditCheck{true, ""};
    // 区间 = "这段文本码点自己占的那一段"，与实际删除（eraseOnTree）同一把尺子。
    //   端点取错侧就会被判成越出控件而拒掉，表象是"删到只剩一个字就删不动了"；
    //   取宽了又会把相邻的括号圈进来，表象是"删完光标跳一格"。
    int rel_from = 0;
    int rel_to = 0;
    if (!textSpanAt(a.para, a.offset, b.offset, &rel_from, &rel_to))
        return EditCheck{false, "删除区间不在文档流里"};
    const FlowPos base = block_flow_[static_cast<size_t>(a.para)];
    return canEditRange(tree_, mode_, Range{base + rel_from, base + rel_to});
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
bool EditorEngine::formDegraded() const { return mode_ == EditMode::Form && !hasEditableControl(); }

bool EditorEngine::hasEditableControl() const {
    for (int i = 0; i < controls_.size(); ++i) {
        if (!controls_.at(i).read_only) return true;
    }
    return false;
}

bool EditorEngine::moveCaretBy(int dcp) {
    if (blocks_.empty() || dcp == 0) return false;
    const int plan_before = bracketPlanId();

    int para = caret_para_;
    if (para < 0) para = 0;
    if (para >= static_cast<int>(blocks_.size())) para = static_cast<int>(blocks_.size()) - 1;
    int rel = caret_rel_;
    // 本段最后一个流位置 = 段长 + 本段边界枚数（＝ relAt(len, 全部边界)）。
    const auto span_of = [&](int q) {
        const size_t len = blocks_[static_cast<size_t>(q)].cp_text.size();
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
                } else if (para + 1 < static_cast<int>(blocks_.size())) {
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

    if (para == caret_para_ && rel == caret_rel_) return false;
    setCaretRel(para, rel);
    // 跨字段移动在聚焦档下会换占位集合、整行重排 —— 光标在 rel 上，重排不改变它。
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::moveCaretToLineStart() {
    const LineBox* lb = findLineFor(caret());
    if (!lb) return false;
    // 行首就是行首，不再往字段边界上收：光标位置不受控件约束（见 setCaret）。
    const TextPos p{lb->paragraph, lb->first_cp};
    if (p == caret()) return false;
    projectCaret(p.para, p.offset, -1);
    return true;
}

bool EditorEngine::moveCaretToLineEnd() {
    const LineBox* lb = findLineFor(caret());
    if (!lb) return false;
    const TextPos p{lb->paragraph, lb->last_cp};
    if (p == caret()) return false;
    projectCaret(p.para, p.offset, -1);
    return true;
}

void EditorEngine::showCaret(bool on) { caret_visible_ = on; }

/**
 * 编辑：插入与删除，先过落点判定（policyAt / policyErase），再按字素簇吸附。
 */

bool EditorEngine::insertCodepoints(int para, size_t offset, const std::vector<uint32_t>& cps) {
    if (cps.empty()) return false;
    if (para < 0 || para >= static_cast<int>(blocks_.size())) return false;
    // 插入前必须吸附：否则一串 e + U+0301 中间被插进字符，基字与标记就分家了。
    const size_t at = snapToCluster(para, offset);

    // 策略判定用的是吸附之后的位置 —— 判定用的位置和真正落笔的位置
    //   必须是同一个。否则会出现"策略放行了一个它根本没看过的位置"这种漏洞：
    //   吸附把 at 挪到簇边界之后，落笔点可能已经跨到另一个控件里去了。
    const TextPos ins{para, at};
    if (!policyAt(ins).allowed) {
        ++blocked_edits_;
        return false;
    }

    // 落笔在树上：插入是一条步骤，经事务提交，块流随后由新树重编译出来。
    // 落点的流位置由 (偏移, 侧别) 给出 —— 侧别取光标自己存的那个。
    //   从控件下标反推（老的 flowPosAt）在多枚括号压在同一个偏移上时会猜错：空嵌套里
    //   "最后一枚右括号之前"会被猜成"第一枚左括号之后"，字与光标一起插到别处去。
    const FlowPos at_flow = flowPosAtSide(para, at, sideForPos(ins));
    if (at_flow < 0) {
        ++blocked_edits_;
        return false;
    }
    InsertText s;
    s.at = at_flow;
    s.cps = cps;
    // 沿用落点左侧的字符格式：否则在粗体字中间敲出来的字不是粗的。
    s.marks = marksAtFlow(at_flow, &s.styleRef);
    Transaction tx;
    tx.label = "输入";
    tx.selection_before = Range{at_flow, at_flow};
    const FlowPos after = at_flow + static_cast<FlowPos>(cps.size());
    tx.selection_after = Range{after, after};
    tx.steps.push_back(Step{s});
    if (!commitOnTree(std::move(tx), para, false)) {
        ++blocked_edits_;
        return false;
    }
    // 落点 = 插入区间的终点，事务里已经算好了（tx.selection_after）—— 直接摆过去，
    //   不再"丢成一个码点偏移再猜侧别"。
    // 段号不变、段内码点偏移整体后移：控件的开闭括号会跟着内容走，而段的码点里
    //   没有它们，所以区间端点不必手工修。
    setCaretAtFlow(after);
    return true;
}

bool EditorEngine::insertCodepoint(int para, size_t offset, uint32_t cp) {
    return insertCodepoints(para, offset, std::vector<uint32_t>{cp});
}

bool EditorEngine::insertTextUtf8(int para, size_t offset, const std::string& utf8) {
    return insertCodepoints(para, offset, utf8ToCodepoints(utf8));
}

bool EditorEngine::typeTextUtf8(const std::string& utf8) {
    return insertCodepoints(caret().para, caret().offset, utf8ToCodepoints(utf8));
}

bool EditorEngine::backspace() {
    if (blocks_.empty()) return false;
    TextPos p = caret();
    if (p.para < 0 || p.para >= static_cast<int>(blocks_.size())) p.para = 0;
    p.offset = snapToCluster(p.para, p.offset);
    // 段首退格 = 并入上一段（跨段编辑）。策略拒绝时返回 false，与其它被拒编辑同一口径：
    //   宿主据此判断"没变"，光标也不会动。
    if (p.offset == 0) return mergeWithPreviousParagraph();

    // 往回删一个簇，不是删一个码点。
    //   e + U+0301 会被一次删掉；只删 U+0301 会留下一个孤立的组合重音。
    const size_t at = snapToCluster(p.para, p.offset - 1);
    // 策略：整个删除区间必须落在同一个可编辑控件内。
    // 这一条同时挡掉两个方向 —— 从控件外往里删、从控件里往外删。
    if (!policyErase(TextPos{p.para, at}, TextPos{p.para, p.offset}).allowed) {
        ++blocked_edits_;
        return false;
    }
    // at 与 p.offset 同段、且 at < p.offset（簇吸附保证的），所以这是一个良构的删除区间。
    return eraseOnTree(TextPos{p.para, at}, TextPos{p.para, p.offset}, "退格");
}

bool EditorEngine::deleteForward() {
    if (blocks_.empty()) return false;
    TextPos p = caret();
    if (p.para < 0 || p.para >= static_cast<int>(blocks_.size())) p.para = 0;
    p.offset = snapToCluster(p.para, p.offset);

    const size_t len = blocks_[static_cast<size_t>(p.para)].cp_text.size();
    // 段末 Delete = 把下一段并进来（与"段首退格并入上一段"对称）。
    if (p.offset >= len) return mergeWithNextParagraph();

    size_t next = p.offset + 1;
    if (p.para < static_cast<int>(layout_.para_cluster_starts.size()) &&
        !layout_.para_cluster_starts[static_cast<size_t>(p.para)].empty()) {
        const size_t ceil_cp =
            clusterCeil(layout_.para_cluster_starts[static_cast<size_t>(p.para)], p.offset);
        if (ceil_cp > p.offset) next = ceil_cp;
    }
    if (next > len) next = len;

    if (!policyErase(TextPos{p.para, p.offset}, TextPos{p.para, next}).allowed) {
        ++blocked_edits_;
        return false;
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
bool EditorEngine::splitParagraph() {
    if (blocks_.empty()) return false;
    TextPos p = caret();
    if (p.para < 0 || p.para >= static_cast<int>(blocks_.size())) p.para = 0;
    p.offset = snapToCluster(p.para, p.offset);

    // 分段 = 在断点处插入一个段落分隔符，所以判据与"在这里插字"完全相同。
    //   判定用的位置与真正插标记的位置必须是同一个，安全阀也只该有一处。
    if (!policyAt(p).allowed) {
        ++blocked_edits_;
        return false;
    }

    // 新标记插在光标处，于是它结束的是前半段：段落属性整体沿用原段（新段必须与原段
    //   同格式），只有一条例外 —— 拆出来的后半段不该跟着强制分页。
    //   后半段由被替代的那枚标记结束，所以那一条例外要落到它身上；原段不带强制
    //   分页时不必动它（那样会平白多出一条内联覆盖）。
    const NodeId mark_a = (p.para < static_cast<int>(block_src_.size()))
                              ? block_src_[static_cast<size_t>(p.para)].mark
                              : kInvalidNodeId;
    const Node* mk = tree_.find(mark_a);
    ParaFormatPatch patch;
    std::string style_ref;
    if (mk != nullptr && isParagraphMark(mk->kind)) {
        patch = mk->para;
        style_ref = mk->styleRef;
    }
    // 位置由 (偏移, 侧别) 给出，不从控件下标反推。
    //   反推在空控件处会猜错侧别，于是标记插到了别的控件里（"回车之后光标跑进第三个控件"）。
    const FlowPos at_flow = flowPosAtSide(p.para, p.offset, sideForPos(p));
    if (at_flow < 0) {
        ++blocked_edits_;
        return false;
    }
    Transaction tx;
    tx.label = "分段";
    tx.selection_before = Range{at_flow, at_flow};
    tx.selection_after = Range{at_flow + 1, at_flow + 1};
    InsertParaMark s;
    s.at = at_flow;
    s.para = patch;
    s.styleRef = style_ref;
    tx.steps.push_back(Step{s});
    if (mk != nullptr && isParagraphMark(mk->kind) &&
        blocks_full_[static_cast<size_t>(p.para)].page_break_before) {
        SetParaMarkFormat clear_pbb;
        clear_pbb.id = mark_a;
        clear_pbb.para = patch;
        clear_pbb.para.page_break_before = false;
        clear_pbb.styleRef = style_ref;
        clear_pbb.before = patch;
        clear_pbb.beforeStyleRef = style_ref;
        tx.steps.push_back(Step{clear_pbb});
    }
    if (!commitOnTree(std::move(tx), p.para, true)) {
        ++blocked_edits_;
        return false;
    }
    // 新标记之后那一位 = 新段的第一个位置。段数变了，后缀复用随之作废（见 commitOnTree）。
    setCaretAtFlow(at_flow + 1);
    return true;
}

/**
 * 合并的一般形式：把 head + 1 段并进 head 段。三种入口（段首退格 / 段末 Delete /
 * 显式调用）只是"从哪一端触发"不同，落到文档上的操作是同一个。
 */
bool EditorEngine::mergeParagraphInto(int head) {
    if (head < 0 || head + 1 >= static_cast<int>(blocks_.size())) return false;

    // 合并 = 删掉前一段的结束标记。两段的内容合成一段，而留下来的标记是后一段的 ——
    //   于是"合并之后那一段保留前一段的格式"这条约定不能白拿：要显式搬到后一段的标记上。
    const NodeId mark_a = block_src_[static_cast<size_t>(head)].mark;
    const NodeId mark_b = block_src_[static_cast<size_t>(head + 1)].mark;
    const Node* ma = tree_.find(mark_a);
    FlowPos at_a = 0;
    if (ma == nullptr || !flow_->intervalOf(mark_a, &at_a, nullptr)) return false;

    // 权限按"这枚段落标记能不能被删"判：区间 [at_a, at_a + 1) 就是标记自己那一位。
    //   判的是标记那一位可不可编辑，不是光标落在哪 —— 光标在字段外、标记在字段内时能合。
    //   安全阀（表单退化成文档模式）先过一遍，退化的情形该放行。
    if (!formDegraded() && !canEditRange(tree_, mode_, Range{at_a, at_a + 1}).allowed) {
        ++blocked_edits_;
        return false;
    }

    Transaction tx;
    tx.label = "合并";
    tx.selection_before = Range{at_a, at_a};
    tx.selection_after = Range{at_a, at_a};

    const ParaMarkBuild rm = makeRemoveParaMark(tree_, at_a);
    if (!rm.ok) return false;

    if (mark_b != kInvalidNodeId) {
        const Node* mb = tree_.find(mark_b);
        if (mb == nullptr) return false;
        SetParaMarkFormat keep;
        keep.id = mark_b;
        keep.para = ma->para;
        keep.styleRef = ma->styleRef;
        keep.before = mb->para;
        keep.beforeStyleRef = mb->styleRef;
        tx.steps.push_back(Step{keep});
        tx.steps.push_back(rm.step);
    } else {
        // 后一段是末尾那一段、没有结束标记：删掉前一段的标记之后，合并出来的那一段就没有
        //   标记了 —— 它会按默认段落格式编译，前一段的格式静默丢掉。所以补一枚标记在末尾，
        //   带上被删那枚的格式。
        //   位置要按"删完之后"的坐标系给：删除让标记之后的一切前移一位。
        const FlowPos end_before = block_flow_[static_cast<size_t>(head + 1)] +
                                   block_flow_span_[static_cast<size_t>(head + 1)];
        InsertParaMark ins;
        ins.at = mapPos(rm.step, end_before, Assoc::After);
        ins.para = ma->para;
        ins.styleRef = ma->styleRef;
        tx.steps.push_back(rm.step);
        tx.steps.push_back(Step{ins});
    }
    if (!commitOnTree(std::move(tx), head, true)) {
        ++blocked_edits_;
        return false;
    }
    setCaretAtFlow(at_a);  // 被删掉的那枚段落标记的位置 = 合并点
    return true;
}

bool EditorEngine::mergeWithPreviousParagraph() {
    if (blocks_.empty()) return false;
    TextPos p = caret();
    if (p.para <= 0 || p.para >= static_cast<int>(blocks_.size())) return false;
    // 只有光标确实在段首才合并（否则退格该删的是前一个簇）。
    if (snapToCluster(p.para, p.offset) != 0) return false;
    return mergeParagraphInto(p.para - 1);
}

bool EditorEngine::mergeWithNextParagraph() {
    if (blocks_.empty()) return false;
    TextPos p = caret();
    if (p.para < 0 || p.para + 1 >= static_cast<int>(blocks_.size())) return false;
    const size_t len = blocks_[static_cast<size_t>(p.para)].cp_text.size();
    if (snapToCluster(p.para, p.offset) != len) return false;
    return mergeParagraphInto(p.para);
}

bool EditorEngine::typeTextAndRender(const std::string& utf8) {
    if (!typeTextUtf8(utf8)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::backspaceAndRender() {
    if (!backspace()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::deleteForwardAndRender() {
    if (!deleteForward()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::splitParagraphAndRender() {
    if (!splitParagraph()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::mergeWithPreviousParagraphAndRender() {
    if (!mergeWithPreviousParagraph()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::mergeWithNextParagraphAndRender() {
    if (!mergeWithNextParagraph()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

/**
 * 表单：区间标注加编辑策略。
 *
 * 这一节一行都不碰排版管线。控件不参与断行/分页，只做两件事：给一段已有的文字挂上
 * "类型加可编辑性"（Control / ControlSet），以及在表单模式下把这段文字的几何画成
 * "字段"的样子（controlRects）。这正是选区间模型而不是矩形模型换来的。
 *
 * 控件集合没有独立的写入口：它是编译产物的派生结果（见 exportControlsFromBlocks），
 * 换一份字段名单 = 换一棵树 = 重新装载一次文档。
 */

void EditorEngine::setEditMode(EditMode m) {
    if (mode_ == m) return;
    const int plan_before = bracketPlanId();
    mode_ = m;

    // 切模式要重编译，而且整篇重排：隐藏控件的可见性口径随模式变（文档模式是设计态、
    //   表单与只读是呈现态），所以块流本身可能变 —— 隐藏字段的子树被摘除或放回，段数都可能
    //   跟着变。不重编译的话，呈现态下那些内容还留在版面上，而它们已经不可寻址。
    //   不能走后缀复用：摘除/放回发生在文档任何位置，脏区不是一个后缀。
    rebuildFromTree();
    markAllLayoutDirty();

    // 切模式会改变占位集合本身（文档模式一个都不占、表单模式要占），
    //   所以这里可能也要重排（Focused 档下还有"光标被夹进最近字段"这一层）。
    relayoutIfBracketPlanChanged(plan_before);

    // 切模式只影响帧上的标记（画 / 不画），底图一个字都不变 ——
    //   所以不动页指纹，只要求下一次整帧重贴。
    //   如果哪天标记改回"画进底图"，这里必须换成重算页指纹 ——
    //     否则切回文档模式后括号会残留，而"内容没变 → 跳过重画"会让它一直在。
    //     对应的断言是单测里"切回文档模式后一个像素都不剩"那一条。
    frame_needs_full_refresh_ = true;
}

int EditorEngine::controlAtPt(float x_pt_frame, float y_pt_frame) const {
    TextPos p{};
    if (!hitTestPt(x_pt_frame, y_pt_frame, &p)) return -1;
    // include_end = false：点在内容上才算"点进了这个控件"。
    // 点在字段末尾之后一位、视觉上已经在框外，就不该算。
    return controls_.innermostAt(p, /*include_end=*/false);
}

std::vector<EditorEngine::ControlRect> EditorEngine::controlRects(int index) const {
    std::vector<ControlRect> out;
    if (index < 0 || index >= controls_.size()) return out;
    const ControlRange& cr = controls_.at(index).range;

    for (const LineBox& lb : layout_.lines) {
        if (lb.paragraph < cr.start.para || lb.paragraph > cr.end.para) continue;

        // 这一行里被区间覆盖的码点范围 [lo, hi)
        uint32_t lo = lb.first_cp;
        uint32_t hi = lb.last_cp;
        if (lb.paragraph == cr.start.para && cr.start.offset > lo)
            lo = static_cast<uint32_t>(cr.start.offset);
        if (lb.paragraph == cr.end.para && cr.end.offset < hi)
            hi = static_cast<uint32_t>(cr.end.offset);
        if (lo >= hi) continue;

        // 左右边缘不能用同一个函数取，这是最容易写错的地方：
        //   inlineOffsetFor(lo) 给的是"lo 那个字的左边缘"（对）；
        //   而 inlineOffsetFor(hi) 给的是"hi 那个字的左边缘" —— 那是区间内最后一个字
        //   的左边缘，会整整少掉一个字的宽度（症状：框比字窄一格，看着像没框住）。
        //   右边缘必须取"最后一个 cp < hi 的簇的右边缘"。
        // 跳过占位簇：括号在内容之外（[内容]），控件矩形只该框住内容。
        //   不跳的话矩形会把前缀的括号宽度也算进来，左边缘会左移一个括号 ——
        //   于是"点在字段矩形中心"这类断言会差几像素，而画面看着又是对的。
        float left = lb.inline_end_pt;
        for (const LineCluster& c : lb.clusters) {
            if (c.cp >= lo && !c.placeholder) {
                left = c.inline_pt;
                break;
            }
        }
        float right = left;
        for (size_t i = 0; i < lb.clusters.size(); ++i) {
            if (lb.clusters[i].cp >= hi) break;
            right = (i + 1 < lb.clusters.size()) ? lb.clusters[i + 1].inline_pt : lb.inline_end_pt;
        }
        if (!(right > left)) continue;

        const float a0 = lb.inline_origin_pt + left;
        const float a1 = lb.inline_origin_pt + right;
        Rect r;
        if (!lb.vertical) {
            r.x = a0;
            r.y = lb.block_lo_pt;
            r.w = a1 - a0;
            r.h = lb.block_hi_pt - lb.block_lo_pt;
        } else {
            r.x = lb.block_lo_pt;
            r.y = a0;
            r.w = lb.block_hi_pt - lb.block_lo_pt;
            r.h = a1 - a0;
        }
        out.push_back(ControlRect{lb.page, r});
    }
    return out;
}

bool EditorEngine::focusControl(bool forward) {
    std::vector<TextPos> starts;
    for (const Control& c : controls_.all()) {
        if (!c.read_only) starts.push_back(c.range.start);
    }
    if (starts.empty()) return false;
    std::sort(starts.begin(), starts.end(),
              [](const TextPos& a, const TextPos& b) { return a < b; });

    TextPos target{};
    bool found = false;
    if (forward) {
        for (const TextPos& s : starts) {
            if (caret() < s) {
                target = s;
                found = true;
                break;
            }
        }
        if (!found) target = starts.front();  // 到头绕回第一个（表单里 Tab 就该循环）
    } else {
        for (auto it = starts.rbegin(); it != starts.rend(); ++it) {
            if (*it < caret()) {
                target = *it;
                found = true;
                break;
            }
        }
        if (!found) target = starts.back();
    }
    if (target == caret()) return false;
    const int plan_before = bracketPlanId();
    setCaretByText(target);
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

/**
 * 表单标记的"计划"指纹：编辑模式 + （Focused 档下的）聚焦字段 + 每个控件的投影矩形。
 * 这就是 presentInternal 里"变了就整帧重贴"的判据。
 * 它必须只包含会影响标记的东西 —— 多塞了会让整帧重贴变得频繁
 * （整帧重贴本身不贵，但它会把增量重绘的账面数字冲掉），
 * 少塞了则会把陈旧标记留在画面上。
 * All 档（默认）下括号与光标无关，所以不把聚焦字段塞进去：
 * 塞了的话光标每跨一次字段就整帧重贴一次 —— 那正是"光标移动 = 真增量"
 * 最想避免的开销，而且换不来任何画面差异（这么写是量出来的，
 * 不是推出来的：记号里多一项，跨字段时的 repainted_pixels 会立刻跳起来）。
 */
uint64_t EditorEngine::markPlanSignature() const {
    uint64_t h = kFnvBasis;
    h = fnvMix(h, static_cast<uint64_t>(static_cast<int>(mode_)));
    if (cfg_.form_brackets == FormBracketScope::Focused) {
        h = fnvMix(h, static_cast<uint64_t>(static_cast<int64_t>(focusedControlIndex() + 1)));
    }
    for (int ci = 0; ci < controls_.size(); ++ci) {
        const Control& c = controls_.at(ci);
        h = fnvMix(h, c.read_only ? 1ULL : 0ULL);
        for (const ControlRect& cr : controlRects(ci)) {
            h = fnvMix(h, static_cast<uint64_t>(cr.page));
            h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(cr.r_pt.x))));
            h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(cr.r_pt.y))));
            h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(cr.r_pt.w))));
            h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(cr.r_pt.h))));
        }
    }
    return h;
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
bool EditorEngine::indent() {
    const int n = std::max(0, cfg_.tab_spaces);
    if (n == 0) return false;
    std::string spaces(static_cast<size_t>(n), ' ');
    return typeTextUtf8(spaces);
}

bool EditorEngine::outdent() {
    if (blocks_.empty()) return false;
    TextPos p = caret();
    if (p.para < 0 || p.para >= static_cast<int>(blocks_.size())) p.para = 0;
    p.offset = snapToCluster(p.para, p.offset);

    const std::vector<uint32_t>& cps = blocks_[static_cast<size_t>(p.para)].cp_text;
    // 光标前连续空格的起点（最多 tab_spaces 个；只在本段内找，
    // 不跨段回退 —— 跨段删空格会把上一段的段尾吃掉，那是不对的）。
    size_t at = p.offset;
    int n = 0;
    while (at > 0 && n < std::max(0, cfg_.tab_spaces) && cps[at - 1] == 0x20u) {
        --at;
        ++n;
    }
    if (n == 0) return false;

    // 与 backspace 同一条策略：删掉的区间必须落在同一个可编辑控件内。
    if (!policyErase(TextPos{p.para, at}, TextPos{p.para, p.offset}).allowed) {
        ++blocked_edits_;
        return false;
    }
    return eraseOnTree(TextPos{p.para, at}, TextPos{p.para, p.offset}, "反缩进");
}

bool EditorEngine::indentAndRender() {
    if (!indent()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::outdentAndRender() {
    if (!outdent()) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::focusControlAndRender(bool forward) {
    if (!focusControl(forward)) return false;
    // 必须收尾重绘，不能只改状态。
    //
    //   focusControl 会跨字段移动光标，导致占位变化与整行重排。若只改状态，
    //   JS 侧紧接着读 editStatsJson() 拿到的是上一次呈现留下的账（脏矩形属于上一次
    //   事件），用它 putImageData 只会贴上画布一小块，形成"引擎帧是新的、画布是新旧
    //   混合"。
    //
    //   规矩（整个 wasm 接口面都遵守）：凡是会改引擎状态的入口，返回宿主前必须把
    //   画面收尾。typeText/backspace/moveCaret
    //   都是 *AndRender，唯独 focusControl 漏了。
    repaintAfterEdit();
    return true;
}

bool EditorEngine::tabCaret(bool backward) {
    if (blocks_.empty()) return false;
    // 文档模式与只读模式都不消费 Tab：前者要把 Tab 当制表符，后者要放行给宿主
    //   （吞掉会把键盘用户困在编辑器里）。模型文档第七节把这条写成了分工。
    if (mode_ != EditMode::Form) return false;

    const int plan_before = bracketPlanId();
    const TextPos before = caret();
    if (!flow_.has_value()) return false;
    const FlowPos from = flowPosAtSide(caret().para, caret().offset, caretSide());
    if (from < 0) return false;
    const TabStop stop = tabFrom(tree_, mode_, from, backward);
    // handled 为假 = 已经没有下一个（上一个）可编辑控件了：放行给宿主，不绕回第一个。
    if (!stop.handled) return false;
    // 目标落在控件的内容起点上，共享边界上"前一个控件的末尾"与"这个控件的开头"是
    //   同一个偏移 —— 位置与侧别都由目标流位置一次算准（重推会推成前一个控件，
    //   Tab 跳过去就打不进字）。
    setCaretAtFlow(stop.target);
    if (caret() == before) return false;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::tabCaretAndRender(bool backward) {
    if (!tabCaret(backward)) return false;
    // 与 focusControlAndRender 同一条纪律：跨字段移动会让占位变化、整行重排，
    //   只改状态会让宿主拿上一次的脏矩形去上屏，画布成新旧混合体。
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::moveCaretAndRender(int dcp) {
    if (!moveCaretBy(dcp)) return false;
    // 光标移动不重排 —— 这就是增量的意义：
    //   底图一个字都没变，只需要把两个小矩形贴回去。
    //
    // 这里用裸 repaint()，刻意不把光标拉成可见（repaintAfterEdit 才会）。
    //   理由：移动光标不是编辑，光标该继续按自己的相位闪；把它强行点亮会让
    //   "按着方向键"时看不出闪烁。而"隐藏相位 + 宿主补救一次 present"这条
    //   双 present 路径现在是安全的 —— 上屏走 takeBlitBox() 的累加器，
    //   第二次的小盒子挤不掉第一次的账（见 takeBlitBox 的注释、单测 [12]）。
    //   顺带一条：单测里"光标隐藏时写入量更小（只擦不画）"这条断言，
    //   量的正是这条路径 —— 把它改成 repaintAfterEdit 会让它失去意义。
    repaint();
    return true;
}

bool EditorEngine::moveCaretToLineStartAndRender() {
    if (!moveCaretToLineStart()) return false;
    repaint();  // 不重排 —— 和 moveCaretAndRender 同一条增量路径（也不点亮光标，同上）
    return true;
}

bool EditorEngine::moveCaretToLineEndAndRender() {
    if (!moveCaretToLineEnd()) return false;
    repaint();  // 同 moveCaretAndRender：不重排、不点亮光标
    return true;
}

/**
 * 命中测试：几何的反向半边 —— 帧坐标反查 (段, 偏移) + 侧别 + 归属控件。
 */

bool EditorEngine::hitTestPt(float x_pt_frame, float y_pt_frame, TextPos* out, int* out_owner,
                             int* out_side) const {
    if (out_owner) *out_owner = -1;
    if (out_side) *out_side = 0;
    if (!out || layout_.lines.empty()) return false;

    const float page_w = cfg_.page.width_pt;
    const float page_h = cfg_.page.height_pt;
    const float stride = page_h + cfg_.page_gap_pt;

    int page = 0;
    float local_y = y_pt_frame;
    if (stride > 0.0f) {
        // 帧是视口窗口：传进来的 y 是"窗口内"的，而页号是文档里的绝对页号。
        //   加回窗口首页的偏移才是同一个坐标系 —— 与 caretRectPx 的 off_y 对称，
        //   少了这一步，点第二页会被判成第一页（整体少了 base_first_page_ 页）。
        const float doc_y = y_pt_frame + static_cast<float>(base_first_page_) * stride;
        page = static_cast<int>(std::floor(doc_y / stride));
        if (page < 0) page = 0;
        local_y = doc_y - static_cast<float>(page) * stride;
        // 落在页间空隙里，或者横向出了页外 —— 没有"这一页的哪一行"可言
        if (local_y < 0.0f || local_y > page_h) return false;
        if (x_pt_frame < 0.0f || x_pt_frame > page_w) return false;
    }

    // 块轴：找同一页里最近的一行。
    const LineBox* best = nullptr;
    float best_d = 1e30f;
    for (const LineBox& lb : layout_.lines) {
        if (lb.page != page) continue;
        const float bp = lb.vertical ? x_pt_frame : local_y;
        const float d = (bp < lb.block_lo_pt) ? (lb.block_lo_pt - bp)
                                              : (bp > lb.block_hi_pt ? bp - lb.block_hi_pt : 0.0f);
        if (d < best_d) {
            best_d = d;
            best = &lb;
        }
    }
    if (!best) return false;

    // 行内轴：找最近的插入点。
    //
    //   每个插入点的"势力范围"是它到相邻插入点的中点。
    //   落在某个簇的后半段时，光标会跳到下一个簇的首码点 ——
    //     这就是"字素不可切分"的正确语义：点 e+U+0301 的右半边，
    //     光标跑到它后面，而不是钻进它内部。
    const float ip = best->vertical ? local_y : x_pt_frame;
    const float rel = ip - best->inline_origin_pt;
    const size_t n = best->clusters.size();

    size_t cp = best->last_cp;  // 默认：落在最后半个簇之后 → 行末
    // 命中的簇下标（= clusters.size() 表示落在最后一个簇之后）。落点是"这一簇之前"那个
    //   边界，侧别就是同一个偏移上排在它前面的簇数 —— 数格子，不需要认识括号。
    size_t hit_i = n;
    if (rel < 0.0f) {
        cp = n ? best->clusters.front().cp : best->first_cp;
        hit_i = n ? 0 : n;
    } else {
        for (size_t i = 0; i < n; ++i) {
            const float a = best->clusters[i].inline_pt;
            const float b = (i + 1 < n) ? best->clusters[i + 1].inline_pt : best->inline_end_pt;
            if (rel < (a + b) * 0.5f) {
                cp = best->clusters[i].cp;
                hit_i = i;
                break;
            }
        }
    }
    const size_t k0 = best->firstClusterAt(static_cast<uint32_t>(cp));
    const int side = (hit_i > k0) ? static_cast<int>(hit_i - k0) : 0;

    TextPos p{best->paragraph, cp};
    // 侧别是落点的完整身份（同一个偏移上有几个位置就看它）；归属只是它的一个投影，
    //   给宿主显示"当前在哪个字段"用。两者一起交出去：宿主搬运的是侧别。
    if (out_side) *out_side = side;
    int owner = -1;
    if (controls_.size() > 0)
        owner = hintFromRel(best->paragraph, relAt(best->paragraph, cp, side));
    // 命中即落点：光标位置不受控件约束，所以这里不再做"就近吸附到最近字段"。
    //   吸附会把"点字段外正文"改写成"停在字段端点"，于是画出来的光标不在点的地方，
    //   表象就是"点了没反应"。归属照实返回，编辑侧自己会按策略拒绝。
    if (out_owner) *out_owner = owner;
    *out = p;
    return true;
}

}  // namespace heditor
