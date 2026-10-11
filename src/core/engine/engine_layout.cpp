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


}  // namespace heditor
