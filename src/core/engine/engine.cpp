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
    // 页度量改了整篇的断行位置都会变，没有可定位的影响面 → 整篇标脏。
    const bool page_changed = (cfg.page != cfg_.page);
    cfg_ = cfg;
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
}

void EditorEngine::setCaretAndSelectionStyle(const Color& caret, float caret_width_pt,
                                             const Color& selection) {
    cfg_.caret_color = caret;
    cfg_.caret_width_pt = caret_width_pt < kMinCaretWidthPt ? kMinCaretWidthPt : caret_width_pt;
    cfg_.selection_color = selection;
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
    const int rebase_para = caretPara();
    const int rebase_rel = caretRel();
    const int rebase_anchor_para = paraForFlow(selection_.anchor);
    const int rebase_anchor_rel = relForFlow(selection_.anchor);
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
    setCaretRel(rebase_para, rebase_rel);
    selection_.anchor = flowForRel(rebase_anchor_para, rebase_anchor_rel);

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

/**
 * 光标侧别（派生）= focus 的侧别，与 caret() 同源，不可能不同步。
 */
int EditorEngine::caretSide() const { return sideForFlow(selection_.focus); }

int EditorEngine::caretFieldHint() const {
    if (blocks_.empty()) return -1;
    return hintFromRel(caretPara(), caretRel());
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

bool EditorEngine::commitOnTree(Transaction tx, int dirty_para, ParaDelta delta) {
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
    if (!commitOnTree(std::move(tx), a.para, ParaDelta{})) return false;
    // 落点 = 删除区间的起点，也就是被删掉那段文本所在的位置。这个流位置本来就算出来了，
    //   直接摆过去即可 —— 只留一个码点偏移、再由"最内层含端点"猜回侧别的话，
    //   删完一个字光标会落到相邻括号的另一侧（"跳了一格"）。
    setCaretAtFlow(from);
    return true;
}

namespace {

/**
 * 一步删除的起点（把整批删除步骤从后往前排时用）。
 * 只认两种删除步骤 —— 这个序列里不会有别的。
 */
FlowPos stepStartFlow(const Step& s) {
    if (const auto* d = std::get_if<DeleteRange>(&s)) return d->begin;
    if (const auto* r = std::get_if<RemoveParaMark>(&s)) return r->at;
    return -1;
}

}  // namespace

std::vector<Step> EditorEngine::eraseFlowSteps(FlowPos from, FlowPos to) const {
    std::vector<Step> steps;
    if (from < 0 || to <= from) return steps;
    // 优先"整段并成一步"：区间里全是文本与段落标记时成立，而整篇删除与跨段删除都是这种。
    //   按节点切分要发几千步，每一步在提交时都要重建一次流索引（O(节点数)）——
    //   2000 段整篇删除实测几秒，其中一多半在这一项上。
    if (flow_.has_value()) {
        steps = makeDeleteRangeOneStep(tree_, *flow_, from, to);
        if (!steps.empty()) return steps;
    }
    // 文本那一条：按"所属文本节点"切分，落在段落标记、容器边界与原子上的位置不产生步骤。
    steps = makeDeleteTextSteps(tree_, from, to);
    // 段落标记那一条：标记各占一个流位置，要么整覆盖要么不覆盖。
    //   它不能用 makeDeleteRange 顶替：那条按结构取区间，会把端点向外扩到控件边界
    //   （见 makeDeleteTextSteps 的说明），而这里要的正是"只删掉这一位"。
    //
    //   位置由块表直接给出，不逐位问 posAt：标记夹在两段之间，位置就是后一段的流起点减一。
    //   逐位问是 O(位置数 x 扇出)（posAt 要从根下降到最内层，每层线性扫孩子），
    //   2000 段文档整篇删除是亿级 —— 实测那一笔占了删除总耗时的几秒。
    if (flow_.has_value()) {
        for (size_t k = 1; k < block_flow_.size(); ++k) {
            const FlowPos p = block_flow_[k] - 1;
            if (p < from || p >= to) continue;
            const ParaMarkBuild mb = makeRemoveParaMark(tree_, *flow_, p);
            if (mb.ok) steps.push_back(mb.step);
        }
    }
    // 从后往前：删右边的不会移动左边的坐标，于是每步的坐标都是它自己那一刻的坐标。
    std::sort(steps.begin(), steps.end(), [](const Step& a, const Step& b) {
        return stepStartFlow(a) > stepStartFlow(b);
    });
    // 安全阀：两批步骤加起来必须刚好覆盖整个区间。差一位都说明区间里有东西是这套可编辑
    //   集合处理不了的（节边界、控件边界），此时宁可整条编辑被拒，也不要删掉半截区间 ——
    //   半截区间的症状是"选中的字还剩一个没删掉"，很难归因。
    FlowPos covered = 0;
    for (const Step& s : steps) {
        if (const auto* d = std::get_if<DeleteRange>(&s)) {
            covered += d->end - d->begin;
        } else if (std::get_if<RemoveParaMark>(&s) != nullptr) {
            covered += 1;
        }
    }
    if (covered != to - from) return {};
    return steps;
}

EditorEngine::SelectionErase EditorEngine::planEraseSelection() {
    SelectionErase plan;
    if (!hasSelection() || !flow_.has_value() || block_flow_.empty()) return plan;
    plan.before = selection_;
    plan.at = selection_.begin();
    const FlowPos from = selection_.begin();
    const FlowPos to = selection_.end();
    if (from >= to) return plan;
    // 判定与实际删除同一把尺子：判的是整个区间能不能编辑，删的是同一个区间。
    //   跨段的区间在这里第一次被放行 —— 档 1 的 policyErase 只管同段。
    if (!formDegraded() && !canEditRange(tree_, mode_, Range{from, to}).allowed) {
        ++blocked_edits_;
        plan.blocked = true;
        return plan;
    }
    plan.steps = eraseFlowSteps(from, to);
    if (plan.steps.empty()) {
        ++blocked_edits_;
        plan.blocked = true;
    }
    return plan;
}

bool EditorEngine::commitErase(const SelectionErase& plan, const char* label) {
    if (plan.steps.empty()) return false;
    // 段数变了就得禁掉后缀复用：跨段删除删掉段落标记，后一段的起点整体前移，
    //   "第 k 段还是同一段"这件事不再成立。同段内删除照旧只标脏那一段。
    const bool cross = paraForFlow(plan.at) != paraForFlow(plan.before.end() - 1);
    Transaction tx;
    tx.label = label;
    tx.selection_before = plan.before;
    tx.selection_after = Range{plan.at, plan.at};
    tx.steps = plan.steps;
    if (!commitOnTree(std::move(tx), paraForFlow(plan.at),
                      cross ? ParaDelta{true} : ParaDelta{})) {
        ++blocked_edits_;
        return false;
    }
    setCaretAtFlow(plan.at);
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
bool EditorEngine::replaceSelectionWithText(const std::vector<uint32_t>& cps) {
    if (cps.empty()) return false;
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked || plan.steps.empty()) return false;

    InsertText s;
    s.at = plan.at;
    s.cps = cps;
    s.marks = marksAtFlow(s.at, &s.styleRef);

    const FlowPos after = plan.at + static_cast<FlowPos>(cps.size());
    Transaction tx;
    tx.label = "输入";
    tx.selection_before = plan.before;
    tx.selection_after = Range{after, after};
    tx.steps = plan.steps;
    tx.steps.push_back(Step{s});

    const bool cross = paraForFlow(plan.at) != paraForFlow(plan.before.end() - 1);
    if (!commitOnTree(std::move(tx), paraForFlow(plan.at),
                      cross ? ParaDelta{true} : ParaDelta{})) {
        ++blocked_edits_;
        return false;
    }
    setCaretAtFlow(after);
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
    const float off_y = static_cast<float>(frameOffsetOfPage(page));
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
std::vector<EditorEngine::PageRect> EditorEngine::selectionRects() const {
    return selectionRectsIn(0, 0);
}

/**
 * 当前视口窗口内的选区矩形（视口没钉过时退化成整篇）。
 */
std::vector<EditorEngine::PageRect> EditorEngine::selectionRectsVisible() const {
    if (vp_count_ <= 0) return selectionRects();
    return selectionRectsIn(vp_first_page_, vp_count_);
}

std::vector<EditorEngine::PageRect> EditorEngine::selectionRectsIn(int first_page, int count) const {
    std::vector<PageRect> out;
    if (selection_.empty() || !flow_.has_value() || layout_.lines.empty() || block_flow_.empty())
        return out;

    const FlowPos lo = selection_.begin();
    const FlowPos hi = selection_.end();
    const int pa_lo = paraForFlow(lo);
    const int pa_hi = paraForFlow(hi);
    const int n_para = static_cast<int>(block_flow_.size());

    // 行下标范围。行数组按页单调，所以限页时两端都能二分 ——
    //   不限页时遍历全篇是为了挑出真正落进选区的那几行，那是"文档行数"这个量级；
    //   而呈现每拍都走这一趟，全选时这笔开销会盖过真正要画的字。
    const int n_lines = static_cast<int>(layout_.lines.size());
    int k_begin = 0;
    int k_end = n_lines - 1;  // 闭区间
    if (count > 0 && n_lines > 0) {
        const int last_page = first_page + count - 1;
        linesOnPage(first_page, &k_begin, &k_end);
        int hi_lo = 0;
        int hi_hi = 0;
        linesOnPage(last_page, &hi_lo, &hi_hi);
        // 末页上没有行时 hi_hi == hi_lo - 1：这时窗口内的行止于末页之前。
        k_end = hi_hi;
        // k_begin 可能被夹到"页号更大的第一行"上，往前一页兜一下不会错：
        //   多算的行会被下面的段区间判断滤掉，少算的行却会漏画。
        if (k_begin > 0) --k_begin;
    }

    for (int k = k_begin; k <= k_end && k < n_lines; ++k) {
        if (k < 0) continue;
        const LineBox& lb = layout_.lines[static_cast<size_t>(k)];
        // 页范围必须逐行再判一次：上面的二分只把范围收窄，端点会为了不漏而往外兜，
        //   而帧坐标是"相对窗口首页"的 —— 窗口上方那一页的矩形算出来是负 y，
        //   正好压在帧顶部，会被当成窗口内的选区画出来。
        if (count > 0 && (lb.page < first_page || lb.page >= first_page + count)) continue;
        const int para = lb.paragraph;
        if (para < pa_lo || para > pa_hi) continue;
        if (para < 0 || para >= n_para) continue;

        // 选区在本段的段内流位置区间（整段被选中时就是 [0, 本段占的位数]）。
        const FlowPos base = block_flow_[static_cast<size_t>(para)];
        const FlowPos seg_lo = std::max(lo, base);
        const FlowPos seg_hi = std::min(hi, base + block_flow_span_[static_cast<size_t>(para)]);
        if (seg_hi <= seg_lo) continue;

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
        const float a1 = lb.inline_origin_pt + lb.offsetAtSide(l_hi, side_hi);
        Rect r;
        if (!lb.vertical) {
            r.x = std::min(a0, a1);
            r.y = lb.block_lo_pt;
            r.w = std::abs(a1 - a0);
            r.h = lb.block_hi_pt - lb.block_lo_pt;
        } else {
            r.x = lb.block_lo_pt;
            r.y = std::min(a0, a1);
            r.w = lb.block_hi_pt - lb.block_lo_pt;
            r.h = std::abs(a1 - a0);
        }
        out.push_back(PageRect{lb.page, r});
    }
    return out;
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

/**
 * 段内相对流位置 → 全局流位置（夹到本段跨度内）。
 *
 * 夹取只做范围收缩，不做簇吸附（簇表要排版之后才有，见 relayout 里的说明）。
 * 上界取 block_flow_span_（流索引给的权威段长）而不是显示名单筛过的那张对象表的长度：
 * 前者是树的口径，后者随聚焦档变化，拿它当上界会在换档时把光标往回拉。
 */
FlowPos EditorEngine::flowForRel(int para, int rel) const {
    if (block_flow_.empty()) return 0;
    if (para < 0) para = 0;
    if (para >= static_cast<int>(block_flow_.size()))
        para = static_cast<int>(block_flow_.size()) - 1;
    const int span = block_flow_span_[static_cast<size_t>(para)];
    if (rel > span) rel = span;
    if (rel < 0) rel = 0;
    return block_flow_[static_cast<size_t>(para)] + rel;
}

/**
 * 唯一写入点：段内流位置。其余入口都只是把它换算成全局流位置再调这里。
 */
void EditorEngine::setCaretRel(int para, int rel, bool collapse) {
    selection_.focus = flowForRel(para, rel);
    if (collapse) {
        selection_.anchor = selection_.focus;
        // 列位是一串连续上下键之间的事：任何别的定位都让它从头取。
        vertical_goal_valid_ = false;
    }
}

/**
 * 任意流位置落在哪一块（派生）。block_flow_ 单调，二分取最后一个起点不超过它的块。
 * caretPara 是它在 focus 上的特例 —— 选区另一端的段号也走这里。
 */
int EditorEngine::paraForFlow(FlowPos f) const {
    if (block_flow_.empty()) return 0;
    const auto it = std::upper_bound(block_flow_.begin(), block_flow_.end(), f);
    const size_t i = static_cast<size_t>(it - block_flow_.begin());
    return i == 0 ? 0 : static_cast<int>(i - 1);
}

/**
 * 任意流位置的段内流位置（派生）= 它减本段起点。
 */
int EditorEngine::relForFlow(FlowPos f) const {
    if (block_flow_.empty()) return 0;
    const int para = paraForFlow(f);
    const int rel = f - block_flow_[static_cast<size_t>(para)];
    return rel < 0 ? 0 : rel;
}

/**
 * 任意流位置的侧别（派生）：段内流位置减去"这个偏移在流里占的起点"就是跨过的边界枚数。
 * 没有"存着的侧别"可以失配，所以既不需要失效标记，也不需要拿边界枚数去卡它。
 */
int EditorEngine::sideForFlow(FlowPos f) const {
    const int para = paraForFlow(f);
    const int rel = relForFlow(f);
    int off = cpOffsetInBlock(para, rel);
    if (off < 0) off = 0;
    const BracketCounts bc = bracketCountsAt(para, off);
    const int side = rel - off - bc.before;
    return side < 0 ? 0 : side;
}

/**
 * 光标那一段（派生）：focus 落在哪一块。
 *
 * 不能借 flowToTextPos：那个入口的职责是"把流位置翻成文本坐标"，段间的位置会被归到相邻段
 * 端点；这里要的是"focus 归属哪一块"，段尾那个位置（段落标记所在位）必须仍算本段。
 */
int EditorEngine::caretPara() const { return paraForFlow(selection_.focus); }

/**
 * 段内流位置（派生）= focus 减本段起点。
 */
int EditorEngine::caretRel() const { return relForFlow(selection_.focus); }

/**
 * 光标位置（派生）：段号 + 段内码点偏移，由 focus 投影出来 —— 不再是独立状态。
 */
TextPos EditorEngine::caret() const {
    int para = caretPara();
    if (!blocks_.empty() && para >= static_cast<int>(blocks_.size()))
        para = static_cast<int>(blocks_.size()) - 1;
    int off = cpOffsetInBlock(para, caretRel());
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
void EditorEngine::projectCaret(int para, size_t offset, int side, bool extend) {
    if (blocks_.empty()) {
        setCaretRel(0, 0, !extend);
        return;
    }
    if (para < 0) para = 0;
    if (para >= static_cast<int>(blocks_.size())) para = static_cast<int>(blocks_.size()) - 1;
    const auto place = [&]() {
        const size_t at = snapToCluster(para, offset);
        setCaretRel(para, relAt(para, at, (side < 0) ? defaultSideAt(para, at) : side), !extend);
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
 * 拖选的移动端：只搬 focus，anchor 留在按下点。
 *
 * projectCaret 的 extend 形参本来就有（键盘的 Shift 扩展族走它），拖选要的是同一个动作，
 * 只是位置来自命中测试而不是方向键。所以这里没有第二套几何：落点、吸附、侧别全部复用。
 */
void EditorEngine::setFocusAtSide(int para, size_t offset, int side) {
    projectCaret(para, offset, side, /*extend=*/true);
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
 * 程序化设选区（宿主恢复、探针摆位）。两端各自夹到文档流的合法范围内 —— 越界的那
 * 一端收到能到的最远处，不会让一个悬空端点把后面的几何全带偏。
 */
bool EditorEngine::setSelection(Range r) {
    if (!flow_.has_value() || block_flow_.empty()) return false;
    const size_t last = block_flow_.size() - 1;
    const FlowPos hi = block_flow_[last] + block_flow_span_[last];
    const auto clamp_flow = [hi](FlowPos v) -> FlowPos {
        if (v < 0) return 0;
        return v > hi ? hi : v;
    };
    selection_.anchor = clamp_flow(r.anchor);
    selection_.focus = clamp_flow(r.focus);
    vertical_goal_valid_ = false;
    return true;
}

/**
 * 取消选区：anchor 收到 focus，焦点端不动 —— 光标留在原地。
 */
void EditorEngine::clearSelection() {
    selection_.anchor = selection_.focus;
    vertical_goal_valid_ = false;
}

/**
 * 全选：文档首到文档末。
 *
 * 末端是"最后一块的段内末端"（块起点 + 本块占的位数），而不是下一块之前那个段落标记
 * 所在的位置 —— 段落标记不属于任何一段的内容，够不到它。
 */
bool EditorEngine::selectAll() {
    if (!flow_.has_value() || block_flow_.empty()) return false;
    const size_t last = block_flow_.size() - 1;
    selection_.anchor = block_flow_[0];
    selection_.focus = block_flow_[last] + block_flow_span_[last];
    vertical_goal_valid_ = false;
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

bool EditorEngine::moveCaretBy(int dcp, bool extend) {
    if (blocks_.empty() || dcp == 0) return false;
    const int plan_before = bracketPlanId();

    int para = caretPara();
    if (para >= static_cast<int>(blocks_.size())) para = static_cast<int>(blocks_.size()) - 1;
    int rel = caretRel();
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

    if (para == caretPara() && rel == caretRel()) return false;
    setCaretRel(para, rel, !extend);
    // 跨字段移动在聚焦档下会换占位集合、整行重排 —— 光标在 rel 上，重排不改变它。
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::moveCaretToLineStart(bool extend) {
    const LineBox* lb = findLineFor(caret());
    if (!lb) return false;
    // 行首就是行首，不再往字段边界上收：光标位置不受控件约束（见 setCaret）。
    const TextPos p{lb->paragraph, lb->first_cp};
    if (p == caret()) return false;
    projectCaret(p.para, p.offset, -1, extend);
    return true;
}

bool EditorEngine::moveCaretToLineEnd(bool extend) {
    const LineBox* lb = findLineFor(caret());
    if (!lb) return false;
    const TextPos p{lb->paragraph, lb->last_cp};
    if (p == caret()) return false;
    projectCaret(p.para, p.offset, -1, extend);
    return true;
}

/**
 * 到文档首 / 文档末（Ctrl+Home / Ctrl+End）。
 *
 * 落点用流位置给（块起点 / 块起点 + 本块占的位数），不走文本坐标 —— 段末那个位置在
 * 文本坐标里与"下一段段首"是同一个偏移，翻回来会跑到下一段去。
 */
bool EditorEngine::moveCaretToDocStart(bool extend) {
    if (block_flow_.empty()) return false;
    if (selection_.focus == block_flow_[0]) return false;
    const int plan_before = bracketPlanId();
    setCaretRel(0, 0, !extend);
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::moveCaretToDocEnd(bool extend) {
    if (block_flow_.empty()) return false;
    const size_t last = block_flow_.size() - 1;
    if (selection_.focus == block_flow_[last] + block_flow_span_[last]) return false;
    const int plan_before = bracketPlanId();
    setCaretRel(static_cast<int>(last), block_flow_span_[last], !extend);
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

/**
 * 光标沿行内轴的页面坐标 —— 上下键的列位就是它。找不到行盒返回 0。
 */
float EditorEngine::caretInlineAxisPt() const {
    const TextPos p = caret();
    const LineBox* lb = findLineFor(p);
    if (!lb) return 0.0f;
    const float off = lb->offsetAtSide(static_cast<uint32_t>(p.offset), caretSide());
    return lb->inline_origin_pt + off;
}

/**
 * 上下键：沿块轴走到相邻行，列位沿行内轴保持。
 *
 * 实现是"当前行盒在全局行数组里的下标 ±1 → 在目标行上按列位做一次行内命中"，不新增
 * 排版接口 —— 行盒数组本来就是按块轴排好的。到文档首/末时停在原地（返回假），但列位
 * 留着，于是"按到头上、再按回来"落的是同一个列位。
 */
bool EditorEngine::moveCaretVertical(int dir, bool extend) {
    if (layout_.lines.empty() || dir == 0) return false;
    const int n = static_cast<int>(layout_.lines.size());
    const LineBox* cur_lb = findLineFor(caret());
    if (!cur_lb) return false;
    int cur_i = -1;
    for (int i = 0; i < n; ++i) {
        if (&layout_.lines[static_cast<size_t>(i)] == cur_lb) {
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

    const LineBox& dst = layout_.lines[static_cast<size_t>(dst_i)];
    uint32_t cp = 0;
    int side = 0;
    hitTestInLine(dst, goal, &cp, &side);
    const int rel = relAt(dst.paragraph, cp, side);
    if (dst.paragraph == caretPara() && rel == caretRel()) return false;

    const int plan_before = bracketPlanId();
    setCaretRel(dst.paragraph, rel, !extend);
    // collapse 那一路会把列位清掉（它就是"别的定位"），这里再摆回去 —— 连续按要同一列。
    vertical_goal_pt_ = goal;
    vertical_goal_valid_ = true;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

/**
 * 在某一行里按"沿行内轴的页面坐标"（横排 x / 竖排 y）命中插入点。
 *
 * 每个插入点的势力范围是它到相邻插入点的中点；落在某个簇的后半段时，落点是下一个簇的
 * 首码点 —— 这就是"字素不可切分"的正确语义（点 e+U+0301 的右半边，光标跑到它后面，
 * 而不是钻进它内部）。hitTestPt 与上下键共用它，免得两份实现分头长出不同的势力范围。
 */
void EditorEngine::hitTestInLine(const LineBox& lb, float inline_page_pt, uint32_t* cp_out,
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
    if (cp_out) *cp_out = cp;
    if (side_out) *side_out = (hit_i > k0) ? static_cast<int>(hit_i - k0) : 0;
}

void EditorEngine::showCaret(bool on) { caret_visible_ = on; }

/**
 * 编辑：插入与删除，先过落点判定（policyAt / policyErase），再按字素簇吸附。
 */

bool EditorEngine::insertCodepoints(int para, size_t offset, const std::vector<uint32_t>& cps) {
    if (cps.empty()) return false;
    if (para < 0 || para >= static_cast<int>(blocks_.size())) return false;
    // 落点就是光标（打字与缩进都走这里）而选区非空：这一次输入是"替换选中内容"。
    //   落到别处的插入（程序化插入）不动选区 —— 那种调用的坐标是按调用时的文档算出来
    //   的，删掉一段会让它整批一起过期。
    if (hasSelection()) {
        const TextPos here = caret();
        if (para == here.para && offset == here.offset) return replaceSelectionWithText(cps);
    }
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
    if (!commitOnTree(std::move(tx), para, ParaDelta{})) {
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
    // 选区非空时退格是"删掉选区"，不再往前多删一个字 —— 这是最容易抄错的一条：
    //   抄成"先折叠选区再退格"的症状是用户删掉三个字，却连带把选区前面那个字也吃了。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (!plan.steps.empty()) return commitErase(plan, "退格");
    TextPos p = caret();
    if (p.para < 0 || p.para >= static_cast<int>(blocks_.size())) p.para = 0;
    p.offset = snapToCluster(p.para, p.offset);

    // 光标紧贴着一枚控件的闭括号（在它后面）时，退格退掉的是「[字段]」这个整体 —— 保留
    //   内容、拆成普通正文。闭括号只占流位置不占码点，按码点往回删会删掉字段里的最后一个
    //   字，而用户眼里那里根本没有字可删。
    // 判据只能落在流位置上：同一枚闭括号，"在它后面"与"在内容末尾"在文本坐标上是同一个
    //   (段, 偏移)，只有流位置分得开（侧别也是流位置的派生量）。控件区间是
    //   [开括号, 闭括号之后)，于是"闭括号就在光标左边那一格"等价于 hi == 光标流位置。
    // 这一条要排在"段首退格 = 并入上一段"之前：控件跨段时它的闭括号落在下一段的段首，
    //   此时光标在段首靠右（跨过了那枚闭括号），按段首处理会并段而不是退役这枚悬空边界；
    //   而段首靠左那一格不可能是任何控件的 hi —— 它既然是闭括号所在格，hi 就至少再多一。
    // 命中即唯一：嵌套时内层的 hi 严格小于外层，相邻字段（][）处只有左边那个的 hi 落在这
    //   一格上；hi 是区间右端，而控件区间彼此要么不相交、要么严格包含，不会有两个相等。
    // 策略拒绝时（表单模式默认把结构锁住）落到下面那条普通退格上，不把这一次按键吞掉。
    if (flow_.has_value() && !controls_.empty() && !block_flow_.empty()) {
        // 光标的全局流位置就是 focus —— 它就是为"同一码点偏移上的不同插入点"而存在的量。
        const FlowPos cur = selection_.focus;
        for (int i = 0; i < controls_.size(); ++i) {
            int lo = -1;
            int hi = -1;
            if (!flow_->intervalOf(control_node_ids_[static_cast<size_t>(i)], &lo, &hi)) continue;
            if (hi != cur) continue;
            if (removeControl(i, /*keep_content=*/true)) return true;
            break;  // 被策略拒了：不再找下一个（命中唯一），直接回落
        }
    }

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
    // 与退格同一条：选区非空时只删选区，不再往后多删一个字。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (!plan.steps.empty()) return commitErase(plan, "前删");
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
    // 选区非空时回车是"删掉选区，再在落点分段"。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    // 删完就往下走，而不是删完即止：回车与退格不同，它要的是"在这里断开"。
    // 已经删过之后，下面任何一步被拒都得报"变过"（consumed）—— 报假会让宿主跳过
    //   重绘，症状是"选中的内容已经没了，屏幕上还留着"。
    const bool consumed = !plan.steps.empty();
    if (consumed && !commitErase(plan, "分段")) return false;
    TextPos p = caret();
    if (p.para < 0 || p.para >= static_cast<int>(blocks_.size())) p.para = 0;
    p.offset = snapToCluster(p.para, p.offset);

    // 分段 = 在断点处插入一个段落分隔符，所以判据与"在这里插字"完全相同。
    //   判定用的位置与真正插标记的位置必须是同一个，安全阀也只该有一处。
    if (!policyAt(p).allowed) {
        ++blocked_edits_;
        return consumed;
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
        return consumed;
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
    if (!commitOnTree(std::move(tx), p.para, ParaDelta{true, p.para, 1})) {
        ++blocked_edits_;
        return consumed;
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
    if (!commitOnTree(std::move(tx), head, ParaDelta{true})) {
        ++blocked_edits_;
        return false;
    }
    setCaretAtFlow(at_a);  // 被删掉的那枚段落标记的位置 = 合并点
    return true;
}

bool EditorEngine::mergeWithPreviousParagraph() {
    if (blocks_.empty()) return false;
    // 选区非空时它退化成"删掉选区"：它与段首退格本来就是同一件事的两面，
    //   而退格在选区非空时只删选区（见 backspace），这条必须跟着一致。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (!plan.steps.empty()) return commitErase(plan, "合并");
    TextPos p = caret();
    if (p.para <= 0 || p.para >= static_cast<int>(blocks_.size())) return false;
    // 只有光标确实在段首才合并（否则退格该删的是前一个簇）。
    if (snapToCluster(p.para, p.offset) != 0) return false;
    return mergeParagraphInto(p.para - 1);
}

bool EditorEngine::mergeWithNextParagraph() {
    if (blocks_.empty()) return false;
    // 同上：选区非空时只删选区。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (!plan.steps.empty()) return commitErase(plan, "合并");
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

namespace {

/**
 * 读控件的运行时属性。四个分居两处：hidden 在节点上（呈现态的流位置换算要看它），
 * 其余三个在控件数据里。
 */
bool readPropValue(const Node* n, ControlProp p, bool* out) {
    if (n == nullptr || n->kind != NodeKind::ContentControl || out == nullptr) return false;
    switch (p) {
        case ControlProp::Hidden:
            *out = n->hidden;
            return true;
        case ControlProp::Readonly:
            *out = n->control.readonly;
            return true;
        case ControlProp::Required:
            *out = n->control.required;
            return true;
        case ControlProp::StructureLock:
            *out = n->control.structureLock;
            return true;
    }
    return false;
}

}  // namespace

/**
 * 删控件与改控件属性：结构操作的引擎入口。
 *
 * 两件事都只动树，改完交给 commitOnTree 一次重建 —— 与文本编辑走同一条路，于是撤销、重排、
 * 重绘、记账自动一致，不必另开一条特权通道。
 */
bool EditorEngine::removeControl(int index, bool keep_content) {
    if (!loaded_ || !flow_.has_value()) return false;
    if (index < 0 || index >= controls_.size()) return false;
    const NodeId id = control_node_ids_[static_cast<size_t>(index)];
    const Node* c = tree_.find(id);
    if (c == nullptr || c->kind != NodeKind::ContentControl || c->parent == nullptr) return false;
    const NodeId parent_id = c->parent->id;
    int idx = -1;
    for (size_t i = 0; i < c->parent->children.size(); ++i) {
        if (c->parent->children[i]->id == id) {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (idx < 0) return false;

    // 结构操作的门：表单模式默认被 structureLock 锁住，文档模式放行（作者在设计表单）。
    if (!formDegraded() && !canDoStructureOp(tree_, mode_, id).allowed) {
        ++blocked_edits_;
        return false;
    }

    int lo = -1;
    int hi = -1;
    if (!flow_->intervalOf(id, &lo, &hi)) return false;

    Transaction tx;
    tx.label = keep_content ? "删除控件（保留内容）" : "删除控件";
    tx.selection_before = Range{lo, hi};
    tx.selection_after = Range{lo, lo};

    if (keep_content) {
        // 拆壳：内容上提、节点移除。两个 span 的差就是节点自己那两枚边界。
        UnwrapNode u;
        u.at = Pos{parent_id, idx};
        u.at_flow = lo;
        u.span_before = hi - lo;
        u.span_after = hi - lo - 2;
        u.node = captureSubtree(tree_, id);
        u.detached = id;
        tx.steps.push_back(Step{u});
    } else {
        RemoveNode r;
        r.at = Pos{parent_id, idx};
        r.at_flow = lo;
        r.removed = captureSubtree(tree_, id);
        r.detached = id;
        tx.steps.push_back(Step{r});
    }

    const int dirty = controls_.at(index).range.start.para;
    // 段数可能变（控件里的段落标记没了，或者上提之后换了容器），所以走全量重编译。
    if (!commitOnTree(std::move(tx), dirty, ParaDelta{true})) {
        ++blocked_edits_;
        return false;
    }
    // 落点 = 控件区间的起点：保留内容时那里正是它的第一枚内容。
    setCaretAtFlow(lo);
    return true;
}

bool EditorEngine::setControlProp(int index, ControlProp prop, bool value) {
    if (!loaded_ || index < 0 || index >= controls_.size()) return false;
    const NodeId id = control_node_ids_[static_cast<size_t>(index)];
    bool before = false;
    if (!readPropValue(tree_.find(id), prop, &before)) return false;
    // 幂等：没变就不提交 —— 撤销栈里多一条什么都没做的记录只会让人困惑。
    if (before == value) return true;

    SetControlProp s;
    s.id = id;
    s.prop = prop;
    s.value = value;
    s.before = before;

    Transaction tx;
    tx.label = "改控件属性";
    tx.steps.push_back(Step{s});
    // hidden 改呈现态的流结构（整棵摘除），其余三项不改结构。
    const bool structural = (prop == ControlProp::Hidden);
    const int dirty = controls_.at(index).range.start.para;
    if (!commitOnTree(std::move(tx), dirty, structural ? ParaDelta{true} : ParaDelta{})) return false;
    if (structural) markAllLayoutDirty();
    return true;
}

bool EditorEngine::removeControlAndRender(int index, bool keep_content) {
    if (!removeControl(index, keep_content)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::setControlPropAndRender(int index, ControlProp prop, bool value) {
    // 先读旧值：幂等（值没变）时 setControlProp 也返回 true，但状态与呈现一个字都没动 ——
    //   跟着白跑一次重排重绘没有意义，也会把探针的"一次操作重画了多少"记账弄脏。
    bool before = false;
    if (loaded_ && index >= 0 && index < static_cast<int>(controls_.size())) {
        readPropValue(tree_.find(control_node_ids_[static_cast<size_t>(index)]), prop, &before);
    }
    if (!setControlProp(index, prop, value)) return false;
    if (before == value) return true;
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
    // 反缩进归"删类"：选区非空时它要做的事就是删掉选区，删完即止 —— 与退格同一口径。
    //   删完再去找前面的空格是错的：光标这时停在选区起点，前面那一段空格跟用户选中的
    //   东西无关，退掉它等于多删了用户没选的内容。
    const SelectionErase plan = planEraseSelection();
    if (plan.blocked) return false;
    if (!plan.steps.empty()) return commitErase(plan, "反缩进");
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
                             int* out_side) const {
    if (out_owner) *out_owner = -1;
    if (out_side) *out_side = 0;
    if (!out || layout_.lines.empty()) return false;

    const float page_w = cfg_.page.width_pt;
    const float page_h = cfg_.page.height_pt;
    const float stride = page_h + cfg_.page_gap_pt;

    int page = 0;
    float local_y = y_pt_doc;
    if (stride > 0.0f) {
        // 页号由文档坐标直接给出，页内 y 是它对步距取余。
        page = std::max(0, static_cast<int>(std::floor(y_pt_doc / stride)));
        local_y = y_pt_doc - static_cast<float>(page) * stride;
        // 落在页间空隙里，或者横向出了页外 —— 没有"这一页的哪一行"可言
        if (local_y < 0.0f || local_y > page_h) return false;
        if (x_pt_doc < 0.0f || x_pt_doc > page_w) return false;
    }

    // 块轴：找同一页里最近的一行。
    const LineBox* best = nullptr;
    float best_d = 1e30f;
    for (const LineBox& lb : layout_.lines) {
        if (lb.page != page) continue;
        const float bp = lb.vertical ? x_pt_doc : local_y;
        const float d = (bp < lb.block_lo_pt) ? (lb.block_lo_pt - bp)
                                              : (bp > lb.block_hi_pt ? bp - lb.block_hi_pt : 0.0f);
        if (d < best_d) {
            best_d = d;
            best = &lb;
        }
    }
    if (!best) return false;

    // 行内轴：找最近的插入点。势力范围与簇边界的口径在 hitTestInLine 里（上下键也走它）。
    const float ip = best->vertical ? local_y : x_pt_doc;
    uint32_t cp = 0;
    int side = 0;
    hitTestInLine(*best, ip, &cp, &side);

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
