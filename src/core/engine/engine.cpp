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

/**
 * 光标宽度的下限（磅）。窄于它之后，缩放到 1.0 以下时细到看不见 ——
 * 而"看不见的光标"与"没有光标"在界面上无法区分。比它更细的取值一律夹到这里。
 */
constexpr float kMinCaretWidthPt = 0.4f;

/**
 * Skia 全局字形 mask 缓存的字节预算。
 *
 * SkStrikeCache 的默认上限是 2 MB（SK_DEFAULT_FONT_CACHE_LIMIT），而它清理时按
 *   "整份 strike"删：internalPurge 断言不做小清理（bytesNeeded 至少取已用的四分之一），
 *   从 LRU 尾巴上摘对象，一次就是一份 strike。本项目一般只有一两个字体规格，
 *   也就是一个 strike —— 越过 2 MB 就等于缓存被整份抹掉，接下来每个字形都重新冷光栅，
 *   而"预热"这件事也跟着作废（预热正是把 mask 送进这份缓存）。
 * 取 48 MB：一页约 700 个字形、mask 数百字节，再乘 setSubpixel(true) 的横向相位
 *   （同一字形按 1/4 px 拆成多份 mask），一页的常驻量在 MB 量级；整篇用到的不同字形
 *   撑死几万份 mask，48 MB 足够整篇一次装下、之后只命中。
 */
constexpr size_t kGlyphCacheBudgetBytes = 48u * 1024u * 1024u;

}  // namespace

/**
 * 生命周期：字体加载与引擎初始化。
 */

bool EditorEngine::init(const std::vector<std::string>& fontPaths) {
    // 视图先摆好：init 之后任何一次光标读写都要它，而它只存容器地址（之后不再变）。
    refreshViews();
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

    ensureRenderSetup();
    return true;
}

/**
 * 光栅后端那几项一次性设置。目前只有一项：Skia 全局字形 mask 缓存的预算。
 *
 * 为什么必须显式设：SkStrikeCache 的默认上限只有 2 MB（SK_DEFAULT_FONT_CACHE_LIMIT），
 *   而它清理时按"整份 strike"删 —— internalPurge 断言"不做小清理"，从 LRU 尾巴上
 *   摘对象，一次就是一份 strike。本项目一般只有一两个字体规格，也就是一个 strike：
 *   越限就等于把缓存一次抹光，接下来每个字形都重新冷光栅。
 * 幂等：engine.h 里记了为什么要放在这里而不是 init()（wasm 宿主不走 init）。
 */
void EditorEngine::ensureRenderSetup() {
    if (render_setup_done_) return;
    render_setup_done_ = true;
    base_.setGlyphCacheBudget(kGlyphCacheBudgetBytes);
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
    // 连页标志与条带高度是引擎从"页度量 + 页间隙"装配出来的，排版层只认 PageMetrics。
    //   必须在比较之前装配 —— 否则切模式时 PageMetrics 没变，判不出"要重排"，
    //   画面上就成了"分页的排版 + 连页的落点"（内容按旧断点排，却按新条带贴）。
    PageMetrics want = cfg.page;
    want.continuous = cfg.continuous;
    // 只有连页才需要条带高度。分页下留 0，排版按 slabHeightPt 的回退值算（页高）；
    //   这样改页间隙不会牵动分页的"要不要重排"判据 —— 那个改动只影响落点，不影响断行。
    want.slab_pt = cfg.continuous
                       ? want.height_pt + (cfg.page_gap_pt > 0.0f ? cfg.page_gap_pt : 0.0f)
                       : 0.0f;
    // 页度量改了整篇的断行位置都会变，没有可定位的影响面 → 整篇标脏。
    const bool page_changed = (want != cfg_.page);
    cfg_ = cfg;
    cfg_.page = want;
    if (cfg_.zoom < 0.25f) cfg_.zoom = 0.25f;
    if (cfg_.zoom > 8.0f) cfg_.zoom = 8.0f;
    if (cfg_.caret_width_pt < kMinCaretWidthPt) cfg_.caret_width_pt = kMinCaretWidthPt;
    base_valid_ = false;  // 缩放/页度量变了，底图作废
    caret_rect_px_prev_ = Rect{};
    if (plan_changed) markLayoutDirtyForBrackets();
    if (page_changed) markAllLayoutDirty();
    if (plan_changed || page_changed) relayout();
    // 撤销上限不影响呈现，只影响栈能留多长 —— 调小的话当场裁掉多余的（不必等下一次提交）。
    if (undo_) undo_->setLimit(cfg_.undo_limit);
    refreshViews();
}

void EditorEngine::setCaretAndSelectionStyle(const Color& caret, float caret_width_pt,
                                             const Color& selection) {
    cfg_.caret_color = caret;
    cfg_.caret_width_pt = caret_width_pt < kMinCaretWidthPt ? kMinCaretWidthPt : caret_width_pt;
    cfg_.selection_color = selection;
    refreshViews();
}

/**
 * 文档装载：模型树那条路（第十四节的接缝）。
 */
void EditorEngine::setDocument(const NodeTree& tree, const StyleSheet& sheet) {
    refreshViews();
    tree_.assignFrom(tree);
    sheet_ = sheet;
    loaded_ = true;
    // 撤销栈在建栈时就绑定了 tree_ 的地址，而 assignFrom 只换内容、不换对象 —— 所以
    //   换文档时旧栈必须整个丢掉：它记的步骤属于上一份文档，坐标在新文档上毫无意义。
    undo_ = std::make_unique<UndoStack>(tree_);
    undo_->setLimit(cfg_.undo_limit);
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
    // 视图先摆好：下面这一段就要用位置解析（落基、控件导出），而它读的是视图。
    //   容器都是成员、不换对象，所以地址现在取和最后取一样。
    refreshViews();
    edit_stats_.rebuild_path = 0;
    const FlowVisibility vis = flowVisibility();
    const auto t_compile0 = Clock::now();
    CompileResult res = compileDocument(tree_, vis, sheet_);
    blocks_full_ = std::move(res.blocks);
    block_src_ = std::move(res.sources);
    edit_stats_.rebuild_compile_ms = msSince(t_compile0);
    finishRebuildFromTree(vis);
}

bool EditorEngine::rebuildParagraphFromTree(int para) {
    if (!compileParagraphInto(para)) return false;
    edit_stats_.rebuild_path = 1;
    edit_stats_.rebuild_reject = 0;
    finishRebuildFromTree(flowVisibility());
    return true;
}

bool EditorEngine::compileParagraphInto(int para) {
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

    // 段的流起点必须与容器内这段区间的起点重合。
    //
    //   为什么非判不可：在控件内容里回车时，段落标记落在控件内部（跨段控件是正常形态），
    //   而这一段的内容其实起自控件之外 —— 段首那枚边界可能是前一段的控件在这里结束的
    //   闭括号，它属于容器之外的兄弟。只编"容器内这一截子节点"，产物就成了半个段落：
    //   段首的边界丢掉，block_src_ / block_flow_ 又被这半个产物覆盖，之后每一次编辑都按
    //   错的坐标算、控件集合也空掉，界面上表现为退格删一个字、前面一串字连同控件一起不见。
    //
    //   用流位置判而不是看"第一个内容元素的归属"：归属是 block_src_ 记下来的，记法漏掉
    //   一种形态判据当场失效（闭括号就没被记过）。流位置是段与区间各自的权威口径，
    //   与来源身份怎么记无关。
    if (!flow_.has_value()) return false;
    if (begin < 0 || begin >= static_cast<int>(container->children.size())) return false;
    int seg_start = -1;
    if (para == 0) {
        // 段 0 前面可能有节边界，它的第一个内容元素才是权威起点。
        if (!flow_->intervalOf(block_src_[0].first, &seg_start, nullptr)) return false;
    } else {
        // 段的起点 = 上一枚段落标记之后那一位（标记位置是段区间的权威，见 finishRebuildFromTree）。
        int prev_end = 0;
        if (!flow_->intervalOf(block_src_[static_cast<size_t>(para) - 1].mark, nullptr,
                               &prev_end)) {
            return false;
        }
        seg_start = prev_end;
    }
    int range_start = -1;
    if (!flow_->intervalOf(container->children[static_cast<size_t>(begin)]->id, &range_start,
                           nullptr)) {
        return false;
    }
    if (seg_start < 0 || range_start < 0 || seg_start != range_start) return false;

    const FlowVisibility vis = flowVisibility();
    const auto t_compile0 = Clock::now();
    CompileResult part = compileChildRange(tree_, container->id, begin, mark_idx, vis, sheet_);
    if (part.blocks.size() != 1 || part.sources.size() != 1) return false;
    blocks_full_[static_cast<size_t>(para)] = std::move(part.blocks[0]);
    block_src_[static_cast<size_t>(para)] = part.sources[0];
    edit_stats_.rebuild_compile_ms += msSince(t_compile0);
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
    // 光标与块起点是同一套坐标的两半，所以重建块起点时必须一起重算光标：光标存的是全局
    //   流位置，而这个下标只有配上 block_flow_ 才读得出"段与段内位置"。基一换，光标的数字
    //   没变、含义却变了 —— 前面某一段变长之后，它会静默漂到相邻位置。
    //   重算按"段内位置"落基而不是保留绝对下标：光标此刻的语义是"停在这一段内容的这个流
    //   位置上"，段落内容变了它该跟着文本走。编辑之后真正的跟随由事务的位置映射负责，
    //   这条只保证"基换过之后读数仍然自洽"。
    //   两个读数必须在覆盖 block_flow_ 之前取，assign 之后旧基就没了。
    //   选区落地之后 anchor 也要跟着落基：它现在是一个真位置，只落 focus 会把它收成空选区
    //   （症状是"Shift 扩展之后来一次重编译，选区就没了"）。
    const int rebase_para = caret_.caretPara();
    const int rebase_rel = caret_.caretRel();
    const int rebase_anchor_para = caret_.paraForFlow(caret_.selection().anchor);
    const int rebase_anchor_rel = caret_.relForFlow(caret_.selection().anchor);
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
    // 新基写完了才落基（见上面那段说明）。两端一起落。
    caret_.rebaseSelection(rebase_anchor_para, rebase_anchor_rel, rebase_para, rebase_rel);

    edit_stats_.rebuild_flowpos_ms = msSince(t_pos0);
    const auto t_copy0 = Clock::now();
    blocks_ = blocks_full_;
    edit_stats_.rebuild_copy_ms = msSince(t_copy0);
    const auto t_ctl0 = Clock::now();
    exportControlsFromBlocks();
    edit_stats_.rebuild_controls_ms = msSince(t_ctl0);
}

/**
 * 分段之后的局部重建（说明见 engine.h 的同名声明）。
 *
 * 找新标记的办法：在原标记所在容器里往回找最近的一枚段落标记 —— 它就是新插入的那一枚
 *   （插入点落在段的内容中间，两者之间隔着段的后半内容）。只有"事务确实只插了一枚段落
 *   标记"时才成立，所以下面用两条判据锁住形状：它的流区间末尾必须与原标记的区间起点
 *   重合，且它必须落在原段的区间之内。控件内分段、跨容器插入都会被挡下并退回全量。
 */
bool EditorEngine::rebuildAfterSplit(int at, int* reject_out) {
    auto reject = [&](int code) {
        if (reject_out != nullptr) *reject_out = code;
        return false;
    };
    if (at < 0) return reject(1);
    if (block_src_.size() != blocks_full_.size()) return reject(2);
    if (at + 1 > static_cast<int>(block_src_.size())) return reject(3);
    if (at >= static_cast<int>(block_flow_.size())) return reject(4);

    // 段 at 的原结束标记归新段：新插入的那枚在它前面，被它结束的是前半段。
    const NodeId mark_b = block_src_[static_cast<size_t>(at)].mark;
    const Node* mb = tree_.find(mark_b);
    if (mb == nullptr || !isParagraphMark(mb->kind) || mb->parent == nullptr) return reject(5);
    const Node* container = mb->parent;
    int idx = -1;
    for (size_t i = 0; i < container->children.size(); ++i) {
        if (container->children[i]->id == mark_b) {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (idx <= 0) return reject(6);
    // 新标记 = 原标记之前最近的那一枚段落标记。
    //   插入点落在段的内容中间，所以两者之间隔着段的后半内容 —— 不是紧邻的兄弟。
    //   往回找第一枚段落标记即可；它是不是"贴着原标记的那一枚"由下面的区间判据锁住
    //   （控件内分段那一类会让两者区间不相接，当场退回全量）。
    const Node* mark_a = nullptr;
    for (int i = idx - 1; i >= 0; --i) {
        const Node* sib = container->children[static_cast<size_t>(i)];
        if (sib != nullptr && isParagraphMark(sib->kind)) {
            mark_a = sib;
            break;
        }
    }
    if (mark_a == nullptr) return reject(7);

    // 流索引必须先按新树重建：区间定位全靠它，而旧索引里没有新插入的那枚标记。
    //   这一笔与"整树重编"无关，是局部重建自己的固定成本（2000 段实测约 0.9 ms）。
    const FlowVisibility vis = flowVisibility();
    const auto t_index0 = Clock::now();
    flow_.emplace(tree_, vis);
    edit_stats_.rebuild_flow_ms = msSince(t_index0);

    // 形状判据：新标记必须整个落在原段 at 的区间之内 —— 起点不早于段起点，末尾不晚于原
    //   标记的起点。插入点在段的内容中间，所以两者之间本就隔着段的后半内容，不能要求它们
    //   首尾相接（那是我按"插入点总在段尾"想出来的条件，与真实形状不符）。
    int a_first = 0;
    int a_end = 0;
    int b_first = 0;
    int b_end = 0;
    if (!flow_->intervalOf(mark_a->id, &a_first, &a_end)) return reject(8);
    if (!flow_->intervalOf(mark_b, &b_first, &b_end)) return reject(9);
    if (a_end > b_first) return reject(10);
    if (a_first < block_flow_[static_cast<size_t>(at)]) return reject(11);

    // 数组对齐到新树：段 at+1 是新增的那一段，先占位（它的首个内容元素由下面的局部
    //   编译填对）；段 at 改由新标记结束。其余段落下标整体后移一位，内容原样不动。
    BlockSource placeholder;
    placeholder.mark = mark_b;
    block_src_.insert(block_src_.begin() + at + 1, placeholder);
    blocks_full_.insert(blocks_full_.begin() + at + 1, BlockSpec{});
    block_src_[static_cast<size_t>(at)].mark = mark_a->id;

    // 两段各编一次。任何一段编不成，调用方会退回整树重建 —— 那条路把 blocks_full_ 与
    //   block_src_ 整体换掉，所以上面这几处改动不会留下半截产物。
    if (!compileParagraphInto(at)) return reject(12);
    if (!compileParagraphInto(at + 1)) return reject(13);

    finishRebuildFromTree(vis);
    edit_stats_.rebuild_path = 2;
    if (reject_out != nullptr) *reject_out = 0;
    return true;
}

int EditorEngine::caretSide() const { return caret_.caretSide(); }

int EditorEngine::caretFieldHint() const { return caret_.caretFieldHint(); }

FlowPos EditorEngine::flowPosAtSide(int para, size_t cp_offset, int side) const {
    return caret_.flowPosAtSide(para, cp_offset, side);
}

FlowPos EditorEngine::flowPosAt(int para, size_t cp_offset, int owner_hint) const {
    return caret_.flowPosAt(para, cp_offset, owner_hint);
}

bool EditorEngine::flowToTextPos(FlowPos flow, TextPos* out) const {
    return caret_.flowToTextPos(flow, out);
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

/**
 * 把两份视图摆给它们各自的类：位置解析读 PositionView，命令层读 EditView。
 *
 * 视图里存的是这些容器的地址，而容器都是引擎的成员、不换对象（重建是 assign、重排是就地
 * 改），所以摆一次之后地址一直有效；四个收口点（init / setDocument / 重建之后 / 重排之后）
 * 各摆一次是保守做法。
 */
void EditorEngine::refreshViews() {
    PositionView v;
    v.blocks = &blocks_;
    v.blocks_full = &blocks_full_;
    v.block_flow = &block_flow_;
    v.block_flow_span = &block_flow_span_;
    v.flow = &flow_;
    v.controls = &controls_;
    v.control_node_ids = &control_node_ids_;
    v.owner_index = &owner_index_;
    v.layout = &layout_;
    caret_.setView(v);
    caret_.setCaretWidthPt(cfg_.caret_width_pt);

    EditView e;
    e.host = this;
    e.tree = &tree_;
    e.caret = &caret_;
    e.flow = &flow_;
    e.blocks = &blocks_;
    e.blocks_full = &blocks_full_;
    e.block_flow = &block_flow_;
    e.block_flow_span = &block_flow_span_;
    e.block_src = &block_src_;
    e.controls = &controls_;
    e.control_node_ids = &control_node_ids_;
    e.mode = &mode_;
    e.tab_spaces = &cfg_.tab_spaces;
    e.blocked_edits = &blocked_edits_;
    edit_.setView(e);
}

bool EditorEngine::commitEdit(Transaction tx, int dirty_para, ParaDelta delta) {
    if (!loaded_) return false;  // 没装载过文档：没有树可依，也就没有可提交的事务
    const StepResult r = undo_->commit(std::move(tx));
    if (!r.ok) return false;

    // 树变了：块流、流索引、块起点、来源身份、控件集合一次重建。分两次建就会留下
    //   "块按新树、位置按旧树"的错配，而那种错配只表现为光标落偏一个字。
    //
    // 三条路的取舍：段数没变时只重编被改的那一段；分段只重编被劈开的两段（其余段落的
    //   产物按段号平移）；形状不认识时整树重编。前两条比整树便宜一个数量级 ——
    //   2000 段实测整树 8.5 ms、只编一段 0.10 ms，而它们是每次按键都要付的一笔。
    //
    // 计时先归零：它是"这一轮重建产物花了多少"，而局部路径会连编两段并累加。
    edit_stats_.rebuild_compile_ms = 0.0;
    edit_stats_.rebuild_reject = 0;
    if (!delta.changed) {
        if (!rebuildParagraphFromTree(dirty_para)) rebuildFromTree();
    } else if (delta.at >= 0 && delta.inserted == 1) {
        int reject = 0;
        if (!rebuildAfterSplit(delta.at, &reject)) {
            rebuildFromTree();
            // 退回全量之后再把原因写回去：它记的是"局部路径为什么没接住"，
            //   而全量路径没有这个信息，不该把它清成 0。
            edit_stats_.rebuild_reject = reject;
        }
    } else {
        rebuildFromTree();
    }
    markLayoutDirty(dirty_para);
    return true;
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
    // 恢复的是整个区间，不是只看焦点端：事务里那两份快照本来就是 Range（锚点是不动的
    //   那一端，它决定了 Shift 下一次从哪端继续长）。只摆 focus 的症状是"撤销之后
    //   选中内容回来了，但选区没了"。
    setSelection(restore);
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
    setSelection(restore);
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

bool EditorEngine::caretRectPt(Rect* out) const { return caret_.caretRectPt(out); }

Rect EditorEngine::caretRectPx() const {
    int page = 0;
    Rect r{};
    if (!caret_.caretRectFor(caret(), &page, &r)) return Rect{};
    const float zoom = cfg_.zoom;
    const float off_y = static_cast<float>(frameOffsetOfPage(page));
    Rect px;
    px.x = std::floor(r.x * zoom);
    px.y = std::floor(r.y * zoom + off_y);
    px.w = std::max(1.0f, std::ceil(r.w * zoom));
    px.h = std::max(1.0f, std::ceil(r.h * zoom));
    return px;
}

bool EditorEngine::caretVertical() const { return caret_.caretVertical(); }

bool EditorEngine::caretBlockReversed() const { return caret_.caretBlockReversed(); }

std::vector<EditorEngine::PageRect> EditorEngine::selectionRects() const {
    return caret_.selectionRects();
}

/**
 * 当前视口窗口内的选区矩形（视口没钉过时退化成整篇）。
 */
std::vector<EditorEngine::PageRect> EditorEngine::selectionRectsVisible() const {
    if (vp_count_ <= 0) return selectionRects();
    return selectionRectsIn(vp_first_page_, vp_count_);
}

std::vector<EditorEngine::PageRect> EditorEngine::selectionRectsIn(int first_page, int count) const {
    return caret_.selectionRectsIn(first_page, count);
}

/**
 * 选区矩形（帧像素坐标）。与 caretRectPx 同一套换算：先乘 zoom，再加"窗口首页之下的
 * 那几页"的累加偏移。宽高为 0 的（空行的那个插入点）在这里丢掉 —— 画不出来。
 */
std::vector<Rect> EditorEngine::selectionRectsPx() const {
    return selectionRectsPxIn(0, 0);
}

std::vector<Rect> EditorEngine::selectionRectsPxIn(int first_page, int count) const {
    std::vector<Rect> out;
    const std::vector<PageRect> prs = selectionRectsIn(first_page, count);
    if (prs.empty()) return out;
    const float zoom = cfg_.zoom;
    for (const PageRect& pr : prs) {
        const float off_y = static_cast<float>(frameOffsetOfPage(pr.page));
        Rect px;
        px.x = std::floor(pr.r_pt.x * zoom);
        px.y = std::floor(pr.r_pt.y * zoom + off_y);
        px.w = std::ceil(pr.r_pt.w * zoom);
        px.h = std::ceil(pr.r_pt.h * zoom);
        if (px.w <= 0.0f || px.h <= 0.0f) continue;
        out.push_back(px);
    }
    return out;
}

TextPos EditorEngine::caret() const { return caret_.caret(); }

void EditorEngine::setCaretByText(TextPos p) { caret_.setCaretByText(p); }

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
void EditorEngine::projectCaret(int para, size_t offset, int side, bool extend, int line_aff) {
    const int plan_before = bracketPlanId();
    caret_.placeCaret(para, offset, side, extend, line_aff);
    relayoutIfBracketPlanChanged(plan_before);
    caret_.placeCaret(para, offset, side, extend, line_aff);
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
 * 拖选的移动端：只搬 focus，anchor 留在按下点。
 *
 * projectCaret 的 extend 形参本来就有（键盘的 Shift 扩展族走它），拖选要的是同一个动作，
 * 只是位置来自命中测试而不是方向键。所以这里没有第二套几何：落点、吸附、侧别全部复用。
 */
void EditorEngine::setFocusAtSide(int para, size_t offset, int side) {
    projectCaret(para, offset, side, /*extend=*/true);
}

/**
 * 带行别的定位与拖选（点击落点）。
 *
 * 与上一对只差行别：折行处那个位置属于两行，点下一行行首时必须说清是哪一行，
 * 否则光标画到上一行行尾（见 caretLineAffinity）。
 */
void EditorEngine::setCaretAtSideLine(int para, size_t offset, int side, int line_aff) {
    projectCaret(para, offset, side, /*extend=*/false, line_aff);
}

void EditorEngine::setFocusAtSideLine(int para, size_t offset, int side, int line_aff) {
    projectCaret(para, offset, side, /*extend=*/true, line_aff);
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
    const size_t at = caret_.snapToCluster(p, offset);
    setCaretAtSide(p, at, caret_.sideFromHint(p, at, field));
}

bool EditorEngine::setCaretAtFlow(FlowPos flow) { return caret_.setCaretAtFlow(flow); }

bool EditorEngine::setSelection(Range r) { return caret_.setSelection(r); }

void EditorEngine::clearSelection() { caret_.clearSelection(); }

bool EditorEngine::selectAll() { return caret_.selectAll(); }

EditCheck EditorEngine::policyAt(TextPos p) const { return edit_.policyAt(p); }

EditCheck EditorEngine::policyErase(TextPos a, TextPos b) const {
    return edit_.policyErase(a, b);
}

bool EditorEngine::moveCaretBy(int dcp, bool extend) {
    // 移动本身在解析器里；"换占位集合要重排"归引擎 —— 聚焦档下显示哪些括号由光标位置定。
    const int plan_before = bracketPlanId();
    if (!caret_.moveCaretBy(dcp, extend)) return false;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::moveCaretToLineStart(bool extend) {
    const int plan_before = bracketPlanId();
    if (!caret_.moveCaretToLineStart(extend)) return false;
    relayoutIfBracketPlanChanged(plan_before);
    // 行首/行尾是 (偏移, 侧别) 投影出来的，投影要吸附簇 ⇒ 重排换了簇表就得再投影一次。
    caret_.reprojectCaret(extend);
    return true;
}

bool EditorEngine::moveCaretToLineEnd(bool extend) {
    const int plan_before = bracketPlanId();
    if (!caret_.moveCaretToLineEnd(extend)) return false;
    relayoutIfBracketPlanChanged(plan_before);
    caret_.reprojectCaret(extend);
    return true;
}

bool EditorEngine::moveCaretToDocStart(bool extend) {
    const int plan_before = bracketPlanId();
    if (!caret_.moveCaretToDocStart(extend)) return false;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::moveCaretToDocEnd(bool extend) {
    const int plan_before = bracketPlanId();
    if (!caret_.moveCaretToDocEnd(extend)) return false;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::moveCaretVertical(int dir, bool extend) {
    const int plan_before = bracketPlanId();
    if (!caret_.moveCaretVertical(dir, extend)) return false;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

void EditorEngine::showCaret(bool on) { caret_visible_ = on; }

bool EditorEngine::insertCodepoints(int para, size_t offset, const std::vector<uint32_t>& cps) {
    return edit_.insertCodepoints(para, offset, cps);
}

bool EditorEngine::insertCodepoint(int para, size_t offset, uint32_t cp) {
    return edit_.insertCodepoint(para, offset, cp);
}

bool EditorEngine::insertTextUtf8(int para, size_t offset, const std::string& utf8) {
    return edit_.insertTextUtf8(para, offset, utf8);
}

bool EditorEngine::typeTextUtf8(const std::string& utf8) {
    return edit_.typeTextUtf8(utf8);
}

bool EditorEngine::backspace() { return edit_.backspace(); }

bool EditorEngine::deleteForward() { return edit_.deleteForward(); }

bool EditorEngine::splitParagraph() { return edit_.splitParagraph(); }

bool EditorEngine::mergeWithPreviousParagraph() {
    return edit_.mergeWithPreviousParagraph();
}

bool EditorEngine::mergeWithNextParagraph() {
    return edit_.mergeWithNextParagraph();
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

    // 切模式会改变占位集合本身（只读模式一个都不占，文档与表单都要占），
    //   所以这里可能也要重排（Focused 档下还有"光标被夹进最近字段"这一层）。
    relayoutIfBracketPlanChanged(plan_before);

    // 切模式只影响帧上的标记（画 / 不画），底图一个字都不变 ——
    //   所以不动页指纹，只要求下一次整帧重贴。
    //   如果哪天标记改回"画进底图"，这里必须换成重算页指纹 ——
    //     否则切到只读模式后括号会残留，而"内容没变 → 跳过重画"会让它一直在。
    //     对应的断言是单测里"切到只读模式后一个像素都不剩"那一条。
    frame_needs_full_refresh_ = true;
}

int EditorEngine::controlAtPt(float x_pt_doc, float y_pt_doc) const {
    TextPos p{};
    if (!hitTestPt(x_pt_doc, y_pt_doc, &p)) return -1;
    // include_end = false：点在内容上才算"点进了这个控件"。
    // 点在字段末尾之后一位、视觉上已经在框外，就不该算。
    return controls_.innermostAt(p, /*include_end=*/false);
}

bool EditorEngine::removeControl(int index, bool keep_content, FlowPos caret_before) {
    // "有没有可编辑的东西"是引擎的装载状态，命令层只管"编辑怎么做" —— 这一条留在门面。
    if (!loaded_) return false;
    return edit_.removeControl(index, keep_content, caret_before);
}

bool EditorEngine::setControlProp(int index, ControlProp prop, bool value) {
    if (!loaded_) return false;  // 没装载过文档：没有控件可改
    const EditCommand::PropResult r = edit_.setControlProp(index, prop, value);
    if (r == EditCommand::PropResult::Failed) return false;
    // hidden 改的是呈现态的流结构（整棵摘除），影响面不可定位 —— 整篇排版作废。
    if (r == EditCommand::PropResult::Structural) markAllLayoutDirty();
    return true;
}

bool EditorEngine::removeControlAndRender(int index, bool keep_content) {
    if (!removeControl(index, keep_content)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::setControlPropAndRender(int index, ControlProp prop, bool value) {
    if (!loaded_) return false;
    const EditCommand::PropResult r = edit_.setControlProp(index, prop, value);
    if (r == EditCommand::PropResult::Failed) return false;
    if (r == EditCommand::PropResult::Structural) markAllLayoutDirty();
    // 幂等（值没变）时状态与呈现一个字都没动 —— 跟着白跑一次重排重绘没有意义，
    //   也会把探针的"一次操作重画了多少"记账弄脏。
    if (r == EditCommand::PropResult::Unchanged) return true;
    relayout();
    repaintAfterEdit();
    return true;
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
        //   左边缘取"lo 那个簇自己的起点"（对）；
        //   右边缘若也取"hi 那个簇的起点"，拿到的是区间内最后一个字的左边缘，
        //   会整整少掉一个字的宽度（症状：框比字窄一格，看着像没框住）。
        //   所以右边缘取"最后一个 cp < hi 的簇的右边缘"。
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

bool EditorEngine::indent() { return edit_.indent(); }

bool EditorEngine::outdent() { return edit_.outdent(); }

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
    // 文档模式与只读模式都不消费 Tab：后者要放行给宿主（吞掉会把键盘用户困在编辑器里），
    //   前者的 Tab 该是制表符 —— 但那一步还没落地（排版层没有制表位），所以文档模式现在
    //   由宿主当缩进处理，见 EngineConfig::tab_spaces。
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

bool EditorEngine::moveCaretAndRender(int dcp, bool extend) {
    if (!moveCaretBy(dcp, extend)) return false;
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

bool EditorEngine::moveCaretToLineStartAndRender(bool extend) {
    if (!moveCaretToLineStart(extend)) return false;
    repaint();  // 不重排 —— 和 moveCaretAndRender 同一条增量路径（也不点亮光标，同上）
    return true;
}

bool EditorEngine::moveCaretToLineEndAndRender(bool extend) {
    if (!moveCaretToLineEnd(extend)) return false;
    repaint();  // 同 moveCaretAndRender：不重排、不点亮光标
    return true;
}

bool EditorEngine::moveCaretToDocStartAndRender(bool extend) {
    if (!moveCaretToDocStart(extend)) return false;
    repaint();
    return true;
}

bool EditorEngine::moveCaretToDocEndAndRender(bool extend) {
    if (!moveCaretToDocEnd(extend)) return false;
    repaint();
    return true;
}

bool EditorEngine::moveCaretVerticalAndRender(int dir, bool extend) {
    if (!moveCaretVertical(dir, extend)) return false;
    repaint();
    return true;
}

bool EditorEngine::selectAllAndRender() {
    if (!selectAll()) return false;
    repaint();
    return true;
}

bool EditorEngine::setSelectionAndRender(Range r) {
    if (!setSelection(r)) return false;
    repaint();
    return true;
}

/**
 * 命中测试：几何的反向半边 —— 由落点反查 (段, 偏移) + 侧别 + 归属控件。
 *
 * 收的是文档坐标：命中测试问的是"文档里的哪个位置"，与帧怎么摆无关。帧按槽号装页
 *   （页号 mod 槽数），拿窗口坐标或槽号去反查都会让页归属整体错位。
 */

bool EditorEngine::hitTestPt(float x_pt_doc, float y_pt_doc, TextPos* out, int* out_owner,
                             int* out_side, int* out_line_aff) const {
    if (out_owner) *out_owner = -1;
    if (out_side) *out_side = 0;
    if (out_line_aff) *out_line_aff = 0;
    if (!out || layout_.lines.empty()) return false;

    const float page_w = cfg_.page.width_pt;
    const float zoom = cfg_.zoom;
    const int page_px_h = pagePxHeight();
    const int step_px = page_px_h + pageGapPx();

    int page = 0;
    float local_y = y_pt_doc;
    if (step_px > 0 && zoom > 0.0f) {
        // 页号按宿主摆页用的那个像素步距折，不按磅步距：宿主把页摆在
        //   pagePxHeight + pageGapPx 这些整数像素上，而磅步距是未取整的页高 + 页间隙
        //   （本机 855.89 磅对 1142 px / zoom = 856.5 磅）。用磅步距折，误差随页号累积
        //   —— 2000 段文档第 160 页上实测偏下约 130 px（0.6 磅/页 x 160 页），
        //   表象就是"最后几页点哪都偏下"。乘 zoom 折成设备像素再取页号，两边就一致。
        const float y_px = y_pt_doc * zoom;
        page = std::max(0, static_cast<int>(std::floor(y_px / static_cast<float>(step_px))));
        const float local_px = y_px - static_cast<float>(page) * static_cast<float>(step_px);
        // 落在页间空隙里，或者横向出了页外 —— 没有"这一页的哪一行"可言
        if (local_px < 0.0f || local_px > static_cast<float>(page_px_h)) return false;
        if (x_pt_doc < 0.0f || x_pt_doc > page_w) return false;
        local_y = local_px / zoom;
    }

    // 块轴：找最近的一行。
    //   连页下按文档像素比：一行可以跨条带，它跨过去的那一段落在下一条带里，
    //   而那个位置的页号已经不是行盒的页号 —— 按页号筛会把这一点判给下一条带上
    //   离得最近的另一行（每跨一条带就有一条这样的窄带点不中）。
    //   比的是"渲染位置"（页号 x 像素步距 + 页内磅 x zoom），与宿主把页摆在哪、
    //   与帧里的槽号都无关 —— 与绘制用的是同一套映射。
    const bool cont = cfg_.page.continuous;
    const LineBox* best = nullptr;
    float best_d = 1e30f;
    for (const LineBox& lb : layout_.lines) {
        float bp = 0.0f;
        float lo = 0.0f;
        float hi = 0.0f;
        if (lb.vertical) {
            bp = x_pt_doc;
            lo = lb.block_lo_pt;
            hi = lb.block_hi_pt;
        } else if (cont) {
            const float base = static_cast<float>(lb.page) * static_cast<float>(step_px);
            bp = y_pt_doc * zoom;
            lo = base + lb.block_lo_pt * zoom;
            hi = base + lb.block_hi_pt * zoom;
        } else {
            if (lb.page != page) continue;
            bp = local_y;
            lo = lb.block_lo_pt;
            hi = lb.block_hi_pt;
        }
        const float d = (bp < lo) ? (lo - bp) : (bp > hi ? bp - hi : 0.0f);
        if (d < best_d) {
            best_d = d;
            best = &lb;
        }
    }
    if (!best) return false;

    // 行内轴：找最近的插入点。势力范围与簇边界的口径在 hitTestInLine 里（上下键也走它）。
    //   竖排的行内轴是 y，页内位置要按"选中那一行自己的页号"折 —— 跨条带时它与
    //   点击位置的页号可以差一条带。
    const float ip = best->vertical
                         ? (cont ? (y_pt_doc * zoom -
                                    static_cast<float>(best->page) * static_cast<float>(step_px)) /
                                       zoom
                                 : local_y)
                         : x_pt_doc;
    uint32_t cp = 0;
    int side = 0;
    caret_.hitTestInLine(*best, ip, &cp, &side);

    TextPos p{best->paragraph, cp};
    // 行别：命中的这一行是不是"以这个偏移开头"。是的话，同一个位置也是上一行的末尾
    //   （折行处一行有两个候选），落点必须贴着这一行 —— 否则点行首会画到上一行行尾。
    //   段首那一行不算：它前面没有上一行，贴哪儿都一样。
    const int line_aff = (cp == best->first_cp && best->first_cp != 0) ? 1 : 0;
    // 侧别是落点的完整身份（同一个偏移上有几个位置就看它）；归属只是它的一个投影，
    //   给宿主显示"当前在哪个字段"用。两者一起交出去：宿主搬运的是侧别。
    if (out_side) *out_side = side;
    if (out_line_aff) *out_line_aff = line_aff;
    int owner = -1;
    if (controls_.size() > 0)
        owner = caret_.hintFromRel(best->paragraph, caret_.relAt(best->paragraph, cp, side));
    // 命中即落点：光标位置不受控件约束，所以这里不再做"就近吸附到最近字段"。
    //   吸附会把"点字段外正文"改写成"停在字段端点"，于是画出来的光标不在点的地方，
    //   表象就是"点了没反应"。归属照实返回，编辑侧自己会按策略拒绝。
    if (out_owner) *out_owner = owner;
    *out = p;
    return true;
}

}  // namespace heditor
