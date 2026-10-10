#include "core/engine.h"

#include "core/hash.h"

#include <hb.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

#include "core/text/shaper.h"

namespace ced {
namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

const std::vector<uint32_t> kEmptyStarts;

// 页内容指纹
//
// 用途：判断"这一页的画面对不对"。指纹没变就跳过重画 —— 这是光栅增量的全部依据。
// 要求不高：不是密码学强度，只要求"内容变了几乎一定会变"。
//
// 混合器（kFnvBasis / fnvMix）挪去了 core/hash.h：现在排版层也要用它算
//   段级整形指纹，两处必须是同一套混合（各写一份的话，"哪一处换了算法"
//   会静默地退化成"缓存永不命中"或更糟的"两处对同一输入给出不同指纹"）。

/**
 * 把磅坐标量化到 1/16 磅再比较。
 * 不量化就会出问题：浮点末位抖动会让"内容没变"的页也判成脏页，
 * 于是每一页都重画，增量等于没做。这类 bug 的表象是"看着对，但数字没下来"。
 */
int32_t quantPt(float v) { return static_cast<int32_t>(std::lround(v * 16.0f)); }

/**
 * 把 hb_font_draw_glyph 的轮廓收集起来求包围盒。
 * 用途：确定轮廓坐标的 y 轴方向到底朝哪 —— 这件事不靠推理，靠数据。
 */
struct BBoxCollector {
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    bool any = false;
    void add(float x, float y) {
        minx = std::min(minx, x);
        miny = std::min(miny, y);
        maxx = std::max(maxx, x);
        maxy = std::max(maxy, y);
        any = true;
    }
};

void bboxMoveTo(hb_draw_funcs_t*, void* dd, hb_draw_state_t*, float x, float y, void*) {
    static_cast<BBoxCollector*>(dd)->add(x, y);
}
void bboxLineTo(hb_draw_funcs_t*, void* dd, hb_draw_state_t*, float x, float y, void*) {
    static_cast<BBoxCollector*>(dd)->add(x, y);
}
void bboxQuadTo(hb_draw_funcs_t*, void* dd, hb_draw_state_t*, float, float, float x, float y,
                void*) {
    static_cast<BBoxCollector*>(dd)->add(x, y);
}
void bboxCubicTo(hb_draw_funcs_t*, void* dd, hb_draw_state_t*, float, float, float, float, float x,
                 float y, void*) {
    static_cast<BBoxCollector*>(dd)->add(x, y);
}

hb_draw_funcs_t* bboxFuncs() {
    static hb_draw_funcs_t* f = nullptr;
    if (!f) {
        f = hb_draw_funcs_create();
        hb_draw_funcs_set_move_to_func(f, bboxMoveTo, nullptr, nullptr);
        hb_draw_funcs_set_line_to_func(f, bboxLineTo, nullptr, nullptr);
        hb_draw_funcs_set_quadratic_to_func(f, bboxQuadTo, nullptr, nullptr);
        hb_draw_funcs_set_cubic_to_func(f, bboxCubicTo, nullptr, nullptr);
    }
    return f;
}

}  // namespace

/** 生命周期：字体加载与引擎初始化。 */

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

void EditorEngine::setBlocks(std::vector<BlockSpec> blocks) {
    blocks_ = std::move(blocks);
    caret_ = TextPos{0, 0};
    caret_rect_px_prev_ = Rect{};
    base_valid_ = false;
    // 整份文档换了：段数与内容都可能全变，没有可复用的前缀。
    markAllLayoutDirty();
}

size_t EditorEngine::frameBytes() const { return bitmap_.byteSize(); }

size_t EditorEngine::documentCodepoints() const {
    size_t n = 0;
    for (const BlockSpec& b : blocks_) n += b.cp_text.size();
    return n;
}

/** 布局：重排整篇，并把排版统计同步到 EngineStats。 */

void EditorEngine::syncStatsFromLayout() {
    stats_.page_count = layout_.stats.page_count;
    stats_.glyph_count = layout_.stats.glyph_count;
    stats_.placed_line_count = layout_.stats.placed_line_count;
    stats_.cluster_count = layout_.stats.cluster_count;
    stats_.missing_glyph_count = layout_.stats.missing_glyph_count;
    stats_.font_run_count = layout_.stats.font_run_count;
    stats_.shape_ms = layout_.stats.shape_ms;
    stats_.layout_ms = layout_.stats.layout_ms;
    stats_.face_count = fonts_.faceCount();
    stats_.font_instance_count = static_cast<int>(fonts_.instanceCount());
    stats_.zoom = cfg_.zoom;
}

void EditorEngine::computePageFingerprints(int dirty_lo, int dirty_hi) {
    const int n = std::max(1, layout_.stats.page_count);

    // 增量重算的前提：页数没变（页数变了一定整篇的重画判据都要重来），
    // 且上一轮的指纹数组还在（长度够、页数对得上）。不满足就退化成整篇重算。
    const bool partial = dirty_lo >= 0 && dirty_hi >= dirty_lo && page_fp_new_count_ == n &&
                         page_fp_new_.size() >= static_cast<size_t>(n);
    if (!partial) {
        page_fp_new_.assign(static_cast<size_t>(n), kFnvBasis);
        dirty_lo = 0;
        dirty_hi = n - 1;
    } else {
        // 只把要重算的那几页清零，其余页保留上一轮的值 ——
        // 那些页的字形一个字节都没动，指纹自然不变（这正是增量的依据）。
        for (int p = dirty_lo; p <= dirty_hi; ++p) page_fp_new_[static_cast<size_t>(p)] = kFnvBasis;
    }
    page_fp_new_count_ = n;

    // 只遍历受影响页的字形区间。
    //
    // 依据是这条不变式：layout_ 的字形数组按页单调（排版是逐段追加的，
    // 而页号只增不减），所以"页号落在 [dirty_lo, dirty_hi]"的字形是一段连续区间，
    // 二分就能定位。它成立的前提是排版永远按段序追加 ——
    // 单测 [15] 里有一条断言专门钉住它（一旦有人打乱追加顺序，这里会静默算错）。
    const std::vector<PlacedGlyph>& gs = layout_.glyphs;
    const auto gb = std::lower_bound(gs.begin(), gs.end(), dirty_lo,
                                     [](const PlacedGlyph& g, int page) { return g.page < page; });
    const auto ge = std::upper_bound(gb, gs.end(), dirty_hi,
                                     [](int page, const PlacedGlyph& g) { return page < g.page; });
    for (auto it = gb; it != ge; ++it) {
        const PlacedGlyph& g = *it;
        if (g.page < 0 || g.page >= n) continue;
        uint64_t h = page_fp_new_[static_cast<size_t>(g.page)];
        h = fnvMix(h, g.glyph_id);
        h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(g.x_pt))));
        h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(g.y_pt))));
        h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(g.rotate_deg))));
        h = fnvMix(h, g.cluster);
        h = fnvMix(h, static_cast<uint64_t>(g.color.r) | (static_cast<uint64_t>(g.color.g) << 8) |
                          (static_cast<uint64_t>(g.color.b) << 16));
        page_fp_new_[static_cast<size_t>(g.page)] = h;
    }

    // 表单控件与编辑模式刻意不进页指纹。
    //
    //   页指纹是"底图该不该重画"的判据，而底图里只有页面底色与字形 ——
    //   控件标记（小点 / 方括号）画在帧上、不进底图（原因见 presentInternal）。
    //   既然底图不依赖控件与模式，把它们混进来就只是无谓地重画所有页。
    //
    //   切模式时旧标记的清除靠 frame_needs_full_refresh_：切模式 / 改控件时置位，
    //     下一次呈现整帧重贴，旧标记自然被覆盖。这比往指纹里塞东西更直接，
    //     也不影响底图的增量。
}

void EditorEngine::relayout() {
    const auto t0 = Clock::now();

    // 顺序有讲究：先夹光标、再排版 —— 因为排版要知道"哪些字段要显示括号"
    //   （Focused 档下那张名单就是由光标位置决定的，见 layout.h / bracketedControls）。
    //
    //   这里只夹数值范围，不吸附簇：簇吸附需要簇表，而此刻尚未排版；用上一轮的簇表
    //     会把光标拉回旧簇边界，表现为"敲字后光标差一个位置"。簇吸附放在排版之后。
    if (!blocks_.empty()) {
        if (caret_.para < 0) caret_.para = 0;
        if (caret_.para >= static_cast<int>(blocks_.size()))
            caret_.para = static_cast<int>(blocks_.size()) - 1;
        const size_t len = blocks_[static_cast<size_t>(caret_.para)].cp_text.size();
        if (caret_.offset > len) caret_.offset = len;
    } else {
        caret_ = TextPos{0, 0};
    }

    // 把控件与要显示括号的那些控件一起传进排版：它们的两个边界会插入
    //   "括号占位"（占宽度、不产生字形），于是断行/行盒长度/簇位置全把括号算进去
    //   —— 这就是"括号进文档流"。不显示的就不占位（规则：显示才占位）。
    //
    // 名单是引擎算好的（bracketedControls），排版不认识"聚焦"这个概念：
    //   两边共用同一张名单，"要画"和"要占位"在结构上不可能不一致。
    const std::vector<int> bracketed = bracketedControls();
    const int n_para = static_cast<int>(blocks_.size());

    // 四档决策，从严到宽：整篇都脏或没有脏段走全量（前者影响面不可定位，后者是保守
    // 回退）；只重排脏段、后缀原样搬回（trySpliceOnly，要求后缀的行结构没变）；
    // 前缀保留加从脏段续排（常规增量）；其余情况全量。
    const bool must_full = layout_needs_full_ || layout_dirty_from_ >= n_para;
    const int from = layout_dirty_from_;
    const bool can_resume = !must_full && from > 0 && from < n_para &&
                            layout_.para_index.size() > static_cast<size_t>(from);

    last_reused_paragraphs_ = 0;
    last_suffix_shifted_ = false;
    const auto t_layout0 = Clock::now();  // 只量排版本身：拼接/续排/全量三种走法都算
    // 需要重算页指纹的页区间（-1 = 整篇），由后缀复用那条路带回来：
    // 它知道脏段新旧两版占了哪几页 —— 那才是"内容真的变了的页"，
    // 比整篇重扫一遍字形便宜两个数量级。
    int fp_lo = -1, fp_hi = -1;
    SuffixReuse reuse = SuffixReuse::None;
    if (!must_full && !para_count_changed_ && from >= 0 && from < n_para) {
        reuse = trySuffixReuse(from, bracketed, &fp_lo, &fp_hi);
    }
    if (reuse != SuffixReuse::None) {
        const ParaIndex& pi = layout_.para_index[static_cast<size_t>(from)];
        last_relaid_paragraphs_ = 1;
        last_reused_paragraphs_ = n_para - 1 - from;
        last_suffix_shifted_ = (reuse == SuffixReuse::Shifted);
        last_relaid_lines_ = pi.line_end - pi.line_begin;
        last_layout_full_ = false;
    } else if (can_resume) {
        // 续排锚点必须在截断之前读出来 —— 截断会把它一起丢掉。
        const ReflowState resume_state =
            layout_.para_index[static_cast<size_t>(from)].state_at_start;
        const int prefix_lines = layout_.para_index[static_cast<size_t>(from)].line_begin;
        truncateLayoutAt(from);
        layoutDocumentRange(fonts_, blocks_, cfg_.page, &controls_, &bracketed, from, n_para,
                            resume_state, &shaping_cache_, &layout_);
        last_relaid_paragraphs_ = n_para - from;
        last_relaid_lines_ = layout_.stats.placed_line_count - prefix_lines;
        last_layout_full_ = false;
    } else {
        layout_ = LayoutResult{};
        layoutDocumentRange(fonts_, blocks_, cfg_.page, &controls_, &bracketed, 0, n_para,
                            freshReflowState(cfg_.page), &shaping_cache_, &layout_);
        last_relaid_paragraphs_ = n_para;
        last_relaid_lines_ = layout_.stats.placed_line_count;
        last_layout_full_ = true;
        fp_lo = -1;  // 整篇重排：没有"没变过的页"可言
    }
    layout_dirty_from_ = n_para;  // 已重排到末尾，没有脏段了
    layout_needs_full_ = false;
    para_count_changed_ = false;  // 这一轮已经把新段号排进去了

    // 这一轮排版的耗时。只算排版本身（拼接/续排/全量三种走法都算），
    // 不含页指纹记账与统计同步 —— 那两项是 O(全篇) 的固定开销，
    // 混进来的话"增量到底省了多少"就看不出来了（它们的量级能盖过排版本身）。
    // 必须在 syncStatsFromLayout 之前写：统计是从 layout_.stats 抄过去的。
    layout_.stats.layout_ms = msSince(t_layout0);
    syncStatsFromLayout();
    const auto t_fp0 = Clock::now();
    computePageFingerprints(fp_lo, fp_hi);
    stats_.page_fp_ms = msSince(t_fp0);

    // 光标夹回合法范围并吸附到簇边界 —— 编辑可能让段落变短。
    if (!blocks_.empty()) {
        caret_.offset = snapToCluster(caret_.para, caret_.offset);
    }

    // 这一轮排版的真实耗时（含截断/拼接/逐段汇总）—— 有些路径不经过
    // layoutDocumentRange（快路），那里算不出这个数，所以在这里统一收口。
    edit_stats_.layout_ms = layout_.stats.layout_ms;
    // 排版成本只记一次：置位后由 presentInternal() 消费。
    layout_pending_ = true;
    stats_.total_ms += msSince(t0);
}

EditorEngine::SuffixReuse EditorEngine::trySuffixReuse(int from, const std::vector<int>& bracketed,
                                                       int* page_lo, int* page_hi) {
    *page_lo = -1;
    *page_hi = -1;
    const int n_para = static_cast<int>(blocks_.size());
    // 没有后缀可搬（脏段就是最后一段），就没什么可省的 —— 交给常规路径。
    if (from < 0 || from + 1 >= n_para) return SuffixReuse::None;
    if (layout_.para_index.size() <= static_cast<size_t>(from + 1)) return SuffixReuse::None;

    // 括号占位计划不能伸进后缀段：插入/删除会移动控件端点，而占位槽的位置由端点
    // 决定 —— 那些段的内容一个字没变、占位却变了。边界落在脏段之内的都可以搬。
    for (int ci : bracketed) {
        if (ci < 0 || ci >= controls_.size()) continue;
        const Control& c = controls_.at(ci);
        if (c.range.start.para > from || c.range.end.para > from) return SuffixReuse::None;
    }

    const ParaIndex old_pi = layout_.para_index[static_cast<size_t>(from)];
    // 脏段自己的续排锚点来自上一轮（前缀一个字都没动）。第 0 段没有前缀，用全篇起点。
    const ReflowState resume = (from == 0) ? freshReflowState(cfg_.page) : old_pi.state_at_start;

    // 只排脏段自己。结果先待在 scratch 里，等状态判完再决定要不要用。
    LayoutResult scratch;
    const ReflowState after =
        layoutDocumentRange(fonts_, blocks_, cfg_.page, &controls_, &bracketed, from, from + 1,
                            resume, &shaping_cache_, &scratch);
    const ParaIndex& np = scratch.para_index[static_cast<size_t>(from)];

    // 脏段新旧两版覆盖的页区间（并集）：只有这几页的内容可能变了，
    // 页指纹只需重算它们。取不到（两版都没行）就给 -1，让调用方整篇重算。
    int u_lo = -1, u_hi = -1;
    auto addPageRange = [&](const std::vector<LineBox>& lines, int b, int e) {
        if (e <= b) return;
        const int lo = lines[static_cast<size_t>(b)].page;
        const int hi = lines[static_cast<size_t>(e - 1)].page;
        u_lo = (u_lo < 0) ? lo : std::min(u_lo, lo);
        u_hi = (u_hi < 0) ? hi : std::max(u_hi, hi);
    };
    addPageRange(layout_.lines, old_pi.line_begin, old_pi.line_end);
    addPageRange(scratch.lines, np.line_begin, np.line_end);

    const int old_lines = old_pi.line_end - old_pi.line_begin;
    const int new_lines = np.line_end - np.line_begin;
    const int old_glyphs = old_pi.glyph_end - old_pi.glyph_begin;
    const int new_glyphs = np.glyph_end - np.glyph_begin;
    const int dline = new_lines - old_lines;
    const int dglyph = new_glyphs - old_glyphs;

    // 换掉脏段那一段：erase + insert 各一次 memmove，比重新排一遍后缀便宜一到两个数量级。
    layout_.lines.erase(layout_.lines.begin() + old_pi.line_begin,
                        layout_.lines.begin() + old_pi.line_end);
    layout_.lines.insert(layout_.lines.begin() + old_pi.line_begin,
                         scratch.lines.begin() + np.line_begin,
                         scratch.lines.begin() + np.line_end);
    layout_.glyphs.erase(layout_.glyphs.begin() + old_pi.glyph_begin,
                         layout_.glyphs.begin() + old_pi.glyph_end);
    layout_.glyphs.insert(layout_.glyphs.begin() + old_pi.glyph_begin,
                          scratch.glyphs.begin() + np.glyph_begin,
                          scratch.glyphs.begin() + np.glyph_end);

    // 切片：本段换成新的（起点仍在原处），后面的段整体平移 dline / dglyph。
    // 不能只改本段 —— 切片的 begin/end 是"在全局数组里的下标"，前一段的长度一变，
    // 后面所有段的下标都要跟着移，否则汇总会按错位的下标求和（数字会静默地错）。
    {
        ParaIndex& p = layout_.para_index[static_cast<size_t>(from)];
        p = np;
        p.line_begin = old_pi.line_begin;
        p.line_end = old_pi.line_begin + new_lines;
        p.glyph_begin = old_pi.glyph_begin;
        p.glyph_end = old_pi.glyph_begin + new_glyphs;
    }
    for (int j = from + 1; j < n_para; ++j) {
        ParaIndex& p = layout_.para_index[static_cast<size_t>(j)];
        p.line_begin += dline;
        p.line_end += dline;
        p.glyph_begin += dglyph;
        p.glyph_end += dglyph;
    }
    layout_.para_cluster_starts[static_cast<size_t>(from)] =
        scratch.para_cluster_starts[static_cast<size_t>(from)];
    summarizeLayoutStats(&layout_);
    layout_.stats.shape_ms = scratch.stats.shape_ms;

    // 一级复用：后缀开始时的续排状态逐位相同 → 后缀的断页、全局行号与坐标
    // 都会与上一轮一模一样，一个字节都不用动。
    if (after == layout_.para_index[static_cast<size_t>(from + 1)].state_at_start) {
        *page_lo = u_lo;
        *page_hi = u_hi;
        return SuffixReuse::Unchanged;
    }

    // 二级复用：行结构变了（后缀整体挪位），但后缀是纯横排 → 只有块轴位置变，
    // 断行与字形的行内位置一个字都不用重算。见 shiftSuffixBlockAxis。
    if (u_lo < 0) return SuffixReuse::None;
    if (!suffixIsHorizontalOnly(from)) return SuffixReuse::None;
    if (!shiftSuffixBlockAxis(from, after)) return SuffixReuse::None;
    *page_lo = u_lo;
    *page_hi = std::max(0, layout_.stats.page_count - 1);
    return SuffixReuse::Shifted;
}

bool EditorEngine::suffixIsHorizontalOnly(int from) const {
    for (int p = from + 1; p < static_cast<int>(blocks_.size()); ++p) {
        if (blocks_[static_cast<size_t>(p)].wm.isVertical()) return false;
    }
    return true;
}

bool EditorEngine::shiftSuffixBlockAxis(int from, const ReflowState& after) {
    const float cy = cfg_.page.margin_pt;
    const float ch = cfg_.page.contentHeightPt();
    const int n_para = static_cast<int>(blocks_.size());

    const ParaIndex& dp = layout_.para_index[static_cast<size_t>(from)];
    const size_t first_glyph = static_cast<size_t>(dp.glyph_end);
    std::vector<PlacedGlyph>& gs = layout_.glyphs;  // 校验与改写共用这一份

    // 沿着"行槽折线"走一遍：每行占 blocks[p].size_pt * line_factor，段间让出
    // space_after_pt，放不下就换页 —— 与 layoutDocumentRange 横排分支同一套规则。
    //
    // 先空跑一遍（apply=false）做校验，通过了再真改。理由：这一趟边走边写
    //   layout_，一旦中途发现"字形与行号对不上"就已经改了一半 —— 那时既不能
    //   继续（结果错）也不能退出（数据已经脏了）。校验与修改必须是两趟。
    int final_page = after.page_index;  // 出参：走完最后落在哪一页
    auto walk = [&](bool apply) -> bool {
        float s = after.cursor_y;
        int page = after.page_index;
        int line_no = after.line_total;
        size_t g = first_glyph;
        for (int p = from + 1; p < n_para; ++p) {
            const BlockSpec& b = blocks_[static_cast<size_t>(p)];
            // 续排锚点：每一段都要更新成新的位置，否则以后从这一段续排会拿着过期
            // 状态（页号 / 块轴位置 / 行号都变了）。与主循环一致：在空段判定与翻页
            // 之前记录。
            ParaIndex& pi = layout_.para_index[static_cast<size_t>(p)];
            if (apply) pi.state_at_start = ReflowState{page, s, line_no};

            if (b.page_break_before && s > cy + 0.01f) {
                ++page;
                s = cy;
            }
            // 一段占多少块轴空间由"它实际产出了几行"决定：空段落也有一行
            //   （见 layout.cpp 里那段说明），而有文本却取不到字体的段一行都没有 ——
            //   后者在排版里只翻页、不让出 space_after，这里必须与它一致。
            if (pi.line_end == pi.line_begin) continue;
            const float lh = b.size_pt * b.line_factor;
            for (int L = pi.line_begin; L < pi.line_end; ++L) {
                LineBox& lb = layout_.lines[static_cast<size_t>(L)];
                if (s + lh > cy + ch) {
                    ++page;
                    s = cy;
                }
                const float dy = s - lb.block_lo_pt;
                // 字形与它的行一一对应：两边都是按行序追加的，并行往前走即可
                // （某行可能一个字形都没有 —— 全是占位槽的行 —— 指针不动就是对的）。
                const int old_line = lb.line;
                while (g < gs.size() && gs[g].line == old_line) {
                    if (apply) {
                        gs[g].y_pt += dy;
                        gs[g].page = page;
                        gs[g].line = line_no;
                    }
                    ++g;
                }
                if (apply) {
                    lb.block_lo_pt += dy;
                    lb.block_hi_pt += dy;
                    lb.bracket_axis_pt += dy;  // 横排下它就是基线，跟着行一起挪
                    lb.page = page;
                    lb.line = line_no;
                }
                s += lh;
                ++line_no;
            }
            s += b.space_after_pt;
        }
        // 字形必须正好走完：多一个少一个都说明行号与字形对不上 —— 宁可整篇重排，
        // 也不要留下错位的排版（错位是静默的，只在画面上表现为"某个字串行了"）。
        final_page = page;
        return g == gs.size();
    };
    if (!walk(/*apply=*/false)) return false;
    walk(/*apply=*/true);

    layout_.stats.page_count = final_page + 1;
    summarizeLayoutStats(&layout_);
    return true;
}

void EditorEngine::markLayoutDirty(int para) {
    if (para < 0) para = 0;
    if (layout_dirty_from_ > para) layout_dirty_from_ = para;
}

int EditorEngine::minControlParagraph() const {
    int lo = -1;
    for (int i = 0; i < controls_.size(); ++i) {
        const Control& c = controls_.at(i);
        if (c.range.start.para < 0) continue;
        if (lo < 0 || c.range.start.para < lo) lo = c.range.start.para;
    }
    return lo;
}

void EditorEngine::markLayoutDirtyForBrackets() {
    const int lo = minControlParagraph();
    if (lo >= 0) markLayoutDirty(lo);
}

void EditorEngine::truncateLayoutAt(int para) {
    if (para <= 0) {
        layout_ = LayoutResult{};
        return;
    }
    const size_t k = static_cast<size_t>(para);
    if (layout_.para_index.size() <= k) {
        layout_ = LayoutResult{};
        return;
    }
    const ParaIndex& cut = layout_.para_index[k];
    const size_t line_cut = static_cast<size_t>(std::max(0, cut.line_begin));
    const size_t glyph_cut = static_cast<size_t>(std::max(0, cut.glyph_begin));
    if (line_cut > layout_.lines.size() || glyph_cut > layout_.glyphs.size()) {
        layout_ = LayoutResult{};
        return;
    }
    // 字形与行盒严格按段序追加，所以"第 para 段的起点"就是安全的切点：
    // [0, para) 的切片与内容一个字节都不用动。
    layout_.lines.resize(line_cut);
    layout_.glyphs.resize(glyph_cut);
    layout_.para_index.resize(k);
    layout_.para_cluster_starts.resize(k);
}

void EditorEngine::relayoutIfBracketPlanChanged(int prev_plan) {
    if (bracketPlanId() == prev_plan) return;
    markLayoutDirtyForBrackets();
    relayout();
}

/** 呈现：把底图与光标提交到帧，并维护上屏脏矩形。 */

void EditorEngine::repaint() {
    // 帧范围取本次布局的页数，不能锁在"底图已画过多少页"上。
    //
    //   否则文档从 N 页涨到 N+1 页时（页尾敲字、内容溢出到下一页）帧高不变，
    //   presentInternal 里 geom_changed 恒为 false，新页永远不进帧 —— 表现为
    //   "字打进去了，屏幕上什么都没多"，且之后每次重绘都按 N 页算帧高，不会自愈。
    //   页数增加时全帧重建；页数不变时仍走脏页增量。
    const int total = std::max(1, stats_.page_count);
    // 档 1 的帧恒从第 0 页起（还没有视口虚拟化）。将来做滚动窗口时，
    // 这里换成"窗口首屏"，并且必须同时把 total 换成窗口内的页数 ——
    // 只改其中一个，就会退化成"帧高是全文、只画一页"或反过来。
    presentInternal(/*first_page=*/0, /*count=*/total, /*force_full=*/false);
}

void EditorEngine::paintPageRange(int first_page, int count) {
    presentInternal(first_page, count, /*force_full=*/false);
}

void EditorEngine::presentInternal(int first_page, int count, bool force_full) {
    const auto t0 = Clock::now();
    bitmap_.resetPaintedPixels();
    // 本次呈现的缓存命中率 —— 与 paintedPixels 同一条纪律：统计只描述这一轮。
    if (glyph_cache_) glyph_cache_->resetStats();

    const float zoom = cfg_.zoom;
    const int page_px_w = std::max(1, static_cast<int>(std::lround(cfg_.page.width_pt * zoom)));
    const int page_px_h = std::max(1, static_cast<int>(std::lround(cfg_.page.height_pt * zoom)));
    const int gap_px = std::max(0, static_cast<int>(std::lround(cfg_.page_gap_pt * zoom)));

    const int total_pages = std::max(1, stats_.page_count);
    if (first_page < 0) first_page = 0;
    if (first_page > total_pages - 1) first_page = total_pages - 1;
    count = std::max(1, std::min(count, total_pages - first_page));
    count = std::min(count, 64);  // 防御：异常文档别把帧缓冲撑爆

    const int frame_w = page_px_w;
    const int frame_h = count * page_px_h + (count - 1) * gap_px;
    const float units_to_px = zoom / kFontScalePerPt;

    // 局部页的矩形（含上下半格页间隙；间隙是背景色，重复擦无副作用）
    auto pageRectLocal = [&](int local) -> Rect {
        const float off_y = static_cast<float>(local * (page_px_h + gap_px));
        const float top = std::max(0.0f, off_y - static_cast<float>(gap_px) * 0.5f - 1.0f);
        const float bot =
            std::min(static_cast<float>(frame_h), off_y + static_cast<float>(page_px_h) +
                                                      static_cast<float>(gap_px) * 0.5f + 1.0f);
        return Rect{0.0f, top, static_cast<float>(frame_w), bot - top};
    };

    // 三段分解的累加器（见 EngineStats 里 paint_fill_ms 的注释）
    double sub_fill_ms = 0.0;
    double sub_glyph_ms = 0.0;

    auto paintPageIntoBase = [&](int local, int page_index) {
        const auto t_fill0 = Clock::now();
        const Rect full = pageRectLocal(local);
        base_.fillRect(full, cfg_.background);
        const float off_y = static_cast<float>(local * (page_px_h + gap_px));
        const Rect page_rect{0.0f, off_y, static_cast<float>(page_px_w),
                             static_cast<float>(page_px_h)};
        base_.fillRect(page_rect, cfg_.page_fill);
        if (cfg_.draw_page_border) base_.strokeRect(page_rect, cfg_.page_border, 1.0f);
        sub_fill_ms += msSince(t_fill0);

        // 底图里只有页面底色与字形 —— 表单标记（小点/方括号）不在这里画。
        //   它们依赖光标位置，所以画在帧上；见 presentInternal 里那段说明。
        const auto t_glyph0 = Clock::now();
        for (const PlacedGlyph& pg : layout_.glyphs) {
            if (pg.page != page_index || !pg.font) continue;
            base_.drawGlyph(pg.font->hb(), pg.glyph_id, pg.x_pt * zoom, pg.y_pt * zoom + off_y,
                            units_to_px, pg.color, pg.rotate_deg);
        }
        sub_glyph_ms += msSince(t_glyph0);
    };

    // 脏页数（在底图指纹被刷新之前算，否则比不出差异）
    {
        int dp = 0;
        const int n = std::max(page_fp_new_count_, page_fp_base_count_);
        for (int i = 0; i < n; ++i) {
            const uint64_t a = (i < page_fp_new_count_) ? page_fp_new_[static_cast<size_t>(i)] : 0;
            const uint64_t b =
                (i < page_fp_base_count_) ? page_fp_in_base_[static_cast<size_t>(i)] : 0;
            if (a != b) ++dp;
        }
        edit_stats_.dirty_page_count = dp;
    }

    const bool geom_changed = (base_.width() != frame_w) || (base_.height() != frame_h) ||
                              (base_first_page_ != first_page) || (base_page_count_ != count);

    // 标记计划变了就整帧重贴。这是结构性的：标记画在帧上，擦除只能靠
    // control_marks_prev_ 记下的矩形，一旦记漏就会永久残留。
    //
    //   为什么必须有它：表单标记（小点 / 方括号）是画在帧上的，
    //     而"擦掉上一帧的标记"只能靠 control_marks_prev_ 里记下的那些矩形。
    //     —— 这是一个脆弱前提：只要有一次记错、记漏、或者被别处的
    //     control_marks_prev_.clear() 清掉，那个标记就会永久残留
    //     （后续帧的擦除清单里只会有本帧要画的标记，再也碰不到它）。
    //
    //   用户真报过："光标离开某字段后，那个字段左边的方括号还在，
    //     而同一帧里另一侧的括号却正常擦掉了。" —— 只残留一个，
    //     说明不是"整块没刷新"，而是那条擦除矩形没生效。
    //     开发机上按鼠标点击 / Tab / 程序化移动都复现不出来（33 次采样全绿），
    //     所以这里不去猜触发条件，而是把这个脆弱前提整个拆掉：
    //     只要"这一帧要画的标记"与"上一帧画过的"不同，就整帧重贴 ——
    //     整帧重贴是从底图整块搬，旧标记不可能活下来。
    //
    //   代价：只在标记计划变化时付，而它恰好是"聚焦字段变了 / 控件变了 /
    //     切模式了"—— 这三种情况本来就会让页变脏、本来就要整页重画，
    //     所以这里几乎不额外增加上屏量。同字段内移动光标不触发。
    {
        const uint64_t sig = markPlanSignature();
        if (sig != mark_sig_prev_) {
            mark_sig_prev_ = sig;
            frame_needs_full_refresh_ = true;
        }
    }

    std::vector<Rect> refresh;

    if (force_full || geom_changed || !base_valid_) {
        // 底图全建
        base_.reset(frame_w, frame_h);
        base_.clear(cfg_.background);
        for (int i = 0; i < count; ++i) paintPageIntoBase(i, first_page + i);
        base_valid_ = true;
        base_first_page_ = first_page;
        base_page_count_ = count;
        page_fp_in_base_ = page_fp_new_;
        page_fp_base_count_ = page_fp_new_count_;
        edit_stats_.repainted_pages = count;
        for (int i = 0; i < count; ++i) refresh.push_back(pageRectLocal(i));
    } else {
        // 底图增量：只重画指纹变了的页
        int dirty = 0;
        for (int i = 0; i < count; ++i) {
            const int page = first_page + i;
            const bool changed = (page >= page_fp_base_count_) ||
                                 (page_fp_base_count_ != page_fp_new_count_) ||
                                 (page_fp_new_[static_cast<size_t>(page)] !=
                                  page_fp_in_base_[static_cast<size_t>(page)]);
            if (!changed) continue;
            paintPageIntoBase(i, page);
            refresh.push_back(pageRectLocal(i));
            ++dirty;
        }
        page_fp_in_base_ = page_fp_new_;
        page_fp_base_count_ = page_fp_new_count_;
        edit_stats_.repainted_pages = dirty;
    }
    edit_stats_.total_pages = count;

    // 把帧缓冲刷新到与底图一致
    //
    // 关键：bitmap_ 只在尺寸变化时才 reset。尺寸不变时它保留上一帧内容，
    //   于是"没有脏页、只有光标动了一下"时只贴回两个小矩形；写下的像素数由 Bitmap 记账。
    const bool frame_geom_changed = (bitmap_.width() != frame_w) || (bitmap_.height() != frame_h);
    if (frame_geom_changed || frame_needs_full_refresh_) {
        if (frame_geom_changed) bitmap_.reset(frame_w, frame_h);
        refresh.clear();
        refresh.push_back(
            Rect{0.0f, 0.0f, static_cast<float>(frame_w), static_cast<float>(frame_h)});
        frame_needs_full_refresh_ = false;
        control_marks_prev_.clear();  // 整帧重贴，旧标记自然没了
    } else {
        // 上一帧的光标也要擦掉 —— 它不一定落在任何脏页上
        if (caret_rect_px_prev_.w > 0.0f) refresh.push_back(caret_rect_px_prev_);
        // 上一帧的表单标记同样要擦，而且这条是功能性的（不只是省像素）：
        //   光标移出某个字段时，那个字段的方括号必须消失。
        for (const Rect& r : control_marks_prev_) refresh.push_back(r);
        control_marks_prev_.clear();
    }
    for (const Rect& r : refresh) bitmap_.copyRectFrom(base_, r);

    // 光标
    const Rect cr = caretRectPx();
    if (cr.w > 0.0f) {
        // 这里不再 copyRectFrom(base_, cr)。
        //
        //   上一帧光标已在 refresh 里擦过（caret_rect_px_prev_ 那一项），此处多余；
        //   更要紧的是它会把落在光标矩形内的表单标记一并抹掉，而 control_marks_prev_
        //   已把那些标记记为"画过了"，于是它们既留不下来也不会被补画，
        //   表现为"标记渲染不更新"。当前几何下两侧括号都在光标矩形之外，所以加回去
        //   暂时看不出差别 —— 但一旦间隙调小、字号变大或光标加宽就会开始吃标记。
        if (caret_visible_) bitmap_.fillRect(cr, cfg_.caret_color);
        caret_rect_px_prev_ = cr;
    } else {
        caret_rect_px_prev_ = Rect{};
    }

    // 表单标记（方括号；Focused 档下另有小点）
    //
    // 画在帧上、不进底图：底图的更新判据是页指纹，而页指纹里没有
    //   光标也没有标记（光标一动就重画整页，增量就失去意义了）。
    //   注意这不等于"标记依赖光标"：默认档（All）下括号是常驻的、
    //     与光标无关，只有 Focused 档的小点/括号才跟着光标走。见 drawControlMarks。
    //
    // 画在光标之后：光标和标记会在字段末尾附近贴住，
    //   标记后画才能保证它不被光标盖掉（反过来就是上面那个 bug）。
    if (policy_.mode() == EditMode::Form) drawControlMarks();

    // 记账
    //
    //   口径：repainted_pixels 是 实际写入次数，不是去重后的脏面积。
    //   重叠区域会被重复计数，所以它偶尔会略大于 frame_w×frame_h，
    //   这是对的、不是越界。两条恒等式可以拿来核对：
    //     光标移动（无脏页）  = 2 × 光标矩形（擦旧、填新）+ 表单标记
    //     整帧重画            = 整帧 + 光标矩形（+ 表单标记）
    //   光标移动现在是 2 × 光标矩形（擦旧、填新）：原先中间那次"擦底"已去掉，
    //     因为它会连带抹掉光标矩形内的表单标记（见上）。
    //   记"写入次数"而非"去重面积"：这个数字的用途是证明"真的没写整页"，
    //     写入次数是上界，上界都小两个数量级时结论才站得住。
    edit_stats_.repainted_pixels = static_cast<long long>(bitmap_.paintedPixels());
    edit_stats_.frame_w = frame_w;
    edit_stats_.frame_h = frame_h;

    {
        Rect uni{};
        bool any = false;
        auto grow = [&](const Rect& r) {
            if (r.w <= 0.0f || r.h <= 0.0f) return;
            if (!any) {
                uni = r;
                any = true;
                return;
            }
            const float x1 = std::max(uni.x + uni.w, r.x + r.w);
            const float y1 = std::max(uni.y + uni.h, r.y + r.h);
            uni.x = std::min(uni.x, r.x);
            uni.y = std::min(uni.y, r.y);
            uni.w = x1 - uni.x;
            uni.h = y1 - uni.y;
        };
        for (const Rect& r : refresh) grow(r);
        // 这一帧新画的表单标记也必须计入。
        //
        //   上面那个循环遍历的是 refresh，而 refresh 里的标记是上一帧的
        //   （它们进 refresh 是为了被擦掉）。这一帧新画的标记是在
        //   drawControlMarks() 之后才进 control_marks_prev_ 的，refresh 里没有。
        //
        //   这份 bbox 是给宿主做 putImageData(脏矩形) 用的契约 —— 它必须覆盖
        //   这一帧实际写过的所有像素。少算了，宿主就不会更新那一块：
        //   引擎帧是对的，画布上那块却留着上一帧的内容（新旧混合）。
        //   平时看不出来，是因为标记一挪位置通常伴随页重画、bbox 被页矩形兜住；
        //   但这个"兜住"是巧合，不该靠它。
        for (const Rect& r : control_marks_prev_) grow(r);
        grow(cr);
        edit_stats_.repainted_x = any ? static_cast<int>(std::floor(uni.x)) : 0;
        edit_stats_.repainted_y = any ? static_cast<int>(std::floor(uni.y)) : 0;
        edit_stats_.repainted_w = any ? static_cast<int>(std::ceil(uni.w)) : 0;
        edit_stats_.repainted_h = any ? static_cast<int>(std::ceil(uni.h)) : 0;

        // 同时累积进"待上屏"—— 宿主上屏必须用它，不能用上面这几个"本轮"数字。
        //   理由（一次操作两次 present）写在 takeBlitBox 的注释里。
        if (any) noteBlitDirty(uni);
    }

    edit_stats_.caret_para = caret_.para;
    edit_stats_.caret_offset = caret_.offset;
    edit_stats_.caret_visible = (caret_visible_ && cr.w > 0.0f);
    edit_stats_.caret_x_px = static_cast<int>(cr.x);
    edit_stats_.caret_y_px = static_cast<int>(cr.y);
    edit_stats_.caret_w_px = static_cast<int>(cr.w);
    edit_stats_.caret_h_px = static_cast<int>(cr.h);

    // 表单
    // caret_control 用 include_end = true，与 canPlaceCaret 同一口径 ——
    // 于是"面板显示光标在某控件内"与"策略允许它停在这儿"永远不会互相矛盾。
    edit_stats_.edit_mode = static_cast<int>(policy_.mode());
    edit_stats_.control_count = controls_.size();
    edit_stats_.caret_control =
        controls_.empty() ? -1 : controls_.innermostAt(caret_, /*include_end=*/true);
    edit_stats_.blocked_edits = blocked_edits_;
    edit_stats_.caret_index = caret_.offset;
    for (int i = 0; i < caret_.para && i < static_cast<int>(blocks_.size()); ++i)
        edit_stats_.caret_index += blocks_[static_cast<size_t>(i)].cp_text.size();
    edit_stats_.doc_codepoints = documentCodepoints();

    // 只有在"这一轮真的重排过"时才上报 —— 见 layout_pending_ 的注释。
    // 数字来自 relayout() 记下的粒度：全量时 relaid_lines 等于总行数，
    // 增量时只数这一轮真正排过的后缀（前缀是复用的，不能算进来）。
    edit_stats_.layout_ran = layout_pending_;
    edit_stats_.total_paragraphs = static_cast<int>(blocks_.size());
    edit_stats_.total_lines = layout_.stats.placed_line_count;
    if (layout_pending_) {
        layout_pending_ = false;
        edit_stats_.relaid_paragraphs = last_relaid_paragraphs_;
        edit_stats_.reused_paragraphs = last_reused_paragraphs_;
        edit_stats_.suffix_shifted = last_suffix_shifted_;
        edit_stats_.relaid_lines = last_relaid_lines_;
        edit_stats_.layout_was_full = last_layout_full_;
    } else {
        // 没重排：行数记 0、耗时记 0。
        // 不归零的话，光标闪烁会每 530ms 把上一次重排的 3ms 重复上报一遍。
        edit_stats_.relaid_paragraphs = 0;
        edit_stats_.reused_paragraphs = 0;
        edit_stats_.suffix_shifted = false;
        edit_stats_.relaid_lines = 0;
        edit_stats_.layout_was_full = false;
        edit_stats_.layout_ms = 0.0;
    }

    const double paint_ms = msSince(t0);
    stats_.paint_ms = paint_ms;
    stats_.paint_fill_ms = sub_fill_ms;
    stats_.paint_glyph_ms = sub_glyph_ms;
    // 减掉前两段，剩下的就是"帧缓冲搬移"这一段 —— 比再插一个计时点更不容易漏
    // （中间还有光标擦除/填充那一小块）。
    stats_.paint_present_ms = std::max(0.0, paint_ms - sub_fill_ms - sub_glyph_ms);
    stats_.frame_width = frame_w;
    stats_.frame_height = frame_h;
    edit_stats_.paint_ms = paint_ms;
    edit_stats_.total_ms = edit_stats_.layout_ms + paint_ms;

    if (glyph_cache_) {
        stats_.glyph_cache_hits = static_cast<long long>(glyph_cache_->hits());
        stats_.glyph_cache_misses = static_cast<long long>(glyph_cache_->misses());
        stats_.glyph_cache_entries = static_cast<int>(glyph_cache_->entries());
    }
}

/**
 * 表单标记：
 * 方括号 —— 字段常驻（默认全部字段；Focused 档下只有光标所在的那个）
 * 小点   —— 只有 Focused 档才画，给"光标不在"的字段（右下角一个方块）
 * 括号用字体里的字形而非矩形拼几何：拼出来的括号与正文不像同一套设计，
 * 而真字形与正文同粗细、同风格，且随正文字号缩放。字形无法纵向拉伸，
 * 跨行时每行各括一次。
 *
 * 几何全部从排版的行盒派生：位置取占位簇的 inline_pt、朝向取 bracket_side、
 * 字体与基线取行盒的 bracket_font 与 bracket_axis_pt。这样"要占位"与"要画"
 * 共用同一份数据，槽在就画得出，与那一行有没有字形无关，空字段也不例外。
 */
void EditorEngine::drawControlMarks() {
    const float zoom = cfg_.zoom;
    const int page_px_h = std::max(1, static_cast<int>(std::lround(cfg_.page.height_pt * zoom)));
    const int gap_px = std::max(0, static_cast<int>(std::lround(cfg_.page_gap_pt * zoom)));

    const float units_to_px = zoom / kFontScalePerPt;
    const float dot = std::max(1.0f, cfg_.control_dot_pt * zoom);

    // 括号
    for (const LineBox& lb : layout_.lines) {
        if (!lb.bracket_font) continue;
        const float off_y = static_cast<float>((lb.page - base_first_page_) * (page_px_h + gap_px));

        hb_font_t* hbf = lb.bracket_font->hb();
        int sx = 0, sy = 0;
        hb_font_get_scale(hbf, &sx, &sy);
        // 擦除用近似矩形（字形实际轮廓比它小）。宁可大一点 ——
        // 记小了会留下残影，记大了只是多写几十个像素。
        const float em_px = std::max(1.0f, static_cast<float>(sx) * units_to_px);
        const float up = em_px * 0.9f;
        const float dn = em_px * 0.5f;

        for (const LineCluster& c : lb.clusters) {
            if (!c.placeholder) continue;
            const bool open = (c.bracket_side != BracketSide::Close);
            hb_codepoint_t g = 0;
            const hb_codepoint_t want = static_cast<hb_codepoint_t>(open ? '[' : ']');
            if (!hb_font_get_nominal_glyph(hbf, want, &g) || !g) continue;
            const float adv = hb_font_get_glyph_h_advance(hbf, g) * units_to_px;
            if (!(adv > 0.0f)) continue;

            const Color col = c.read_only ? cfg_.control_mark_ro : cfg_.control_mark;

            // 槽 = [槽首, 槽首 + 括号advance + 间隙)（槽宽由排版给，等于簇的 advance）。
            //   字形要贴住槽里朝向内容的那一端，间隙才落在括号与内容之间：
            //     [ 槽：内容在右 → 字形贴槽首（间隙落在括号右边）
            //     ] 槽：内容在左 → 字形贴槽尾（间隙落在括号左边）
            //   间隙必须和排版用同一个常数（kControlBracketSlotGapPt）。
            //     各算各的 = 括号盖到相邻的字上，或者槽里空一条而字已经走了。
            const float along_pt =
                lb.inline_origin_pt + c.inline_pt + (open ? 0.0f : kControlBracketSlotGapPt);

            if (!lb.vertical) {
                // 横排：字形原点的 y 就是基线，行内轴是 x。
                const float x = along_pt * zoom;
                const float y = lb.bracket_axis_pt * zoom + off_y;
                bitmap_.drawGlyph(hbf, g, x, y, units_to_px, col, 0.0f);
                control_marks_prev_.push_back(Rect{x, y - up, adv, up + dn});
            } else {
                // 竖排：括号跟着转 90°（顺时针）。字形轮廓本身是横排形态，
                //   [ 转过来开口朝下 —— 正好是"内容继续往下排"的意思。
                //   不转的话括号会横躺在列里，开口方向与行内轴垂直（错的）。
                //   旋转后 advance 沿 +y，所以位置分量是 (列内横轴笔位, 行内轴)。
                const float x = lb.bracket_axis_pt * zoom;
                const float y = along_pt * zoom + off_y;
                bitmap_.drawGlyph(hbf, g, x, y, units_to_px, col, 90.0f);
                control_marks_prev_.push_back(Rect{x - up, y, up + dn, adv});
            }
        }
    }

    // All 档到这儿就结束了：所有字段常驻括号，没有小点。
    if (cfg_.form_brackets == FormBracketScope::All) return;

    // 小点（Focused 档：光标不在的字段）
    //
    // 与 canPlaceCaret 同一口径（include_end = true）：光标停在字段末尾
    // 也算"在这个字段里" —— 否则刚打到最后一个字时括号会突然消失，
    // 而那一刻正是最需要看到边界的时候。
    const int focused = controls_.empty() ? -1 : controls_.innermostAt(caret_, true);

    for (int ci = 0; ci < controls_.size(); ++ci) {
        if (ci == focused) continue;
        const Control& c = controls_.at(ci);
        const Color col = c.read_only ? cfg_.control_mark_ro : cfg_.control_mark;
        for (const ControlRect& cr : controlRects(ci)) {
            const float off_y =
                static_cast<float>((cr.page - base_first_page_) * (page_px_h + gap_px));
            // 小点：右下角、内缩一点
            const float x1 = (cr.r_pt.x + cr.r_pt.w) * zoom;
            const float y1 = (cr.r_pt.y + cr.r_pt.h) * zoom + off_y;
            const Rect box{x1 - dot, y1 - dot, dot, dot};
            bitmap_.fillRect(box, col);
            control_marks_prev_.push_back(box);
        }
    }
}

/** 光标：位置设置、字素簇吸附与光标矩形查询。 */

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
    const float off = lb->inlineOffsetFor(static_cast<uint32_t>(pos.offset));
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
    return caretRectFor(caret_, &page, out);
}

Rect EditorEngine::caretRectPx() const {
    int page = 0;
    Rect r{};
    if (!caretRectFor(caret_, &page, &r)) return Rect{};
    const float zoom = cfg_.zoom;
    const int page_px_h = std::max(1, static_cast<int>(std::lround(cfg_.page.height_pt * zoom)));
    const int gap_px = std::max(0, static_cast<int>(std::lround(cfg_.page_gap_pt * zoom)));
    const float off_y = static_cast<float>((page - base_first_page_) * (page_px_h + gap_px));
    Rect px;
    px.x = std::floor(r.x * zoom);
    px.y = std::floor(r.y * zoom + off_y);
    px.w = std::max(1.0f, std::ceil(r.w * zoom));
    px.h = std::max(1.0f, std::ceil(r.h * zoom));
    return px;
}

void EditorEngine::setCaret(int para, size_t offset) {
    if (blocks_.empty()) {
        caret_ = TextPos{0, 0};
        return;
    }
    if (para < 0) para = 0;
    if (para >= static_cast<int>(blocks_.size())) para = static_cast<int>(blocks_.size()) - 1;
    const int plan_before = bracketPlanId();
    TextPos p{para, snapToCluster(para, offset)};
    // 表单模式下光标只允许停在可编辑字段里。找不到就近的合法位置就原地不动 ——
    // 这比"退回到某个默认位置"安全：后者会让光标莫名其妙地跳到文档开头。
    if (!clampToAllowed(&p, /*dir=*/0)) p = caret_;
    caret_ = p;
    // 占位计划变了 → 必须重排（占位进文档流）。
    //   All 档下计划与光标无关 → 这一句恒为 no-op（光标移动是真增量的地基）。
    relayoutIfBracketPlanChanged(plan_before);
}

bool EditorEngine::moveCaretBy(int dcp) {
    if (blocks_.empty() || dcp == 0) return false;
    const int plan_before = bracketPlanId();

    TextPos p = caret_;
    if (p.para < 0) p.para = 0;
    if (p.para >= static_cast<int>(blocks_.size())) p.para = static_cast<int>(blocks_.size()) - 1;
    p.offset = snapToCluster(p.para, p.offset);

    const int steps = std::abs(dcp);
    for (int k = 0; k < steps; ++k) {
        const bool have_starts = p.para < static_cast<int>(layout_.para_cluster_starts.size()) &&
                                 !layout_.para_cluster_starts[static_cast<size_t>(p.para)].empty();
        const std::vector<uint32_t>& starts =
            have_starts ? layout_.para_cluster_starts[static_cast<size_t>(p.para)] : kEmptyStarts;

        // 先算"下一步"到一个临时量，确认可停才提交 —— 见循环末尾那条。
        TextPos next = p;
        if (dcp < 0) {
            if (next.offset > 0) {
                if (have_starts) {
                    const size_t prev = clusterFloor(starts, next.offset - 1);
                    next.offset = (prev < next.offset) ? prev : (next.offset - 1);
                } else {
                    --next.offset;
                }
            } else if (next.para > 0) {
                --next.para;
                next.offset = blocks_[static_cast<size_t>(next.para)].cp_text.size();
            } else {
                break;
            }
        } else {
            const size_t len = blocks_[static_cast<size_t>(next.para)].cp_text.size();
            if (next.offset < len) {
                if (have_starts) {
                    const size_t nx = clusterCeil(starts, next.offset);
                    next.offset = (nx > next.offset) ? nx : (next.offset + 1);
                } else {
                    ++next.offset;
                }
            } else if (next.para + 1 < static_cast<int>(blocks_.size())) {
                ++next.para;
                next.offset = 0;
            } else {
                break;
            }
        }

        // 表单模式：方向键不跨控件，下一步不可停就停在原地。
        //
        //   若改成"整段走完再投影到最近的合法位置"，投影不认方向只认距离，
        //   一步会变成瞬移到另一头（从 #2 起点按一次左键会跨过只读字段落到 #0）。
        //   投影适用于点击（给的是粗略的点，就近吸附是合理猜测），不适用于键盘
        //   （给的是明确指令，光标应一次一格、走不动就停）。因此 setCaret 继续用
        //   clampToAllowed，方向键走逐格停。
        //
        //   代价说清楚：字段之间不再能靠方向键走过去了（中间不是可停位置），
        //     要进另一个字段就用鼠标点。这是"不跨控件"的必然结果，不是 bug。
        if (!EditPolicy::canPlaceCaret(controls_, policy_.mode(), next)) break;
        p = next;
    }

    if (p == caret_) return false;
    caret_ = p;
    // 只有 Focused 档下跨字段移动才会走到"要重排"（All 档计划恒定）。
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::clampToAllowed(TextPos* pos, int dir) const {
    if (!pos) return false;
    // 策略允许 → 原样通过。这一句保证 Document / ReadOnly / 未配控件
    // 这三种情况下本函数是完全透明的（改动前后行为逐字节一致）。
    if (EditPolicy::canPlaceCaret(controls_, policy_.mode(), *pos)) return true;

    // 走到这里只可能是表单模式（canPlaceCaret 的语义见 edit_policy.h）。
    TextPos best{};
    bool found = false;
    auto dist = [](const TextPos& a, const TextPos& b) -> long long {
        const long long dp = static_cast<long long>(a.para) - static_cast<long long>(b.para);
        const long long dof = static_cast<long long>(a.offset) - static_cast<long long>(b.offset);
        return (dp < 0 ? -dp : dp) * 1000000LL + (dof < 0 ? -dof : dof);
    };
    auto consider = [&](const TextPos& cand) {
        if (!EditPolicy::canPlaceCaret(controls_, policy_.mode(), cand)) return;
        // 方向约束：dir > 0 只往文档后方找、< 0 只往前找（"停在边界"就是这么来的）；
        // dir == 0 就近取（鼠标点击 / setCaret 用）。
        if (dir > 0 && !(*pos < cand)) return;
        if (dir < 0 && !(cand < *pos)) return;
        if (!found || dist(cand, *pos) < dist(best, *pos)) {
            best = cand;
            found = true;
        }
    };
    // 候选 = 每个可编辑控件的两个端点。
    // 端点就够了：能走到这里说明 pos 在字段外，那么最近的合法可停处必定是
    // 某个字段的端点（字段内部的任何位置本来就合法，不会进这个函数）。
    for (int i = 0; i < controls_.size(); ++i) {
        const Control& c = controls_.at(i);
        if (c.read_only) continue;
        consider(c.range.start);
        consider(c.range.end);
    }
    if (!found) return false;
    *pos = best;
    return true;
}

bool EditorEngine::moveCaretToLineStart() {
    const LineBox* lb = findLineFor(caret_);
    if (!lb) return false;
    const int plan_before = bracketPlanId();
    TextPos p{lb->paragraph, lb->first_cp};
    // 表单模式：不跨控件。行首落在字段外时退到当前字段起点，不作"就近吸附"——
    //   后者会让 Home 把光标甩进另一个控件（在 #2 内按 Home 会落到 #0 的起点）。
    //   与 moveCaretBy 的"一步一格、走不动就停"同一条原则：移动不把光标送到
    //   当前可编辑区间之外。
    if (!clampToFieldEdge(&p, /*to_start=*/true)) return false;
    if (p == caret_) return false;
    caret_ = p;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

bool EditorEngine::moveCaretToLineEnd() {
    const LineBox* lb = findLineFor(caret_);
    if (!lb) return false;
    const int plan_before = bracketPlanId();
    TextPos p{lb->paragraph, lb->last_cp};
    if (!clampToFieldEdge(&p, /*to_start=*/false)) return false;
    if (p == caret_) return false;
    caret_ = p;
    relayoutIfBracketPlanChanged(plan_before);
    return true;
}

/**
 * Home/End 的收边：目标（行首/行尾）在当前可编辑字段之内就照常，
 * 否则退到本字段的起点/终点；光标本来就不在任何可编辑字段里 → 不动。
 * 为什么不去用 clampToAllowed：那个函数是"投影到最近的合法位置"，
 * 对点击是对的（点击给的是粗略的点），对键盘移动是错的（会把光标送到别的控件）。
 */
bool EditorEngine::clampToFieldEdge(TextPos* pos, bool to_start) const {
    if (EditPolicy::canPlaceCaret(controls_, policy_.mode(), *pos)) return true;
    if (policy_.mode() != EditMode::Form) return false;
    const int ci = controls_.empty() ? -1 : controls_.innermostAt(caret_, true);
    if (ci < 0) return false;  // 光标本来就不在字段里，没什么可退的
    const Control& c = controls_.at(ci);
    TextPos edge = to_start ? c.range.start : c.range.end;
    if (edge == caret_) return false;  // 已经在边缘，调用方会据此返回"没动"
    if (!EditPolicy::canPlaceCaret(controls_, policy_.mode(), edge)) return false;
    *pos = edge;
    return true;
}

void EditorEngine::showCaret(bool on) { caret_visible_ = on; }

/** 编辑：插入与删除，先过 EditPolicy 判定，再按字素簇吸附。 */

bool EditorEngine::insertCodepoints(int para, size_t offset, const std::vector<uint32_t>& cps) {
    if (cps.empty()) return false;
    if (para < 0 || para >= static_cast<int>(blocks_.size())) return false;
    // 插入前必须吸附：否则一串 e + U+0301 中间被插进字符，基字与标记就分家了。
    const size_t at = snapToCluster(para, offset);

    // 策略判定用的是吸附之后的位置 —— 判定用的位置和真正落笔的位置
    //   必须是同一个。否则会出现"策略放行了一个它根本没看过的位置"这种漏洞：
    //   吸附把 at 挪到簇边界之后，落笔点可能已经跨到另一个控件里去了。
    if (!EditPolicy::canInsert(controls_, policy_.mode(), TextPos{para, at})) {
        ++blocked_edits_;
        return false;
    }

    textbuf::insertAt(blocks_[static_cast<size_t>(para)].cp_text, at, cps);
    // 区间是活的：端点跟随插入移动（在控件内插则变长，在控件前插则平移）。
    //   漏掉这一步，控件会与它标注的内容脱节：既拒绝本该允许的输入，
    //   又允许本该拒绝的，且区间看起来仍然有效。
    adjustControlsOnInsert(controls_, para, at, cps.size());
    caret_ = TextPos{para, at + cps.size()};
    // 内容只在这一段变了：下一轮排版从这个段续排，前面的段原样复用。
    markLayoutDirty(para);
    return true;
}

bool EditorEngine::insertCodepoint(int para, size_t offset, uint32_t cp) {
    return insertCodepoints(para, offset, std::vector<uint32_t>{cp});
}

bool EditorEngine::insertTextUtf8(int para, size_t offset, const std::string& utf8) {
    return insertCodepoints(para, offset, utf8ToCodepoints(utf8));
}

bool EditorEngine::typeTextUtf8(const std::string& utf8) {
    return insertCodepoints(caret_.para, caret_.offset, utf8ToCodepoints(utf8));
}

bool EditorEngine::backspace() {
    if (blocks_.empty()) return false;
    TextPos p = caret_;
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
    if (!EditPolicy::canErase(controls_, policy_.mode(), TextPos{p.para, at},
                              TextPos{p.para, p.offset})) {
        ++blocked_edits_;
        return false;
    }
    const size_t erased =
        textbuf::eraseRange(blocks_[static_cast<size_t>(p.para)].cp_text, at, p.offset);
    if (erased == 0) return false;
    adjustControlsOnErase(controls_, p.para, at, p.offset);
    markLayoutDirty(p.para);
    caret_ = TextPos{p.para, at};
    return true;
}

bool EditorEngine::deleteForward() {
    if (blocks_.empty()) return false;
    TextPos p = caret_;
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

    if (!EditPolicy::canErase(controls_, policy_.mode(), TextPos{p.para, p.offset},
                              TextPos{p.para, next})) {
        ++blocked_edits_;
        return false;
    }

    const size_t erased =
        textbuf::eraseRange(blocks_[static_cast<size_t>(p.para)].cp_text, p.offset, next);
    if (erased == 0) return false;
    adjustControlsOnErase(controls_, p.para, p.offset, next);
    markLayoutDirty(p.para);
    caret_ = TextPos{p.para, p.offset};
    return true;
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
    TextPos p = caret_;
    if (p.para < 0 || p.para >= static_cast<int>(blocks_.size())) p.para = 0;
    p.offset = snapToCluster(p.para, p.offset);

    if (!EditPolicy::canSplit(controls_, policy_.mode(), p)) {
        ++blocked_edits_;
        return false;
    }

    BlockSpec& head = blocks_[static_cast<size_t>(p.para)];
    BlockSpec tail = head;  // 样式整体继承，只改文本与翻页标记
    tail.page_break_before = false;
    tail.cp_text.assign(head.cp_text.begin() + static_cast<ptrdiff_t>(p.offset),
                        head.cp_text.end());
    head.cp_text.resize(p.offset);

    // 控件端点先适配、再插段：适配函数按"段号 + 偏移"定位，段一插进去就对不上了。
    adjustControlsOnSplitPara(controls_, p);
    blocks_.insert(blocks_.begin() + p.para + 1, std::move(tail));

    markLayoutDirty(p.para);
    // 段数变了：后缀的段号整体后移，而"后缀复用"的判据是"第 from+1 段开始时的状态" ——
    // 现在第 from+1 段已经是另一段了，那个比较毫无意义（碰巧相等就会把整个后缀错当成
    // 没变）。所以段数一变就禁止复用，只保留"前缀复用 + 从脏段续排"。
    para_count_changed_ = true;
    caret_ = TextPos{p.para + 1, 0};
    return true;
}

/**
 * 合并的一般形式：把 head + 1 段并进 head 段。三种入口（段首退格 / 段末 Delete /
 * 显式调用）只是"从哪一端触发"不同，落到文档上的操作是同一个。
 */
bool EditorEngine::mergeParagraphInto(int head) {
    if (head < 0 || head + 1 >= static_cast<int>(blocks_.size())) return false;
    const size_t prev_len = blocks_[static_cast<size_t>(head)].cp_text.size();
    const TextPos join{head, prev_len};

    if (!EditPolicy::canMerge(controls_, policy_.mode(), join)) {
        ++blocked_edits_;
        return false;
    }

    BlockSpec& prev = blocks_[static_cast<size_t>(head)];
    const std::vector<uint32_t>& moved = blocks_[static_cast<size_t>(head + 1)].cp_text;
    prev.cp_text.insert(prev.cp_text.end(), moved.begin(), moved.end());
    // 端点适配在 erase 之前：它按段号定位，段一删就对不上了。
    adjustControlsOnMergePara(controls_, head, prev_len);
    blocks_.erase(blocks_.begin() + head + 1);

    markLayoutDirty(head);
    para_count_changed_ = true;  // 理由同分段
    caret_ = join;
    return true;
}

bool EditorEngine::mergeWithPreviousParagraph() {
    if (blocks_.empty()) return false;
    TextPos p = caret_;
    if (p.para <= 0 || p.para >= static_cast<int>(blocks_.size())) return false;
    // 只有光标确实在段首才合并（否则退格该删的是前一个簇）。
    if (snapToCluster(p.para, p.offset) != 0) return false;
    return mergeParagraphInto(p.para - 1);
}

bool EditorEngine::mergeWithNextParagraph() {
    if (blocks_.empty()) return false;
    TextPos p = caret_;
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
 */

void EditorEngine::setControls(std::vector<Control> list) {
    // 旧集合的最靠前段也要算进来：删掉一个控件同样要把它所在的段重排一次，
    // 否则那一处的括号占位会留在旧的排版结果里（括号不画了、宽度却还占着）。
    int dirty_lo = minControlParagraph();

    controls_.clear();
    for (const Control& c : list) controls_.add(c);
    controls_.normalize();

    const int lo_new = minControlParagraph();
    if (lo_new >= 0 && (dirty_lo < 0 || lo_new < dirty_lo)) dirty_lo = lo_new;
    if (dirty_lo >= 0) {
        markLayoutDirty(dirty_lo);
    } else {
        // 新旧都没有控件：占位集合恒为空，排版结果其实没变。这里按全量处理，
        // 是因为没有可定位的影响面 —— 用一次多余的排版换"不会漏"。
        markAllLayoutDirty();
    }

    // 控件参与排版了（字段的边界要占一个括号的宽度、把正文挤开），所以必须重排。
    //
    //   顺序：先把光标夹到新集合内、再重排 —— 反过来的话，第一次排版用的
    //   还是旧光标，占位名单就会错一个。
    //
    //   前提：调用本函数时 layout_ 必须已与 blocks_ 同步。
    //     刚 setBlocks 过却还没 relayout 的话，控件区间会投影到上一次的行盒上
    //     （症状："4 个字的字段给了 2 个矩形块"，探针当场抓到过）。
    //     规矩是：谁改了文档，谁负责 relayout。
    TextPos p = caret_;
    if (!clampToAllowed(&p, /*dir=*/0)) p = caret_;
    caret_ = p;
    relayout();

    // 标记画在帧上、不进底图 → 页指纹不受影响，只要求下一次整帧重贴。
    frame_needs_full_refresh_ = true;
}

void EditorEngine::clearControls() { setControls({}); }

void EditorEngine::setEditMode(EditMode m) {
    if (policy_.mode() == m) return;
    const int plan_before = bracketPlanId();
    policy_.setMode(m);

    TextPos p = caret_;
    if (!clampToAllowed(&p, /*dir=*/0)) p = caret_;
    caret_ = p;

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
            if (caret_ < s) {
                target = s;
                found = true;
                break;
            }
        }
        if (!found) target = starts.front();  // 到头绕回第一个（表单里 Tab 就该循环）
    } else {
        for (auto it = starts.rbegin(); it != starts.rend(); ++it) {
            if (*it < caret_) {
                target = *it;
                found = true;
                break;
            }
        }
        if (!found) target = starts.back();
    }
    TextPos p = target;
    if (!clampToAllowed(&p, /*dir=*/0)) return false;
    if (p == caret_) return false;
    const int plan_before = bracketPlanId();
    caret_ = p;
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
    h = fnvMix(h, static_cast<uint64_t>(static_cast<int>(policy_.mode())));
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
    TextPos p = caret_;
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
    if (!EditPolicy::canErase(controls_, policy_.mode(), TextPos{p.para, at},
                              TextPos{p.para, p.offset})) {
        ++blocked_edits_;
        return false;
    }
    const size_t erased =
        textbuf::eraseRange(blocks_[static_cast<size_t>(p.para)].cp_text, at, p.offset);
    if (erased == 0) return false;
    adjustControlsOnErase(controls_, p.para, at, p.offset);
    markLayoutDirty(p.para);
    caret_ = TextPos{p.para, at};
    return true;
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

/**
 * 帧自审：这一帧 == "整帧重画一遍" 的结果吗？
 * 为什么不满足于"画布 == 引擎帧"（selfCheck）：
 * 那个判据只能抓"上屏贴错"。如果引擎帧本身就是错的（比如某个旧标记
 * 没被擦掉），画布忠实反映了它，两边当然相等 —— 判据全绿而屏幕是错的。
 * 这正是"光标走了、旧方括号还在"那类问题一直没被抓住的原因之一。
 * 这里用的是和单测 [10]（字形缓存等价性）同一个套路：
 * 增量路径的结果，必须逐字节等于从零重来的结果。
 * "从零重来"由 frame_needs_full_refresh_ 强制：底图整块重建、
 * 整帧从底图搬、再画标记与光标 —— 那就是这一帧的"真值"。
 * 副作用（有意为之）：跑完这一帧就处于真值状态，因此它同时也是自愈手段。
 * 所以它对用户也安全：出问题时跑一次，画面立刻正确，同时拿到取证数据。
 */
std::string EditorEngine::auditFrameJson() {
    const int fw = frameWidth();
    const int fh = frameHeight();
    std::ostringstream o;
    if (fw <= 0 || fh <= 0) return "{\"diff\":-1,\"box\":null}";

    const size_t n = frameBytes();
    const std::vector<uint8_t> before(frame(), frame() + n);

    frame_needs_full_refresh_ = true;  // ← 强制"从零重来"
    repaint();

    const uint8_t* now = frame();
    long long diff = 0;
    int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
    for (int y = 0; y < fh; ++y) {
        for (int x = 0; x < fw; ++x) {
            const size_t off =
                (static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x)) * 4u;
            if (before[off] == now[off] && before[off + 1] == now[off + 1] &&
                before[off + 2] == now[off + 2]) {
                continue;
            }
            ++diff;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    o << "{\"diff\":" << diff << ",\"frameW\":" << fw << ",\"frameH\":" << fh;
    if (diff > 0) {
        o << ",\"box\":[" << minx << "," << miny << "," << maxx << "," << maxy << "]";
    } else {
        o << ",\"box\":null";
    }
    o << "}";
    return o.str();
}

namespace {

/**
 * 位置比较：整数/标志必须逐位相同，浮点坐标允许"远小于可见阈值"的容差。
 *
 * 为什么不能要求浮点也逐位相同：平移复用的坐标是"在旧坐标上加一个偏移"算出来的，
 * 而全量重排是"从 cursor_y 重新累加"出来的 —— 同一条折线两种加法，末位必然可能不同
 * （实测最大偏差 6.1e-05 pt）。除非把复用路径的算术写成与全量逐位一致，否则这不可消除。
 *
 * 容差取 1e-3 pt 的界线是量出来的：比舍入噪声大 16 倍（不会假红），
 * 又比引擎自己的可见阈值小 30 倍 —— 页指纹量化位置用 1/16 pt（见 quantPt），
 * 即 0.03 pt 以内的差别在引擎看来本来就"不算变化"。所以容差不会掩盖任何真错位：
 * 真正的错位是"整行挪错"，量级是几十磅。
 */
constexpr float kPosTol = 1e-3f;
inline bool samePos(float a, float b) { return std::fabs(a - b) <= kPosTol; }

/** 两个字形是否逐字段相同。font 比指针：同一个 FontLibrary 里同一张脸就是同一个指针。 */
bool sameGlyph(const PlacedGlyph& a, const PlacedGlyph& b) {
    return a.glyph_id == b.glyph_id && a.font == b.font && samePos(a.x_pt, b.x_pt) &&
           samePos(a.y_pt, b.y_pt) && a.page == b.page && a.line == b.line &&
           a.vertical == b.vertical && samePos(a.rotate_deg, b.rotate_deg) &&
           a.cluster == b.cluster && a.paragraph == b.paragraph && a.color.r == b.color.r &&
           a.color.g == b.color.g && a.color.b == b.color.b && a.color.a == b.color.a;
}

bool sameLine(const LineBox& a, const LineBox& b) {
    if (a.paragraph != b.paragraph || a.line != b.line || a.page != b.page ||
        a.vertical != b.vertical || a.sideways != b.sideways || !samePos(a.size_pt, b.size_pt) ||
        !samePos(a.block_lo_pt, b.block_lo_pt) || !samePos(a.block_hi_pt, b.block_hi_pt) ||
        !samePos(a.inline_origin_pt, b.inline_origin_pt) ||
        !samePos(a.inline_end_pt, b.inline_end_pt) || a.first_cp != b.first_cp ||
        a.last_cp != b.last_cp || a.bracket_font != b.bracket_font ||
        !samePos(a.bracket_axis_pt, b.bracket_axis_pt) || a.clusters.size() != b.clusters.size()) {
        return false;
    }
    for (size_t i = 0; i < a.clusters.size(); ++i) {
        const LineCluster& x = a.clusters[i];
        const LineCluster& y = b.clusters[i];
        if (x.cp != y.cp || !samePos(x.inline_pt, y.inline_pt) || x.placeholder != y.placeholder ||
            x.bracket_side != y.bracket_side || x.read_only != y.read_only) {
            return false;
        }
    }
    return true;
}

bool sameParaIndex(const ParaIndex& a, const ParaIndex& b) {
    return a.line_begin == b.line_begin && a.line_end == b.line_end &&
           a.glyph_begin == b.glyph_begin && a.glyph_end == b.glyph_end &&
           a.cluster_count == b.cluster_count && a.missing_count == b.missing_count &&
           a.font_run_count == b.font_run_count &&
           a.state_at_start.page_index == b.state_at_start.page_index &&
           a.state_at_start.line_total == b.state_at_start.line_total &&
           samePos(a.state_at_start.cursor_y, b.state_at_start.cursor_y);
}

/** 一次排版自审的结果。diff 是"不符的条目数"，不是"哪里有偏差"的近似。 */
struct LayoutAudit {
    int glyph_diff = 0;
    int line_diff = 0;
    int para_diff = 0;
    int cluster_starts_diff = 0;
    bool stats_same = true;
    int glyph_first = -1;
    int line_first = -1;
    int para_first = -1;

    int total() const {
        return glyph_diff + line_diff + para_diff + cluster_starts_diff + (stats_same ? 0 : 1);
    }
};

LayoutAudit auditLayoutPair(const LayoutResult& cur, const LayoutResult& ref) {
    LayoutAudit a;
    const size_t ng = std::min(cur.glyphs.size(), ref.glyphs.size());
    for (size_t i = 0; i < ng; ++i) {
        if (!sameGlyph(cur.glyphs[i], ref.glyphs[i])) {
            if (a.glyph_first < 0) a.glyph_first = static_cast<int>(i);
            ++a.glyph_diff;
        }
    }
    a.glyph_diff += static_cast<int>(cur.glyphs.size() > ref.glyphs.size()
                                         ? cur.glyphs.size() - ref.glyphs.size()
                                         : ref.glyphs.size() - cur.glyphs.size());

    const size_t nl = std::min(cur.lines.size(), ref.lines.size());
    for (size_t i = 0; i < nl; ++i) {
        if (!sameLine(cur.lines[i], ref.lines[i])) {
            if (a.line_first < 0) a.line_first = static_cast<int>(i);
            ++a.line_diff;
        }
    }
    a.line_diff +=
        static_cast<int>(cur.lines.size() > ref.lines.size() ? cur.lines.size() - ref.lines.size()
                                                             : ref.lines.size() - cur.lines.size());

    const size_t np = std::min(cur.para_index.size(), ref.para_index.size());
    for (size_t i = 0; i < np; ++i) {
        if (!sameParaIndex(cur.para_index[i], ref.para_index[i])) {
            if (a.para_first < 0) a.para_first = static_cast<int>(i);
            ++a.para_diff;
        }
    }
    a.para_diff += static_cast<int>(cur.para_index.size() > ref.para_index.size()
                                        ? cur.para_index.size() - ref.para_index.size()
                                        : ref.para_index.size() - cur.para_index.size());

    const size_t ns = std::min(cur.para_cluster_starts.size(), ref.para_cluster_starts.size());
    for (size_t i = 0; i < ns; ++i) {
        if (cur.para_cluster_starts[i] != ref.para_cluster_starts[i]) ++a.cluster_starts_diff;
    }
    a.cluster_starts_diff +=
        static_cast<int>(cur.para_cluster_starts.size() > ref.para_cluster_starts.size()
                             ? cur.para_cluster_starts.size() - ref.para_cluster_starts.size()
                             : ref.para_cluster_starts.size() - cur.para_cluster_starts.size());

    const LayoutStats& x = cur.stats;
    const LayoutStats& y = ref.stats;
    a.stats_same =
        (x.page_count == y.page_count && x.glyph_count == y.glyph_count &&
         x.placed_line_count == y.placed_line_count && x.cluster_count == y.cluster_count &&
         x.missing_glyph_count == y.missing_glyph_count && x.font_run_count == y.font_run_count);
    return a;
}

}  // namespace

/**
 * 排版自审：拿"不用缓存的全量重排"当对照，与当前 layout_ 逐字段比。
 * 为什么要比逐字段而不是只看行数/字形数：增量最典型的错法是"少排了一段"
 * 或者"前缀多留了一段"，两者都只差几行 —— 看总数看不出来，看内容才看得见。
 */
int EditorEngine::auditLayout() {
    if (blocks_.empty()) return 0;
    const std::vector<int> bracketed = bracketedControls();
    const LayoutResult ref = layoutDocument(fonts_, blocks_, cfg_.page, &controls_, &bracketed);
    return auditLayoutPair(layout_, ref).total();
}

std::string EditorEngine::auditLayoutJson() {
    std::ostringstream o;
    if (blocks_.empty()) return "{\"diff\":0,\"note\":\"empty\"}";

    const auto t0 = Clock::now();
    const std::vector<int> bracketed = bracketedControls();
    const LayoutResult ref = layoutDocument(fonts_, blocks_, cfg_.page, &controls_, &bracketed);
    const double full_ms = msSince(t0);
    const LayoutAudit a = auditLayoutPair(layout_, ref);

    o << "{\"diff\":" << a.total();
    o << ",\"glyphDiff\":" << a.glyph_diff << ",\"lineDiff\":" << a.line_diff;
    o << ",\"paraDiff\":" << a.para_diff << ",\"startsDiff\":" << a.cluster_starts_diff;
    o << ",\"statsSame\":" << (a.stats_same ? "true" : "false");
    o << ",\"first\":{\"glyph\":" << a.glyph_first << ",\"line\":" << a.line_first
      << ",\"para\":" << a.para_first << "}";
    o << ",\"glyphs\":" << layout_.glyphs.size() << ",\"lines\":" << layout_.lines.size();
    o << ",\"paras\":" << layout_.para_index.size();
    o << ",\"relaidParagraphs\":" << last_relaid_paragraphs_;
    o << ",\"relaidLines\":" << last_relaid_lines_;
    o << ",\"layoutWasFull\":" << (last_layout_full_ ? "true" : "false");
    // 两个耗时都来自本次调用：curMs 是刚才那次增量续排，fullMs 是审计顺便做的
    // 全量重排 —— 分母与分子同源，不掺历史数字。
    o << ",\"curMs\":" << layout_.stats.layout_ms << ",\"fullMs\":" << full_ms;
    o << "}";
    return o.str();
}
/**
 * 上屏契约：待上屏脏矩形累加器
 * 详见 engine.h 里 takeBlitBox 的注释。一句话：宿主需要的不是"这一轮写了多少"
 * （那是面板的数字），而是"自上次上屏以来一共写过哪些区域"。
 * 一次用户操作可能触发两次 present，用后一轮的账去上屏就会漏掉前一轮。
 */
void EditorEngine::noteBlitDirty(const Rect& r) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    if (!blit_acc_any_) {
        blit_acc_ = r;
        blit_acc_any_ = true;
        return;
    }
    const float x1 = std::max(blit_acc_.x + blit_acc_.w, r.x + r.w);
    const float y1 = std::max(blit_acc_.y + blit_acc_.h, r.y + r.h);
    blit_acc_.x = std::min(blit_acc_.x, r.x);
    blit_acc_.y = std::min(blit_acc_.y, r.y);
    blit_acc_.w = x1 - blit_acc_.x;
    blit_acc_.h = y1 - blit_acc_.y;
}

Rect EditorEngine::takeBlitBox() {
    const int fw = frameWidth();
    const int fh = frameHeight();
    Rect out{};
    if (blit_acc_any_ && blit_acc_.w > 0.0f && blit_acc_.h > 0.0f && fw > 0 && fh > 0) {
        const int x0 = std::max(0, static_cast<int>(std::floor(blit_acc_.x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(blit_acc_.y)));
        const int x1 = std::min(fw, static_cast<int>(std::ceil(blit_acc_.x + blit_acc_.w)));
        const int y1 = std::min(fh, static_cast<int>(std::ceil(blit_acc_.y + blit_acc_.h)));
        // 夹完之后若是空集，说明累加器和当前帧几何对不上（比如 frame 刚缩过）
        // —— 那就退化成整帧：多贴一块是性能问题，漏贴是正确性问题。
        if (x1 <= x0 || y1 <= y0) {
            out = Rect{0.0f, 0.0f, static_cast<float>(fw), static_cast<float>(fh)};
        } else {
            out = Rect{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0),
                       static_cast<float>(y1 - y0)};
        }
    }
    blit_acc_ = Rect{};
    blit_acc_any_ = false;
    return out;
}

void EditorEngine::repaintAfterEdit() {
    // 编辑之后光标必须立即可见。这不仅是观感，也是宿主记账的依赖：
    //   editStats.caretVisible 为 false 时宿主会再 present 一次，并把上屏矩形换成
    //   那一轮的，于是编辑那次的全帧重绘被丢掉（auditFrame 差 0、selfCheck 差上千
    //   像素，画布停在旧排版上）。
    //   该不变量属于引擎：放在宿主里就成了补救路径，而补救路径本身会引入记账歧义。
    //   顺带重置闪烁相位，符合输入法/编辑器的常见手感。
    caret_visible_ = true;
    repaint();
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

std::string EditorEngine::describeControls() const {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << "edit-mode=" << editModeName(policy_.mode()) << "  controls=" << controls_.size()
       << "  blocked-edits=" << blocked_edits_ << "\n";
    for (int i = 0; i < controls_.size(); ++i) {
        const Control& c = controls_.at(i);
        os << "  #" << i << " " << controlKindName(c.kind) << " id=" << (c.id.empty() ? "-" : c.id)
           << " range=[" << c.range.start.para << ":" << c.range.start.offset << ","
           << c.range.end.para << ":" << c.range.end.offset << ")"
           << (c.read_only ? " read-only" : " editable") << " depth=" << c.depth
           << " parent=" << c.parent << " boxes=" << controlRects(i).size() << "\n";
    }
    os << "  caret-control="
       << (controls_.empty() ? -1 : controls_.innermostAt(caret_, /*include_end=*/true)) << "\n";
    return os.str();
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

/** 命中测试：几何的反向半边 —— 帧坐标反查 (段, 偏移)。 */

bool EditorEngine::hitTestPt(float x_pt_frame, float y_pt_frame, TextPos* out) const {
    if (!out || layout_.lines.empty()) return false;

    const float page_w = cfg_.page.width_pt;
    const float page_h = cfg_.page.height_pt;
    const float stride = page_h + cfg_.page_gap_pt;

    int page = 0;
    float local_y = y_pt_frame;
    if (stride > 0.0f) {
        page = static_cast<int>(std::floor(y_pt_frame / stride));
        if (page < 0) page = 0;
        local_y = y_pt_frame - static_cast<float>(page) * stride;
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
    if (rel < 0.0f) {
        cp = n ? best->clusters.front().cp : best->first_cp;
    } else {
        for (size_t i = 0; i < n; ++i) {
            const float a = best->clusters[i].inline_pt;
            const float b = (i + 1 < n) ? best->clusters[i + 1].inline_pt : best->inline_end_pt;
            if (rel < (a + b) * 0.5f) {
                cp = best->clusters[i].cp;
                break;
            }
        }
    }

    TextPos p{best->paragraph, cp};
    // 表单模式下点到字段之外的正文时，就近吸附到最近的字段 ——
    // 而不是把光标丢在一个"点得到、却改不了"的位置上（那看起来像程序坏了）。
    clampToAllowed(&p, /*dir=*/0);
    *out = p;
    return true;
}

/** 诊断：把内部状态摊平成可读文本，供宿主与探针取证。 */

std::string EditorEngine::describeCaret() const {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(3);
    os << "caret: para=" << caret_.para << " offset(段内码点)=" << caret_.offset
       << " 全文下标=" << edit_stats_.caret_index << " 全文码点=" << documentCodepoints() << "\n";

    const LineBox* lb = findLineFor(caret_);
    if (!lb) {
        os << "  找不到行盒（该段没有可排内容）\n";
        return os.str();
    }
    os << "  行盒: line=" << lb->line << " page=" << lb->page << (lb->vertical ? " 竖排" : " 横排")
       << (lb->sideways ? "(侧倒)" : "") << "\n";
    os << "  段内码点范围=[" << lb->first_cp << ", " << lb->last_cp
       << ")  簇数=" << lb->clusters.size() << "\n";
    os << "  块轴=[" << lb->block_lo_pt << ", " << lb->block_hi_pt
       << "] pt  行内起点=" << lb->inline_origin_pt << " pt  行内末位=" << lb->inline_end_pt
       << " pt\n";
    os << "  簇表(cp→沿轴相对位置):";
    for (size_t i = 0; i < lb->clusters.size() && i < 12; ++i)
        os << " [" << lb->clusters[i].cp << "→" << lb->clusters[i].inline_pt << "]";
    if (lb->clusters.size() > 12) os << " ...";
    os << "\n";
    os << "  本偏移的沿轴位置=" << lb->inlineOffsetFor(static_cast<uint32_t>(caret_.offset))
       << " pt\n";

    Rect r{};
    if (caretRectPt(&r))
        os << "  光标矩形(页面磅): x=" << r.x << " y=" << r.y << " w=" << r.w << " h=" << r.h
           << "\n";
    const Rect px = caretRectPx();
    os << "  光标矩形(帧像素): x=" << px.x << " y=" << px.y << " w=" << px.w << " h=" << px.h
       << "\n";
    return os.str();
}

std::string EditorEngine::describeShaping(int font_index, float size_pt, const std::string& text,
                                          const WritingMode& wm, const char* language) {
    std::ostringstream os;
    FontInstance* f = fonts_.instance(font_index, size_pt);
    if (!f) {
        os << "font instance unavailable\n";
        return os.str();
    }
    const ShapedText st = shapeText(f->hb(), text, wm, language);

    // 字体原始度量：符号约定靠这个确认，不靠猜
    hb_font_extents_t he{};
    const bool hasHe = hb_font_get_h_extents(f->hb(), &he);
    os << "font scale units per pt = " << kFontScalePerPt << "\n";
    os << "h_extents: ok=" << (hasHe ? 1 : 0) << " ascender=" << he.ascender
       << " descender=" << he.descender << " line_gap=" << he.line_gap
       << "   (磅: " << (he.ascender / kFontScalePerPt) << " / " << (he.descender / kFontScalePerPt)
       << ")\n";
    os << "writing-mode=" << wm.name() << "  glyphs=" << st.glyphs.size()
       << "  inline_advance_pt=" << (st.inline_advance / kFontScalePerPt) << "\n";

    for (size_t i = 0; i < st.glyphs.size() && i < 4; ++i) {
        hb_glyph_extents_t ge{};
        const bool hasGe = hb_font_get_glyph_extents(f->hb(), st.glyphs[i].glyph_id, &ge);
        hb_position_t vox = 0, voy = 0;
        hb_font_get_glyph_v_origin(f->hb(), st.glyphs[i].glyph_id, &vox, &voy);

        BBoxCollector bb;
        hb_font_draw_glyph(f->hb(), st.glyphs[i].glyph_id, bboxFuncs(), &bb);

        os << "  glyph[" << i << "] gid=" << st.glyphs[i].glyph_id
           << "  extents(ok=" << (hasGe ? 1 : 0) << "): xb=" << ge.x_bearing
           << " yb=" << ge.y_bearing << " w=" << ge.width << " h=" << ge.height << "   v_origin=("
           << vox << ", " << voy << ")\n";
        os << "            轮廓包围盒(scale单位): x∈[" << bb.minx << ", " << bb.maxx << "] y∈["
           << bb.miny << ", " << bb.maxy << "]"
           << "   即 y∈[" << (bb.miny / kFontScalePerPt) << ", " << (bb.maxy / kFontScalePerPt)
           << "] 磅\n";
    }

    for (size_t i = 0; i < st.glyphs.size(); ++i) {
        const ShapedGlyph& g = st.glyphs[i];
        os << "  [" << i << "] gid=" << g.glyph_id << " cluster(码点)=" << g.cluster
           << " x_adv=" << (g.x_advance / kFontScalePerPt)
           << " y_adv=" << (g.y_advance / kFontScalePerPt)
           << " x_off=" << (g.x_offset / kFontScalePerPt)
           << " y_off=" << (g.y_offset / kFontScalePerPt) << "\n";
    }
    return os.str();
}

/** 样例文书：单测与探针共用的固定内容。 */

BlockSpec mixedParagraphBlock(int cjk_font_index, int latin_font_index) {
    BlockSpec b;
    b.font_index = cjk_font_index;
    b.size_pt = 12.5f;
    b.wm = WritingMode::HorizontalTB();
    b.line_factor = 1.8f;
    b.language = "zh";
    b.color = Color::rgb(24, 24, 24);
    // 拉丁字体挂回退：NotoSansSC-VF 有 ¹²³（U+00B9/B2/B3），
    //   但没有 ⁰⁹（U+2070/U+2079）——而"10⁹/L"是检验报告的标准写法。
    if (latin_font_index >= 0) b.fallback_font_indices.push_back(latin_font_index);
    b.setText(
        "体格检查：T 36.8℃，P 92 次/分，R 20 次/分，BP 138/86 mmHg。"
        "辅助检查：2026-10-01 血常规示 WBC 11.2×10⁹/L，N 78.5%，Hb 132 g/L，PLT 216×10⁹/L；"
        "心电图示 P-R 间期 0.12 s，QRS 波群时限 0.08 s，QTc 0.41 s；"
        "心脏彩超示 LVEF 62%，LA 38 mm，LVEDD 48 mm。"
        "诊断：1. Paroxysmal Supraventricular Tachycardia；2. Hypertension grade 2 (high risk)；"
        "3. Type 2 Diabetes Mellitus。"
        "处置：Amiodarone 150 mg iv 缓慢推注，继以 1 mg/min 静脉维持 6 h，后 0.5 mg/min 维持 18 h；"
        "Metoprolol 25 mg po bid；复查 ECG、电解质及甲状腺功能。"
        "医师签名：______________   日期：2026-10-09");
    return b;
}

std::vector<BlockSpec> sampleDocument(int cjk_font_index, int mongol_font_index,
                                      int latin_font_index) {
    std::vector<BlockSpec> blocks;

    // 第 1 页：中文横排
    BlockSpec zh;
    zh.font_index = cjk_font_index;
    zh.size_pt = 12.0f;
    zh.wm = WritingMode::HorizontalTB();
    zh.line_factor = 1.75f;
    zh.language = "zh";
    zh.color = Color::rgb(24, 24, 24);
    zh.setText(
        "患者主诉：反复心悸3天，加重1天。"
        "现病史：患者3天前无明显诱因出现心悸，伴胸闷、气短，活动后加重，休息后稍缓解，"
        "无黑矇、晕厥，无胸痛、咯血。既往史：高血压病史10年，规律服用氨氯地平5mg每日一次，"
        "血压控制在130/80mmHg左右；2型糖尿病病史6年，口服二甲双胍0.5g每日三次。"
        "体格检查：体温36.8℃，脉搏92次/分，呼吸20次/分，血压138/86mmHg。神志清楚，查体合作，"
        "双肺呼吸音清，未闻及干湿性啰音。心界不大，心率92次/分，律齐，"
        "各瓣膜听诊区未闻及病理性杂音。腹软，无压痛及反跳痛。双下肢无水肿。"
        "初步诊断：1. 心律失常 阵发性室上性心动过速；2. 高血压病2级（高危）；3. 2型糖尿病。"
        "医师签名：______________");
    blocks.push_back(std::move(zh));

    // 第 2 页：蒙文竖排，列序从左往右（vertical-lr）
    std::string mn;
    for (int i = 0; i < 14; ++i) {
        if (i) mn += " ";
        // "\u182E\u1823\u1829\u182D\u1823\u182F" = "ᠮᠣᠩᠭᠣᠯ"（蒙古）
        // "\u182A\u1822\u1834\u1822\u182D"         = "ᠪᠢᠴᠢᠭ"（文字/script）
        mn += "\u182E\u1823\u1829\u182D\u1823\u182F \u182A\u1822\u1834\u1822\u182D";
    }
    BlockSpec mongo_lr;
    mongo_lr.font_index = mongol_font_index;
    mongo_lr.size_pt = 16.0f;
    mongo_lr.wm = WritingMode::VerticalLR();
    mongo_lr.line_factor = 1.8f;
    mongo_lr.language = "mn";
    mongo_lr.page_break_before = true;
    mongo_lr.color = Color::rgb(20, 60, 130);
    mongo_lr.setText(mn);
    blocks.push_back(std::move(mongo_lr));

    // 第 3 页：中文竖排（vertical-rl，列序从右往左，字形正立）
    //
    // 这才是 VerticalRL 的真实用例：中日竖排走 HarfBuzz 的 TTB + vert/vrt2 + vmtx，
    // 字形本身保持正立，列序从右往左 —— 与上一页蒙文"侧倒 + 列序从左往右"正好相反。
    // （把蒙文放进这条通道是错的：见 writing_mode.h 里 isSideways() 的说明。）
    BlockSpec zh_v;
    zh_v.font_index = cjk_font_index;
    zh_v.size_pt = 15.0f;
    zh_v.wm = WritingMode::VerticalRL();
    zh_v.line_factor = 1.9f;
    zh_v.language = "zh";
    zh_v.page_break_before = true;
    zh_v.color = Color::rgb(150, 60, 30);
    zh_v.setText(
        "患者主诉：反复心悸3天，加重1天。现病史：患者3天前无明显诱因出现心悸，"
        "伴胸闷、气短，活动后加重，休息后稍缓解。既往史：高血压病史10年。"
        "初步诊断：心律失常、阵发性室上性心动过速。");
    blocks.push_back(std::move(zh_v));

    // 第 4 页：中英文混排
    BlockSpec mixed = mixedParagraphBlock(cjk_font_index, latin_font_index);
    mixed.page_break_before = true;
    blocks.push_back(std::move(mixed));

    return blocks;
}

}  // namespace ced
