/**
 * 排版调度与增量。
 *
 * 排版算法本身在 core/layout/：这里只管"什么时候排、排哪一段、排完怎么复用后缀"，
 * 也就是脏段记账（layout_dirty_from_）、页指纹、后缀两级复用与统计同步。
 * 与算法分开的理由是两者的变更理由不同 —— 算法随排版规则变，这里随笔触与增量契约变。
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

// 页内容指纹
//
// 用途：判断"这一页的画面对不对"。指纹没变就跳过重画 —— 这是光栅增量的全部依据。
// 要求不高：不是密码学强度，只要求"内容变了几乎一定会变"。
//
// 混合器（kFnvBasis / fnvMix）挪去了 core/utils/hash.h：现在排版层也要用它算
//   段级整形指纹，两处必须是同一套混合（各写一份的话，"哪一处换了算法"
//   会静默地退化成"缓存永不命中"或更糟的"两处对同一输入给出不同指纹"）。

/**
 * 布局：重排整篇，并把排版统计同步到 EngineStats。
 */

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
    const FontCacheTotals fc = fonts_.cacheTotals();
    stats_.font_cache_hits = static_cast<long long>(fc.hits);
    stats_.font_cache_misses = static_cast<long long>(fc.misses);
    stats_.font_storage_reads = static_cast<long long>(fc.storage_reads);
    stats_.font_storage_bytes = static_cast<long long>(fc.storage_bytes);
    stats_.font_io_ms = fc.io_ms;
    stats_.font_resident_bytes = static_cast<long long>(fc.resident_bytes);
    stats_.font_storage_failures = static_cast<long long>(fc.read_failures);
    stats_.font_storage_eofs = static_cast<long long>(fc.eof_reads);
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
    // 真指纹一律清掉哨兵位（见 kFpDirtyBit）：这一位只在 markPagesChangedFrom 里置起来，
    //   于是"标脏"与"真值"在编码上不相交。别的页保留原值 —— 它们可能正是标脏值。
    for (int p = dirty_lo; p <= dirty_hi; ++p) page_fp_new_[static_cast<size_t>(p)] &= ~kFpDirtyBit;

    // 表单控件与编辑模式刻意不进页指纹。
    //
    //   页指纹是"底图该不该重画"的判据，而底图里只有页面底色与字形 ——
    //   控件标记（小点 / 方括号）画在帧上、不进底图（原因见 presentInternal）。
    //   既然底图不依赖控件与模式，把它们混进来就只是无谓地重画所有页。
    //
    //   切模式时旧标记的清除靠 frame_needs_full_refresh_：切模式 / 改控件时置位，
    //     下一次呈现整帧重贴，旧标记自然被覆盖。这比往指纹里塞东西更直接，
    //     也不影响底图的增量。

    computeLineFingerprints(dirty_lo, dirty_hi, !partial);
}

/**
 * 行指纹与行盒几何快照。
 *
 * 与页指纹同一次调用里做，理由有两条：一是同一批字形不必遍历两遍；
 * 二是"哪几页脏"和"这几页里哪几行脏"必须是同一轮排版的两面 —— 分成两次算，
 * 两次之间排版若再动一次，两边就对不上了。
 *
 * 指纹含三样：行盒几何（位置挪了也算变）、簇表（占位槽与括号侧别都在里面）、
 * 这一行的字形（id / 位置 / 朝向 / 颜色）。少一样就会出现"字形没重画但确实变了"的
 * 幽灵墨迹，而 auditFrame（当前帧 vs 整帧重画）正是抓它的判据。
 */
void EditorEngine::computeLineFingerprints(int dirty_lo, int dirty_hi, bool full) {
    const int n_lines = static_cast<int>(layout_.lines.size());
    const bool partial = !full && dirty_lo >= 0 && dirty_hi >= dirty_lo &&
                         line_fp_new_.size() == static_cast<size_t>(n_lines);
    if (!partial) {
        line_fp_new_.assign(static_cast<size_t>(n_lines), kFnvBasis);
        dirty_lo = 0;
        dirty_hi = std::max(0, layout_.stats.page_count - 1);
    }
    line_fp_new_.resize(static_cast<size_t>(n_lines), kFnvBasis);

    const std::vector<PlacedGlyph>& gs = layout_.glyphs;
    for (int li = 0; li < n_lines; ++li) {
        const LineBox& lb = layout_.lines[static_cast<size_t>(li)];
        if (lb.page < dirty_lo || lb.page > dirty_hi) continue;
        uint64_t h = kFnvBasis;
        auto mixf = [&h](float v) {
            h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(quantPt(v))));
        };
        mixf(lb.block_lo_pt);
        mixf(lb.block_hi_pt);
        mixf(lb.inline_origin_pt);
        mixf(lb.inline_end_pt);
        h = fnvMix(h, static_cast<uint64_t>(lb.page));
        h = fnvMix(h, static_cast<uint64_t>(lb.paragraph));
        h = fnvMix(h, static_cast<uint64_t>(lb.first_cp));
        h = fnvMix(h, static_cast<uint64_t>(lb.last_cp));
        h = fnvMix(h, (lb.vertical ? 1ull : 0ull) | (lb.sideways ? 2ull : 0ull));
        for (const LineCluster& c : lb.clusters) {
            h = fnvMix(h, static_cast<uint64_t>(c.cp));
            mixf(c.inline_pt);
            h = fnvMix(h, (c.placeholder ? 1ull : 0ull) | (c.read_only ? 2ull : 0ull) |
                              (static_cast<uint64_t>(c.bracket_side) << 2));
        }
        // 这一行的字形：按 line 单调，二分定位这一段
        const auto gb = std::lower_bound(
            gs.begin(), gs.end(), li, [](const PlacedGlyph& g, int line) { return g.line < line; });
        for (auto it = gb; it != gs.end() && it->line == li; ++it) {
            h = fnvMix(h, it->glyph_id);
            mixf(it->x_pt);
            mixf(it->y_pt);
            mixf(it->rotate_deg);
            h = fnvMix(h, static_cast<uint64_t>(it->color.r) |
                              (static_cast<uint64_t>(it->color.g) << 8) |
                              (static_cast<uint64_t>(it->color.b) << 16));
        }
        line_fp_new_[static_cast<size_t>(li)] = h & ~kFpDirtyBit;
    }
}

/**
 * 把 [first_page, 末页) 直接标成"与底图那一版不同"，不去哈希（见声明处的说明）。
 *
 * 标脏值的高位恒置：真指纹一律清掉那一位，所以它与任何真指纹都不可能相等 ——
 * 这不是"大概率不同"，而是编码上就不相交。
 *
 * 低位还要按底图那一版翻一下：present 之后底图也会变成标脏值，下一次标脏若写回
 *   同一个值，判据读出来就是"没变"，那一页从此不再重画（症状：连续换行只有第一次
 *   把文档推下去，之后屏幕不动）。
 * 行指纹同一把尺子、同一批页：那些页上的每一行都跟着挪了，行带要覆盖整页。
 */
void EditorEngine::markPagesChangedFrom(int first_page) {
    const int first = std::max(0, first_page);
    const int n = std::max(1, layout_.stats.page_count);
    page_fp_new_.resize(static_cast<size_t>(n), kFnvBasis);
    page_fp_new_count_ = n;
    // 标脏值：高位恒置（真指纹一律清它，所以它不可能等于真指纹），
    //   低位按底图那一版翻一下 —— 这一下是必须的，见下面那段注释。
    auto dirty_value = [](uint64_t base) {
        const uint64_t v = kFnvBasis | kFpDirtyBit;
        return (v == base) ? (v ^ 1ull) : v;
    };
    for (int p = first; p < n; ++p) {
        const uint64_t base =
            (p < page_fp_base_count_) ? page_fp_in_base_[static_cast<size_t>(p)] : 0;
        page_fp_new_[static_cast<size_t>(p)] = dirty_value(base);
    }

    // 行数组只标 [first_page, 末页) 里的行：前缀那些行一个字都没动，标脏只会让它们多画一遍。
    //   linesOnPage 给的 lo 就是"页号不小于 first 的第一行"，正是这里的起点
    //   （这一页上没有行时它会指向页号更大的第一行，同样是对的）。
    const int n_lines = static_cast<int>(layout_.lines.size());
    line_fp_new_.resize(static_cast<size_t>(n_lines), kFnvBasis);
    int lo = 0;
    int hi = 0;
    linesOnPage(first, &lo, &hi);
    (void)hi;
    for (int k = lo; k < n_lines; ++k) {
        const uint64_t base =
            (k < line_fp_base_count_) ? line_fp_in_base_[static_cast<size_t>(k)] : 0;
        line_fp_new_[static_cast<size_t>(k)] = dirty_value(base);
    }
}

/**
 * 第 p 段的第一行落在哪一页；这一段没有行盒时返回 -1。
 */
int EditorEngine::firstPageOfParagraph(int p) const {
    if (p < 0 || p >= static_cast<int>(layout_.para_index.size())) return -1;
    const ParaIndex& pi = layout_.para_index[static_cast<size_t>(p)];
    if (pi.line_begin < 0 || pi.line_begin >= pi.line_end) return -1;
    if (pi.line_begin >= static_cast<int>(layout_.lines.size())) return -1;
    return layout_.lines[static_cast<size_t>(pi.line_begin)].page;
}

/**
 * 第 page 页上的行下标范围（闭区间）。这一页没有行时把 hi 置成 lo-1。
 * 行数组按页单调（与字形数组同一条不变式），所以两端都能二分。
 */
void EditorEngine::linesOnPage(int page, int* lo, int* hi) const {
    const std::vector<LineBox>& ls = layout_.lines;
    const auto b = std::lower_bound(ls.begin(), ls.end(), page,
                                    [](const LineBox& l, int p) { return l.page < p; });
    if (b == ls.end() || b->page != page) {
        *lo = static_cast<int>(b - ls.begin());
        *hi = *lo - 1;
        return;
    }
    *lo = static_cast<int>(b - ls.begin());
    int i = *lo;
    while (i < static_cast<int>(ls.size()) && ls[static_cast<size_t>(i)].page == page) ++i;
    *hi = i - 1;
}

void EditorEngine::relayout() {
    const auto t0 = Clock::now();

    // 顺序有讲究：先夹光标、再写对象表 —— 因为"哪些字段要显示括号"在 Focused 档下
    //   由光标位置决定（见 bracketedControls），表写完才是排版的输入。
    //
    //   这里只夹数值范围，不吸附簇：簇吸附需要簇表，而此刻尚未排版；用上一轮的簇表
    //     会把光标拉回旧簇边界，表现为"敲字后光标差一个位置"。簇吸附放在排版之后。
    if (!blocks_.empty()) {
        int para = caretPara();
        if (para >= static_cast<int>(blocks_.size()))
            para = static_cast<int>(blocks_.size()) - 1;
        const size_t len = blocks_[static_cast<size_t>(para)].cp_text.size();
        const BracketCounts bc = bracketCountsAt(para, len);
        const int span = static_cast<int>(len) + bc.before + bc.closes + bc.opens;
        if (caretRel() > span) setCaretRel(para, span);
    } else {
        setCaretRel(0, 0);
    }

    // 把要显示括号的那些字段的两个边界写进块流的行内对象表：排版据此插入
    //   "括号占位"（占宽度、不产生字形），于是断行/行盒长度/簇位置全把括号算进去
    //   —— 这就是"括号进文档流"。不显示的就不进表（规则：显示才占位）。
    //
    // 名单仍由引擎算（bracketedControls，只有它认识"聚焦"），但它落成的是块流里的
    //   一项，排版层拿到的是一份自足的输入：要画与要占位是同一条记录，分不开。
    syncInlineObjects();
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
    // 段数变了也照样试后缀复用：回车分段与退格合并一个字都没改内容，只挪了段边界。
    int relaid_paras = 1;
    // 后缀整体平移：后缀在的每一页内容都跟着挪了 —— 这个答案已知，不必再去哈希它们。
    bool pages_known_changed = false;
    if (!must_full && from >= 0 && from < n_para) {
        reuse = trySuffixReuse(from, &fp_lo, &fp_hi, &relaid_paras, &pages_known_changed);
    }
    if (reuse != SuffixReuse::None) {
        const ParaIndex& head = layout_.para_index[static_cast<size_t>(from)];
        const ParaIndex& tail = layout_.para_index[static_cast<size_t>(from + relaid_paras - 1)];
        last_relaid_paragraphs_ = relaid_paras;
        last_reused_paragraphs_ = n_para - relaid_paras - from;
        last_suffix_shifted_ = (reuse == SuffixReuse::Shifted);
        last_relaid_lines_ = tail.line_end - head.line_begin;
        last_layout_full_ = false;
    } else if (can_resume) {
        // 脏段在旧版里落在哪一页 —— 截断会把这一段从段表里摘掉，所以先读。
        const int old_first_page = firstPageOfParagraph(from);
        // 续排锚点必须在截断之前读出来 —— 截断会把它一起丢掉。
        const ReflowState resume_state =
            layout_.para_index[static_cast<size_t>(from)].state_at_start;
        const int prefix_lines = layout_.para_index[static_cast<size_t>(from)].line_begin;
        truncateLayoutAt(from);
        layoutDocumentRange(fonts_, blocks_, cfg_.page, from, n_para, resume_state, &shaping_cache_,
                            &layout_);
        last_relaid_paragraphs_ = n_para - from;
        last_relaid_lines_ = layout_.stats.placed_line_count - prefix_lines;
        last_layout_full_ = false;
        // 脏页范围：脏段新旧两版的起点取更靠前的那一页（行数变少时内容往上走，
        //   新页号可能小于旧页号），它之后每一页都可能跟着重排了。
        //   不框出来的话就得整篇重算：在文档末尾敲一个字要哈希全篇的字形
        //   （实测 3000 段 = 7.5 ms，而那一步只动了最后一页）。
        const int new_first_page = firstPageOfParagraph(from);
        const int lo = (old_first_page < 0)   ? new_first_page
                       : (new_first_page < 0) ? old_first_page
                                              : std::min(old_first_page, new_first_page);
        if (lo < 0) {
            fp_lo = -1;  // 两版都找不到行盒（空段）—— 保守起见整篇重算
        } else if (layout_.stats.page_count != page_fp_new_count_) {
            // 页数变了 = 从脏段起重新分页了，那之后的每一页都跟着挪位。
            //   computePageFingerprints 遇到页数变化会退回整篇重算，这里不必：直接标脏。
            pages_known_changed = true;
            fp_lo = lo;
        } else {
            fp_lo = lo;
            fp_hi = std::max(0, layout_.stats.page_count - 1);
        }
    } else {
        layout_ = LayoutResult{};
        layoutDocumentRange(fonts_, blocks_, cfg_.page, 0, n_para, freshReflowState(cfg_.page),
                            &shaping_cache_, &layout_);
        last_relaid_paragraphs_ = n_para;
        last_relaid_lines_ = layout_.stats.placed_line_count;
        last_layout_full_ = true;
        fp_lo = -1;  // 整篇重排：没有"没变过的页"可言
    }
    layout_dirty_from_ = n_para;  // 已重排到末尾，没有脏段了
    layout_needs_full_ = false;

    // 这一轮排版的耗时。只算排版本身（拼接/续排/全量三种走法都算），
    // 不含页指纹记账与统计同步 —— 那两项是 O(全篇) 的固定开销，
    // 混进来的话"增量到底省了多少"就看不出来了（它们的量级能盖过排版本身）。
    // 必须在 syncStatsFromLayout 之前写：统计是从 layout_.stats 抄过去的。
    layout_.stats.layout_ms = msSince(t_layout0);
    syncStatsFromLayout();
    const auto t_fp0 = Clock::now();
    if (pages_known_changed) {
        markPagesChangedFrom(fp_lo);
    } else {
        computePageFingerprints(fp_lo, fp_hi);
    }
    stats_.page_fp_ms = msSince(t_fp0);
    edit_stats_.page_fp_ms = stats_.page_fp_ms;

    // 光标吸附到簇边界 —— 编辑可能让段落变短。吸附只改码点偏移，侧别跟着走
    //   （rel 是唯一真源，见 setCaretRel）：已经落在簇边界上就一个字都别碰，
    //   否则按"最内层"重投影一次。
    if (!blocks_.empty()) {
        const TextPos cp = caret();
        const size_t snapped = snapToCluster(cp.para, cp.offset);
        if (snapped != cp.offset) setCaretByText(TextPos{cp.para, snapped});
    }

    // 这一轮排版的真实耗时（含截断/拼接/逐段汇总）—— 有些路径不经过
    // layoutDocumentRange（快路），那里算不出这个数，所以在这里统一收口。
    edit_stats_.layout_ms = layout_.stats.layout_ms;
    // 排版成本只记一次：置位后由 presentInternal() 消费。
    layout_pending_ = true;
    stats_.total_ms += msSince(t0);
}

EditorEngine::SuffixReuse EditorEngine::trySuffixReuse(int from, int* page_lo, int* page_hi,
                                                       int* relaid_paragraphs,
                                                       bool* all_pages_changed) {
    *page_lo = -1;
    *page_hi = -1;
    *relaid_paragraphs = 1;
    *all_pages_changed = false;
    const int n_para = static_cast<int>(blocks_.size());
    const int n_old = static_cast<int>(layout_.para_index.size());
    // 段数增量：这一次编辑把旧 [from, from + m_old) 段换成了新 [from, from + m_new) 段，
    //   m_new - m_old 恒等于它（回车分段 +1、退格合并 -1、段内敲字 0）。
    const int d_para = n_para - n_old;
    // 没有后缀可搬（脏区已经是最后一段），就没什么可省的 —— 交给常规路径。
    if (from < 0 || from + 1 >= n_para) return SuffixReuse::None;

    // 行内对象不能伸进后缀段：插入/删除会移动控件端点，而占位槽的位置由端点决定 ——
    // 那些段的内容一个字没变、占位却变了。边界落在脏区之内的都可以搬。
    //   判据直接看块流里的对象表，不再反查控件集合：后缀里没有对象，就一定没有占位。
    //   从 from+1 起扫（而不是脏区的后面），偏保守：脏区自己带对象时也一并放弃复用。
    for (int p = from + 1; p < n_para; ++p) {
        if (!blocks_[static_cast<size_t>(p)].objects.empty()) return SuffixReuse::None;
    }

    const ParaIndex old_first = layout_.para_index[static_cast<size_t>(from)];
    // 脏区自己的续排锚点来自上一轮（前缀一个字都没动）。第 0 段没有前缀，用全篇起点。
    const ReflowState resume = (from == 0) ? freshReflowState(cfg_.page) : old_first.state_at_start;

    // 脏区在新旧两版里各占几段。段数没变时各占一段；段数变了时，窄的那一侧恒为一段 ——
    //   段落数的变化只可能来自插入或删除一枚段落标记，而一枚标记只牵动它所在的那一段
    //   （插入：1 段拆成 2 段；删除：2 段并回 1 段；粘进来 k 段就是 1 段拆成 k+1 段）。
    //   哪一侧是"1"由段数增量的符号决定。
    const int m_new = 1 + std::max(0, d_para);
    const int m_old = 1 + std::max(0, -d_para);
    // 新版要够长；旧版也要还有"下一段"可以拿来对照。
    if (from + m_new > n_para || from + m_old >= n_old) return SuffixReuse::None;

    LayoutResult scratch;
    const ReflowState after = layoutDocumentRange(fonts_, blocks_, cfg_.page, from, from + m_new,
                                                  resume, &shaping_cache_, &scratch);

    // 脏区新旧两版的段范围 → 行/字形范围。段在数组里是连续排列的，两端各取一次就是整段。
    const ParaIndex& old_last = layout_.para_index[static_cast<size_t>(from + m_old - 1)];
    const int old_line_lo = old_first.line_begin;
    const int old_line_hi = old_last.line_end;
    const int old_glyph_lo = old_first.glyph_begin;
    const int old_glyph_hi = old_last.glyph_end;
    const ParaIndex& new_first = scratch.para_index[static_cast<size_t>(from)];
    const ParaIndex& new_last = scratch.para_index[static_cast<size_t>(from + m_new - 1)];
    const int new_line_lo = new_first.line_begin;
    const int new_line_hi = new_last.line_end;
    const int new_glyph_lo = new_first.glyph_begin;
    const int new_glyph_hi = new_last.glyph_end;

    // 脏区新旧两版覆盖的页区间（并集）：只有这几页的内容可能变了，
    // 页指纹只需重算它们。取不到（两版都没行）就给 -1，让调用方整篇重算。
    int u_lo = -1, u_hi = -1;
    auto addPageRange = [&](const std::vector<LineBox>& lines, int b, int e) {
        if (e <= b) return;
        const int lo = lines[static_cast<size_t>(b)].page;
        const int hi = lines[static_cast<size_t>(e - 1)].page;
        u_lo = (u_lo < 0) ? lo : std::min(u_lo, lo);
        u_hi = (u_hi < 0) ? hi : std::max(u_hi, hi);
    };
    addPageRange(layout_.lines, old_line_lo, old_line_hi);
    addPageRange(scratch.lines, new_line_lo, new_line_hi);

    const int old_lines = old_line_hi - old_line_lo;
    const int new_lines = new_line_hi - new_line_lo;
    const int old_glyphs = old_glyph_hi - old_glyph_lo;
    const int new_glyphs = new_glyph_hi - new_glyph_lo;
    const int dline = new_lines - old_lines;
    const int dglyph = new_glyphs - old_glyphs;

    // 换掉脏区那一段：erase + insert 各一次 memmove，比重新排一遍后缀便宜一到两个数量级。
    layout_.lines.erase(layout_.lines.begin() + old_line_lo, layout_.lines.begin() + old_line_hi);
    layout_.lines.insert(layout_.lines.begin() + old_line_lo, scratch.lines.begin() + new_line_lo,
                         scratch.lines.begin() + new_line_hi);
    layout_.glyphs.erase(layout_.glyphs.begin() + old_glyph_lo,
                         layout_.glyphs.begin() + old_glyph_hi);
    layout_.glyphs.insert(layout_.glyphs.begin() + old_glyph_lo,
                          scratch.glyphs.begin() + new_glyph_lo,
                          scratch.glyphs.begin() + new_glyph_hi);

    // 段号整体平移：脏区之后那些段的段号全变了，而行盒与字形都把段号记在字段里，
    //   而光标几何、审计都按它找行 —— 不跟着改，光标会落到别的段上（静默错位）。
    //   范围从"脏区在新版里占的那些行/字形之后"起。
    if (d_para != 0) {
        for (size_t k = static_cast<size_t>(old_line_lo + new_lines); k < layout_.lines.size();
             ++k) {
            layout_.lines[k].paragraph += d_para;
        }
        for (size_t k = static_cast<size_t>(old_glyph_lo + new_glyphs); k < layout_.glyphs.size();
             ++k) {
            layout_.glyphs[k].paragraph += d_para;
        }
    }

    // 段表：把脏区之后的项整体挪 d_para 个槽位，再调成新长度。
    //   顺序与方向都不能错：往后挪（d_para > 0）要先扩容、再从尾到头搬，否则会盖住还没搬的项；
    //   往前挪（d_para < 0）要先把项搬到新位置、最后才缩容 —— 反过来会先丢掉末尾那几项，
    //   再读它们就是越界（搬过去的是垃圾，而排版看起来还"成功了"）。
    if (d_para > 0) {
        layout_.para_index.resize(static_cast<size_t>(n_para));
        layout_.para_cluster_starts.resize(static_cast<size_t>(n_para));
        for (int j = n_old - 1; j >= from + m_old; --j) {
            layout_.para_index[static_cast<size_t>(j + d_para)] =
                layout_.para_index[static_cast<size_t>(j)];
            layout_.para_cluster_starts[static_cast<size_t>(j + d_para)] =
                layout_.para_cluster_starts[static_cast<size_t>(j)];
        }
    } else if (d_para < 0) {
        for (int j = from + m_old; j < n_old; ++j) {
            layout_.para_index[static_cast<size_t>(j + d_para)] =
                layout_.para_index[static_cast<size_t>(j)];
            layout_.para_cluster_starts[static_cast<size_t>(j + d_para)] =
                layout_.para_cluster_starts[static_cast<size_t>(j)];
        }
        layout_.para_index.resize(static_cast<size_t>(n_para));
        layout_.para_cluster_starts.resize(static_cast<size_t>(n_para));
    }

    // 脏区：m_new 段一段一段填，行/字形下标接着前一段累加（起点仍在原处）。
    //   切片的 begin/end 是"在全局数组里的下标"，前一段的长度一变，后面所有段的下标
    //   都要跟着移，否则汇总会按错位的下标求和（数字会静默地错）。
    {
        int line_cursor = old_line_lo;
        int glyph_cursor = old_glyph_lo;
        for (int k = 0; k < m_new; ++k) {
            ParaIndex p = scratch.para_index[static_cast<size_t>(from + k)];
            const int lk = p.line_end - p.line_begin;
            const int gk = p.glyph_end - p.glyph_begin;
            p.line_begin = line_cursor;
            p.line_end = line_cursor + lk;
            p.glyph_begin = glyph_cursor;
            p.glyph_end = glyph_cursor + gk;
            line_cursor += lk;
            glyph_cursor += gk;
            layout_.para_index[static_cast<size_t>(from + k)] = p;
            layout_.para_cluster_starts[static_cast<size_t>(from + k)] =
                scratch.para_cluster_starts[static_cast<size_t>(from + k)];
        }
    }
    for (int j = from + m_new; j < n_para; ++j) {
        ParaIndex& p = layout_.para_index[static_cast<size_t>(j)];
        p.line_begin += dline;
        p.line_end += dline;
        p.glyph_begin += dglyph;
        p.glyph_end += dglyph;
    }
    summarizeLayoutStats(&layout_);
    layout_.stats.shape_ms = scratch.stats.shape_ms;
    *relaid_paragraphs = m_new;

    // 一级复用：后缀开始时的续排状态逐位相同 → 后缀的断页、全局行号与坐标
    // 都会与上一轮一模一样，一个字节都不用动。
    //   段表挪完槽位之后，新表的第 from+m_new 项就是旧表的第 from+m_old 项
    //   （就是后缀的第一段），它的 state_at_start 被原样搬了过来 —— 直接拿来比。
    if (after == layout_.para_index[static_cast<size_t>(from + m_new)].state_at_start) {
        *page_lo = u_lo;
        *page_hi = u_hi;
        return SuffixReuse::Unchanged;
    }

    // 二级复用：行结构变了（后缀整体挪位），但后缀是纯横排 → 只有块轴位置变，
    // 断行与字形的行内位置一个字都不用重算。见 shiftSuffixBlockAxis。
    //   段数变化几乎总落到这一级：段落边界一变就多让（或少让）一次段间距，
    //   后缀必然整体下移 —— 那不是"同状态"，而是"纯平移"。
    //   段表已经挪好槽位、行盒与字形的段号也已经重编过，平移那一趟按新段号走即可。
    if (u_lo < 0) return SuffixReuse::None;
    if (!suffixIsHorizontalOnly(from + m_new)) return SuffixReuse::None;
    if (!shiftSuffixBlockAxis(from + m_new, static_cast<size_t>(old_glyph_lo + new_glyphs), after))
        return SuffixReuse::None;
    *page_lo = u_lo;
    *page_hi = std::max(0, layout_.stats.page_count - 1);
    // 后缀整体挪位 ⇒ 它在的每一页内容都变了。这个结论不必靠哈希去问：
    //   后缀每一行都沿块轴平移了（挪量为零的话上面那条"同状态"判据早就命中了），
    //   而指纹里含行盒几何与每个字形的坐标 —— 它们必然不一样。
    *all_pages_changed = true;
    return SuffixReuse::Shifted;
}

bool EditorEngine::suffixIsHorizontalOnly(int from) const {
    // from 就是后缀的第一段（含）；后缀整体平移只对横排成立，见 shiftSuffixBlockAxis。
    for (int p = from; p < static_cast<int>(blocks_.size()); ++p) {
        if (blocks_[static_cast<size_t>(p)].wm.isVertical()) return false;
    }
    return true;
}

bool EditorEngine::shiftSuffixBlockAxis(int from, size_t first_glyph, const ReflowState& after) {
    const float cy = cfg_.page.margin_pt;
    const float ch = cfg_.page.contentHeightPt();
    const int n_para = static_cast<int>(blocks_.size());

    std::vector<PlacedGlyph>& gs = layout_.glyphs;  // 校验与改写共用这一份

    // 沿着"行槽折线"走一遍：每行占 lb.size_pt * line_factor（行盒自己的字号），
    // 段间让出 space_after_pt，放不下就换页 —— 与 layoutDocumentRange 横排分支同一套规则。
    //   行盒的 size_pt 是排版时按本行最大的字符格式 run 定的，所以混排字号时必须用它，
    //   不能用块的 size_pt：否则后缀的页号会与整篇重排不一致。
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
        // from 就是后缀的第一段（脏区可能占不止一段，所以调用方另给了 first_glyph）
        for (int p = from; p < n_para; ++p) {
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
            for (int L = pi.line_begin; L < pi.line_end; ++L) {
                LineBox& lb = layout_.lines[static_cast<size_t>(L)];
                // 行高按行盒自己的字号算（见上面那段说明）：一行里可以混排字号。
                const float lh = lb.size_pt * b.line_factor;
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

/**
 * 把"要显示括号的那些字段"的边界写进块流的行内对象表（元素流的过渡形态）。
 *
 * 边界本身不在这里产：编译层按树的遍历顺序给每个内容控件都产一对（次序规则见
 * compile.cpp —— 同一处边界上闭括号先、开括号后，空控件的一对紧挨着，所以产出端没有
 * 排序步骤）。这里只剩筛名单：只有引擎认识编辑模式与 form_brackets，也就知道哪些字段
 * 要显示括号。排版层从此只吃块流，"要画"与"要占位"是同一条记录，分不开。
 *
 * 筛是破坏性的（对象表会被删项），所以原件留在 blocks_full_ 里 —— 名单会来回变
 * （Focused 档与 All 档之间切），没有那份原件，被删掉的边界就回不来了。
 */
void EditorEngine::syncInlineObjects() {
    // 边界已经在产物里（编译对所有内容控件都产边界，不区分显示与否）——
    //   这一处于是只剩"按要显示括号的名单筛一遍"：不显示的字段连占位都不要。
    //   "显示才占位"这条规则没变，变的是分工：产边界在编译层（第五刀），
    //   决定显示哪些留在这里（只有引擎认识编辑模式与聚焦）。
    //
    //   只改 objects、不整段拷回：这条路径该做的事只有"按名单筛"，别的字段不归它管。
    //   （编辑已经改在树上，所以 blocks_ 与 blocks_full_ 现在同源 —— 整段拷回也等价，
    //   但那样一来"筛名单"就顺带重写了别的字段，将来任何一处增量修改都会被它静默抹掉。）
    std::vector<NodeId> keep;
    const std::vector<int> bracketed = bracketedControls();
    keep.reserve(bracketed.size());
    for (int ci : bracketed) {
        if (ci >= 0 && ci < static_cast<int>(control_node_ids_.size())) {
            keep.push_back(control_node_ids_[static_cast<size_t>(ci)]);
        }
    }
    const size_t n = std::min(blocks_.size(), blocks_full_.size());
    for (size_t p = 0; p < n; ++p) {
        const std::vector<InlineObject>& src = blocks_full_[p].objects;
        std::vector<InlineObject>& dst = blocks_[p].objects;
        dst.clear();
        for (const InlineObject& o : src) {
            const NodeId owner = static_cast<NodeId>(o.owner);
            if (std::find(keep.begin(), keep.end(), owner) != keep.end()) dst.push_back(o);
        }
    }
}

void EditorEngine::relayoutIfBracketPlanChanged(int prev_plan) {
    if (bracketPlanId() == prev_plan) return;
    markLayoutDirtyForBrackets();
    relayout();
}

}  // namespace heditor
