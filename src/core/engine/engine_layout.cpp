/**
 * 排版调度与增量。
 *
 * 排版算法本身在 core/layout/：这里只管"什么时候排、排哪一段、排完怎么复用后缀"，
 * 也就是脏段记账（layout_dirty_from_）、后缀两级复用与统计同步。
 * 页面的失效不在这里：presentInternal 按 PageScene 与已画场景比差异。
 * 与算法分开的理由是两者的变更理由不同 —— 算法随排版规则变，这里随笔触与增量契约变。
 */

#include "core/engine/engine.h"

#include "core/layout/fragment.h"
#include "core/utils/hash.h"
#include "core/utils/time.h"
#include "core/utils/units.h"
#include "core/utils/writing_mode.h"

#include <hb.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

#include "core/text/shaper.h"

namespace heditor {

/**
 * 布局：重排整篇，并把排版统计同步到 EngineStats。
 */

void EditorEngine::syncStatsFromLayout() {
    stats_.page_count = layout_service_.layout.stats.page_count;
    stats_.glyph_count = layout_service_.layout.stats.glyph_count;
    stats_.placed_line_count = layout_service_.layout.stats.placed_line_count;
    stats_.cluster_count = layout_service_.layout.stats.cluster_count;
    stats_.missing_glyph_count = layout_service_.layout.stats.missing_glyph_count;
    stats_.font_run_count = layout_service_.layout.stats.font_run_count;
    stats_.shape_ms = layout_service_.layout.stats.shape_ms;
    stats_.layout_ms = layout_service_.layout.stats.layout_ms;
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
    stats_.density = cfg_.density;
}

int EditorEngine::contentBottomPx() const {
    const float slab = cfg_.page.slabHeightPt();
    float bottom_pt = 0.0f;
    for (const LineBox& lb : layout_service_.layout.lines) {
        // 横排的块轴是 y，竖排的块轴是 x（它的 y 是行内轴）—— 两种朝向取不同的字段。
        const float end = lb.vertical ? (lb.inline_origin_pt + lb.inline_end_pt) : lb.block_hi_pt;
        const float v = static_cast<float>(lb.page) * slab + end;
        if (v > bottom_pt) bottom_pt = v;
    }
    // 末尾补一个下边距：不留的话最后一行会贴着滚动底，鼠标点不到它下面那一小段。
    return static_cast<int>(std::ceil((bottom_pt + cfg_.page.margins.bottom) * cfg_.density));
}

void EditorEngine::relayout() {
    const auto t0 = Clock::now();
    // 视图先摆好：下面第一件事就是夹光标，而它读的是视图（地址稳定，摆一次就够）。
    refreshViews();

    // 只读位改的是括号颜色。它进 LineCluster::read_only，行场景项的版本随之变化，
    //   所以这里不需要再比一份集合、也不需要整窗清页缓存。

    // 顺序有讲究：先夹光标、再写对象表 —— 对象表要按光标算出来的流位置落锚点，
    //   表写完才是排版的输入。
    //
    //   这里只夹数值范围，不吸附簇：簇吸附需要簇表，而此刻尚未排版；用上一轮的簇表
    //     会把光标拉回旧簇边界，表现为"敲字后光标差一个位置"。簇吸附放在排版之后。
    if (!blocks().empty()) {
        int para = edit_session_.caret.caretPara();
        if (para >= static_cast<int>(blocks().size())) para = static_cast<int>(blocks().size()) - 1;
        const size_t len = blocks()[static_cast<size_t>(para)].cp_text.size();
        const CaretResolver::BracketCounts bc = edit_session_.caret.bracketCountsAt(para, len);
        const int span = static_cast<int>(len) + bc.before + bc.closes + bc.opens;
        if (edit_session_.caret.caretRel() > span) edit_session_.caret.setCaretRel(para, span);
    } else {
        edit_session_.caret.setCaretRel(0, 0);
    }

    // 把要显示括号的那些字段的两个边界写进块流的行内对象表：排版据此插入
    //   "括号占位"（占宽度、不产生字形），于是断行/行盒长度/簇位置全把括号算进去
    //   —— 这就是"括号进文档流"。不显示的就不进表（规则：显示才占位）。
    //
    // 筛由引擎做（只有它认识编辑模式与显示开关），但它落成的是块流里的一项，
    //   排版层拿到的是一份自足的输入：要画与要占位是同一条记录，分不开。
    syncInlineObjects();
    const int n_para = static_cast<int>(blocks().size());

    // 四档决策，从严到宽：整篇都脏或没有脏段走全量（前者影响面不可定位，后者是保守
    // 回退）；只重排脏段、后缀原样搬回（trySpliceOnly，要求后缀的行结构没变）；
    // 前缀保留加从脏段续排（常规增量）；其余情况全量。
    const bool must_full = layout_needs_full_ || layout_dirty_from_ >= n_para;
    // 脏区起点先对齐到表格边界：落在表里就从这张表的起始段重排（见 tableAlignedFrom）。
    //   对齐之后，"从 from 续排"与"整篇重排"在表格这一段上走的是同一条路。
    const int from = tableAlignedFrom(layout_dirty_from_);
    // 表格不再拦续排：脏区在表里时 from 已对齐到那张表的起始段，格上下文从零重建；
    //   脏区在表之后时表在前缀里，一个字节都不用动；脏区在表之前时表随后缀一起重排。
    //   （旧版有表格、新版删掉了的情况也走这条路：截断会把那张表的几何一起丢掉，
    //   见 truncateLayoutAt。）
    // 右端同理对齐到表尾：排到一半的表是个中间态（行高还没结算），既不能平移也不能接后缀。
    //   to < 0 表示右端未知（markLayoutDirty 只给了起点），那时本来也不试后缀复用。
    const int to = (layout_dirty_to_ >= 0) ? tableAlignedTo(layout_dirty_to_) : -1;
    // 后缀复用现在也收表格：平移只走到后缀里第一张表为止，剩下的从那张表续排
    //   （表不是行盒折线能算出来的东西，见 trySuffixReuse 的 cut）。
    const bool can_resume = !must_full && from > 0 && from < n_para &&
                            layout_service_.layout.para_index.size() > static_cast<size_t>(from);

    last_reused_paragraphs_ = 0;
    last_suffix_shifted_ = false;
    // 脏区在新版里占几段：右端已知就照实报（提交与撤销都从损伤区间拿到了右端）；
    //   未知时按段数差反推 —— 那要求一次操作只在一处动段落标记，是提交路径的老口径。
    int dirty_count = 1;
    if (to >= 0) {
        dirty_count = std::max(1, to - from);
    } else {
        dirty_count =
            1 + std::max(0, n_para - static_cast<int>(layout_service_.layout.para_index.size()));
    }
    if (from + dirty_count > n_para) dirty_count = n_para - from;
    const auto t_layout0 = Clock::now();  // 只量排版本身：拼接/续排/全量三种走法都算
    SuffixReuse reuse = SuffixReuse::None;
    // 段数变了也照样试后缀复用：回车分段与退格合并一个字都没改内容，只挪了段边界。
    int relaid_paras = 1;
    // 复用只覆盖到第 cut 段之前（后缀里遇到了表）：剩下的从 cut 续排。
    int cut = n_para;
    ReflowState cut_state;
    // 只有"脏区到哪儿结束"是已知的，后缀复用才成立：它把 [from + dirty_count, 末尾) 整段
    //   原样搬回来，等于断言"那之后一个字都没变"。markLayoutDirty 只给起点（"从这里起全脏"），
    //   那个断言就不成立 —— 括号占位就是活例子：聚焦档下换一个字段会挪走旧字段的括号，
    //   而它在脏区起点之后；后缀的空格宽度变了、行盒宽度却没变，续排状态比对看不出来，
    //   搬回来的就是带着旧括号的旧行盒。
    // 有浮动图时不走拼接：拼接把后缀原样搬走、只挪页号与行号，而排除带是绝对位置 ——
    //   搬完文字与带之间的相对关系就变了。续排那条路能带上带（见 ReflowState::bands）。
    const bool splice_ok = (to >= 0) && floats_.empty();
    if (!must_full && splice_ok && from >= 0 && from < n_para) {
        reuse = trySuffixReuse(from, dirty_count, &relaid_paras, &cut, &cut_state);
    }
    if (reuse != SuffixReuse::None) {
        // 段表的下标先取出来：cut 那一路会 truncate + 续排，段表可能重新分配。
        const int head_line_begin =
            layout_service_.layout.para_index[static_cast<size_t>(from)].line_begin;
        const int tail_line_end =
            layout_service_.layout.para_index[static_cast<size_t>(from + relaid_paras - 1)]
                .line_end;
        last_relaid_paragraphs_ = relaid_paras;
        last_reused_paragraphs_ = cut - relaid_paras - from;
        last_suffix_shifted_ = (reuse == SuffixReuse::Shifted);
        last_relaid_lines_ = tail_line_end - head_line_begin;
        last_layout_full_ = false;
        if (cut < n_para) {
            // 复用只到后缀里的第一张表：那张表与它之后的内容从表头续排。
            //   截断连表的旧几何一起丢（first_block >= cut），续排重新 openTable 只留一份。
            truncateLayoutAt(cut);
            layoutDocumentRangeWithTables(fonts_, blocks(), cfg_.page, projection_.tables, cut,
                                          n_para, cut_state, &layout_service_.cache,
                                          &layout_service_.layout, floatsForLayout());
            last_relaid_paragraphs_ = relaid_paras + (n_para - cut);
            last_relaid_lines_ = layout_service_.layout.stats.placed_line_count - head_line_begin;
        }
    } else if (can_resume) {
        // 续排锚点必须在截断之前读出来 —— 截断会把它一起丢掉。
        const ReflowState resume_state =
            layout_service_.layout.para_index[static_cast<size_t>(from)].state_at_start;
        const int prefix_lines =
            layout_service_.layout.para_index[static_cast<size_t>(from)].line_begin;
        truncateLayoutAt(from);
        layoutDocumentRangeWithTables(fonts_, blocks(), cfg_.page, projection_.tables, from, n_para,
                                      resume_state, &layout_service_.cache, &layout_service_.layout,
                                      floatsForLayout());
        last_relaid_paragraphs_ = n_para - from;
        last_relaid_lines_ = layout_service_.layout.stats.placed_line_count - prefix_lines;
        last_layout_full_ = false;
    } else {
        layout_service_.layout = LayoutResult{};
        layoutDocumentRangeWithTables(fonts_, blocks(), cfg_.page, projection_.tables, 0, n_para,
                                      freshReflowState(cfg_.page), &layout_service_.cache,
                                      &layout_service_.layout, floatsForLayout());
        last_relaid_paragraphs_ = n_para;
        last_relaid_lines_ = layout_service_.layout.stats.placed_line_count;
        last_layout_full_ = true;
    }
    layout_dirty_from_ = n_para;  // 已重排到末尾，没有脏段了
    layout_dirty_to_ = n_para;
    layout_needs_full_ = false;
    // 页索引：整篇与续排都走排版函数，那里收尾时建过一次；只有"拼接"不经过它 ——
    //   它用 erase + insert 换掉脏区，再整段平移后缀（页号、行号都变）。
    //   这一路不补建，索引就带着过期的页号去画：看不见的字、画错页的字，
    //   以及越界的下标。
    if (reuse != SuffixReuse::None) layout_service_.layout.rebuildPageIndex();

    // 这一轮排版的耗时。只算排版本身（拼接/续排/全量三种走法都算），
    // 不含页场景记账与统计同步 —— 那两项是 O(全篇) 的固定开销，
    // 混进来的话"增量到底省了多少"就看不出来了（它们的量级能盖过排版本身）。
    // 必须在 syncStatsFromLayout 之前写：统计是从 layout_service_.layout.stats 抄过去的。
    layout_service_.layout.stats.layout_ms = msSince(t_layout0);
    syncStatsFromLayout();
    // 页场景在 presentInternal 里按页构建，排版这一侧不再记这一段的耗时。
    //   这里先清零；呈现那一拍会把它覆盖成场景构建与差异比对的时间。
    stats_.page_fp_ms = 0.0;
    edit_stats_.page_fp_ms = 0.0;

    // 光标吸附到簇边界 —— 编辑可能让段落变短。吸附只改码点偏移，侧别跟着走
    //   （rel 是唯一真源，见 setCaretRel）：已经落在簇边界上就一个字都别碰，
    //   否则按"最内层"重投影一次。
    if (!blocks().empty()) {
        const TextPos cp = caret();
        const size_t snapped = edit_session_.caret.snapToCluster(cp.para, cp.offset);
        if (snapped != cp.offset) setCaretByText(TextPos{cp.para, snapped});
    }

    // 这一轮排版的真实耗时（含截断/拼接/逐段汇总）—— 有些路径不经过
    // layoutDocumentRange（快路），那里算不出这个数，所以在这里统一收口。
    edit_stats_.layout_ms = layout_service_.layout.stats.layout_ms;
    // 排版账在这里发布，不在呈现里：这些数字由这一轮排版决定，呈现只是把它读出去。
    //   挂在呈现上有两处读不到 —— 排版之后、呈现之前读到的是上一轮的账；两次呈现之间
    //   再呈现一次会把数字抹成 0，而抹掉的偏偏是刚算出来的那一份。
    edit_stats_.layout_ran = true;
    edit_stats_.relaid_paragraphs = last_relaid_paragraphs_;
    edit_stats_.reused_paragraphs = last_reused_paragraphs_;
    edit_stats_.suffix_shifted = last_suffix_shifted_;
    edit_stats_.relaid_lines = last_relaid_lines_;
    // 表格数按"脏区起点往后覆盖到几张表"算：全量时整篇都脏，起点取 0。
    //   表格是原子重排单位（见 tableAlignedFrom），所以这个数直接就是本轮的表格成本。
    last_relaid_tables_ = tablesFrom(last_layout_full_ ? 0 : from);
    edit_stats_.tables_relaid = last_relaid_tables_;
    edit_stats_.layout_was_full = last_layout_full_;
    edit_stats_.total_lines = layout_service_.layout.stats.placed_line_count;
    // 呈现只需消费"本轮重排过"这一次信号（见 presentInternal）。
    layout_pending_ = true;
    stats_.total_ms += msSince(t0);
}

EditorEngine::SuffixReuse EditorEngine::trySuffixReuse(int from, int dirty_count,
                                                       int* relaid_paragraphs, int* cut,
                                                       ReflowState* cut_state) {
    *relaid_paragraphs = 1;
    *cut = static_cast<int>(blocks().size());
    *cut_state = ReflowState{};
    const int n_para = static_cast<int>(blocks().size());
    const int n_old = static_cast<int>(layout_service_.layout.para_index.size());
    // 段数增量：这一次编辑把旧 [from, from + m_old) 段换成了新 [from, from + m_new) 段。
    //   新版那一段有多长由调用方给（损伤区间的右端），旧版那一段由总段数差反推：
    //   m_old = n_old - n_para + m_new —— 两侧的尾巴因此对齐到同一段。
    //     （旧写法把 m_new 当成 1 + max(0, d_para)，等于假定"一次只在一处动段落标记"；
    //      撤销一批跨段步骤时那个假定不成立，后缀会从错的地方接上去。）
    const int d_para = n_para - n_old;
    const int m_new = dirty_count;
    const int m_old = n_old - n_para + m_new;
    // 没有后缀可搬（脏区已经是最后一段），就没什么可省的 —— 交给常规路径。
    if (from < 0 || from + 1 >= n_para) return SuffixReuse::None;

    // 行内对象不能伸进后缀段：插入/删除会移动控件端点，而占位槽的位置由端点决定 ——
    // 那些段的内容一个字没变、占位却变了。边界落在脏区之内的都可以搬。
    //   判据直接看块流里的对象表，不再反查控件集合：后缀里没有对象，就一定没有占位。
    //   从 from+1 起扫（而不是脏区的后面），偏保守：脏区自己带对象时也一并放弃复用。
    //
    //   行内原子是例外，它照常复用：原子的锚点是段内码点、几何是自己的宽高，两样都只
    //   属于它自己那一段 —— 前面某一段的编辑既不移它的锚点，也不改它的盒宽高。把原子
    //   也当成"端点会动"的占位，会让"每段都有一张图"的文档整篇失去后缀复用。
    for (int p = from + 1; p < n_para; ++p) {
        for (const InlineObject& o : blocks()[static_cast<size_t>(p)].objects) {
            if (o.kind != InlineObjectKind::Atom) return SuffixReuse::None;
        }
    }

    const ParaIndex old_first = layout_service_.layout.para_index[static_cast<size_t>(from)];
    // 脏区自己的续排锚点来自上一轮（前缀一个字都没动）。第 0 段没有前缀，用全篇起点。
    const ReflowState resume = (from == 0) ? freshReflowState(cfg_.page) : old_first.state_at_start;

    // 两端都要还有"下一段"可以拿来对照：新版要够长，旧版也要留得下一段后缀。
    //   m_old <= 0 说明两侧尾巴对不齐（段数差比脏区还宽），那不是后缀能复用的形状。
    if (m_old <= 0) return SuffixReuse::None;
    if (from + m_new > n_para || from + m_old >= n_old) return SuffixReuse::None;

    // 后缀里遇到第一张表就在表头截断：复用只搬回 [from + m_new, cut)。
    //   表不能平移（行高要等一整行的格排完才结算、放不下整行回挪、跨页按行盒切片），
    //   所以它要么整张在脏区里、要么整张在复用区里 —— 脏区两端已经对齐到表
    //   （tableAlignedFrom / tableAlignedTo），这条不变式是那两处给的。
    //   剩下的部分从 cut 续排，那一趟与全量重排同构。
    int cut_at = n_para;
    for (const LayoutTable& t : projection_.tables) {
        for (const LayoutCell& c : t.cells) {
            if (c.first_block >= from + m_new) cut_at = std::min(cut_at, c.first_block);
        }
    }
    // 表正好从后缀的第一段起（或压在边界上）：没有可复用的一段，交给续排。
    if (cut_at <= from + m_new) return SuffixReuse::None;

    LayoutResult scratch;
    const ReflowState after =
        layoutDocumentRangeWithTables(fonts_, blocks(), cfg_.page, projection_.tables, from,
                                      from + m_new, resume, &layout_service_.cache, &scratch);

    // 脏区新旧两版的段范围 → 行/字形范围。段在数组里是连续排列的，两端各取一次就是整段。
    const ParaIndex& old_last =
        layout_service_.layout.para_index[static_cast<size_t>(from + m_old - 1)];
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

    const int old_lines = old_line_hi - old_line_lo;
    const int new_lines = new_line_hi - new_line_lo;
    const int old_glyphs = old_glyph_hi - old_glyph_lo;
    const int new_glyphs = new_glyph_hi - new_glyph_lo;
    const int dline = new_lines - old_lines;
    const int dglyph = new_glyphs - old_glyphs;

    // 换掉脏区那一段：erase + insert 各一次 memmove，比重新排一遍后缀便宜一到两个数量级。
    layout_service_.layout.lines.erase(layout_service_.layout.lines.begin() + old_line_lo,
                                       layout_service_.layout.lines.begin() + old_line_hi);
    layout_service_.layout.lines.insert(layout_service_.layout.lines.begin() + old_line_lo,
                                        scratch.lines.begin() + new_line_lo,
                                        scratch.lines.begin() + new_line_hi);
    layout_service_.layout.glyphs.erase(layout_service_.layout.glyphs.begin() + old_glyph_lo,
                                        layout_service_.layout.glyphs.begin() + old_glyph_hi);
    layout_service_.layout.glyphs.insert(layout_service_.layout.glyphs.begin() + old_glyph_lo,
                                         scratch.glyphs.begin() + new_glyph_lo,
                                         scratch.glyphs.begin() + new_glyph_hi);

    // 表格的绘制几何一起换：脏区里的表（整张都在脏区里）换成 scratch 这一轮刚排出来的那一份，
    //   后面的表段号跟着平移。不换的话，脏区那张表要么没有几何（画不出来），要么留着旧的一份
    //   （同一张表两份几何 = 重影，而像素判据只在整帧对拍里看得见）。
    //   placed_tables 按 first_block 单调（外层的表先 open，它的首段必不大于内层的），
    //   所以两端各二分一次就圈出"脏区里的那些表"。
    {
        auto byFirstBlock = [](const PlacedTable& t, int p) { return t.first_block < p; };
        const auto lo_it =
            std::lower_bound(layout_service_.layout.placed_tables.begin(),
                             layout_service_.layout.placed_tables.end(), from, byFirstBlock);
        const auto hi_it = std::lower_bound(lo_it, layout_service_.layout.placed_tables.end(),
                                            from + m_old, byFirstBlock);
        std::vector<PlacedTable> merged;
        merged.reserve(layout_service_.layout.placed_tables.size() + scratch.placed_tables.size());
        merged.insert(merged.end(), layout_service_.layout.placed_tables.begin(), lo_it);
        merged.insert(merged.end(), scratch.placed_tables.begin(), scratch.placed_tables.end());
        for (auto it = hi_it; it != layout_service_.layout.placed_tables.end(); ++it) {
            PlacedTable t = *it;
            t.first_block += d_para;
            // 格自己的 first_block 也要跟着走：命中测试与单元格选区都按它把段映射回格
            //   （见 layoutCellAt），只挪表头那一个数会让整张表的格都指向上一段。
            //   走到这里的表通常只剩"整段复用 + 段数变了"那一条（Shifted 那一路会在 cut 处
            //   把它们整张丢掉再续排），但少挪一个数就是静默错位，所以照挪不误。
            for (LayoutCell& c : t.cells) c.first_block += d_para;
            merged.push_back(std::move(t));
        }
        layout_service_.layout.placed_tables = std::move(merged);
    }

    // 段号整体平移：脏区之后那些段的段号全变了，而行盒与字形都把段号记在字段里，
    //   而光标几何、审计都按它找行 —— 不跟着改，光标会落到别的段上（静默错位）。
    //   范围从"脏区在新版里占的那些行/字形之后"起。
    if (d_para != 0) {
        for (size_t k = static_cast<size_t>(old_line_lo + new_lines);
             k < layout_service_.layout.lines.size(); ++k) {
            layout_service_.layout.lines[k].paragraph += d_para;
        }
        for (size_t k = static_cast<size_t>(old_glyph_lo + new_glyphs);
             k < layout_service_.layout.glyphs.size(); ++k) {
            layout_service_.layout.glyphs[k].paragraph += d_para;
        }
    }

    // 段表：把脏区之后的项整体挪 d_para 个槽位，再调成新长度。
    //   顺序与方向都不能错：往后挪（d_para > 0）要先扩容、再从尾到头搬，否则会盖住还没搬的项；
    //   往前挪（d_para < 0）要先把项搬到新位置、最后才缩容 —— 反过来会先丢掉末尾那几项，
    //   再读它们就是越界（搬过去的是垃圾，而排版看起来还"成功了"）。
    if (d_para > 0) {
        layout_service_.layout.para_index.resize(static_cast<size_t>(n_para));
        layout_service_.layout.para_cluster_starts.resize(static_cast<size_t>(n_para));
        for (int j = n_old - 1; j >= from + m_old; --j) {
            layout_service_.layout.para_index[static_cast<size_t>(j + d_para)] =
                layout_service_.layout.para_index[static_cast<size_t>(j)];
            layout_service_.layout.para_cluster_starts[static_cast<size_t>(j + d_para)] =
                layout_service_.layout.para_cluster_starts[static_cast<size_t>(j)];
        }
    } else if (d_para < 0) {
        for (int j = from + m_old; j < n_old; ++j) {
            layout_service_.layout.para_index[static_cast<size_t>(j + d_para)] =
                layout_service_.layout.para_index[static_cast<size_t>(j)];
            layout_service_.layout.para_cluster_starts[static_cast<size_t>(j + d_para)] =
                layout_service_.layout.para_cluster_starts[static_cast<size_t>(j)];
        }
        layout_service_.layout.para_index.resize(static_cast<size_t>(n_para));
        layout_service_.layout.para_cluster_starts.resize(static_cast<size_t>(n_para));
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
            layout_service_.layout.para_index[static_cast<size_t>(from + k)] = p;
            layout_service_.layout.para_cluster_starts[static_cast<size_t>(from + k)] =
                scratch.para_cluster_starts[static_cast<size_t>(from + k)];
        }
    }
    for (int j = from + m_new; j < n_para; ++j) {
        ParaIndex& p = layout_service_.layout.para_index[static_cast<size_t>(j)];
        p.line_begin += dline;
        p.line_end += dline;
        p.glyph_begin += dglyph;
        p.glyph_end += dglyph;
    }
    summarizeLayoutStats(&layout_service_.layout);
    layout_service_.layout.stats.shape_ms = scratch.stats.shape_ms;
    *relaid_paragraphs = m_new;

    // 一级复用：后缀开始时的续排状态逐位相同 → 后缀的断页、全局行号与坐标
    // 都会与上一轮一模一样，一个字节都不用动。
    //   段表挪完槽位之后，新表的第 from+m_new 项就是旧表的第 from+m_old 项
    //   （就是后缀的第一段），它的 state_at_start 被原样搬了过来 —— 直接拿来比。
    if (after ==
        layout_service_.layout.para_index[static_cast<size_t>(from + m_new)].state_at_start) {
        return SuffixReuse::Unchanged;
    }

    // 二级复用：行结构变了（后缀整体挪位），但后缀是纯横排 → 只有块轴位置变，
    // 断行与字形的行内位置一个字都不用重算。重新分页由 repaginateSuffix 按
    // FragmentIndex 的块行切片完成，与整篇排版共用 planBlockLines。
    //   段数变化几乎总落到这一级：段落边界一变就多让（或少让）一次段间距，
    //   后缀必然整体下移 —— 那不是"同状态"，而是"按片段重排"。
    //   重排只走到 cut：那里是第一张表，表的行片段不是折线能推出来的。
    if (!repaginateSuffix(&layout_service_.layout, blocks(), cfg_.page, from + m_new, cut_at, after,
                          cut_state)) {
        return SuffixReuse::None;
    }
    *cut = cut_at;
    return SuffixReuse::Shifted;
}

void EditorEngine::markLayoutDirty(int para) {
    if (para < 0) para = 0;
    // 空区间用 "from >= to（且 to 已知）" 表示，见 relayout 的收尾。
    const bool empty = (layout_dirty_to_ >= 0 && layout_dirty_from_ >= layout_dirty_to_);
    if (!empty && layout_dirty_to_ >= 0 && para >= layout_dirty_from_ && para < layout_dirty_to_) {
        return;
    }
    if (empty || para < layout_dirty_from_) layout_dirty_from_ = para;
    // 只给起点：右端从此未知，后缀复用退回"按段数差反推"。
    layout_dirty_to_ = -1;
}

/**
 * 损伤区间 -> 排版脏区间。
 *
 * 撤销与重做只有这个区间可用（没有命令层算的落点），而它恰好够用：区间左边一个字没变、
 * 右边一个字没变，中间那段必须重排。
 */
void EditorEngine::markLayoutDirtyForDamage(const doc::BlockDamage& dmg) {
    const int n = static_cast<int>(blocks().size());
    if (n <= 0) {
        markAllLayoutDirty();
        return;
    }
    // 右端夹进文档：尾部整段删掉时 lo == hi == 块数，此时没有"新的脏段"，但旧排版多出来
    //   的尾巴必须截掉 —— 从最后一段起排一遍就把它截掉了（代价一段，安全）。
    const int lo = std::min(dmg.lo, n - 1);
    markLayoutDirtyRange(lo, std::min(dmg.hi, n));
}

void EditorEngine::markLayoutDirtyRange(int lo, int hi) {
    if (lo < 0) lo = 0;
    if (hi < lo) hi = lo;
    const bool empty = (layout_dirty_to_ >= 0 && layout_dirty_from_ >= layout_dirty_to_);
    if (empty) {
        layout_dirty_from_ = lo;
        layout_dirty_to_ = hi;
        return;
    }
    if (layout_dirty_to_ < 0) {
        // 已经有一片"到末尾"的脏区：右端不会因为这一次而变具体。
        if (lo < layout_dirty_from_) layout_dirty_from_ = lo;
        return;
    }
    // 两段取并集；中间那道缝一并重排 —— 后缀复用要求"从起点到最后一次改动之间全都是
    //   重排过的"，留缝就等于把缝里的旧行盒接在新后缀前面（保守即正确，多排的代价有限）。
    layout_dirty_from_ = std::min(layout_dirty_from_, lo);
    layout_dirty_to_ = std::max(layout_dirty_to_, hi);
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

int EditorEngine::tableAlignedFrom(int from) const {
    if (projection_.tables.empty()) return from;
    const int n = static_cast<int>(blocks().size());
    if (from <= 0 || from >= n) return from;
    // 由最外层表定起点：内层表的行高并进外层格的内容高，只从内层重排算不出外层那一行
    //   该多高（"等所有格排完才结算"是行的性质，不是格的）。
    const CellIndex& cells = projection_.structure.cells_index;
    const int ti = cells.outermostTable(from);
    if (ti < 0) return from;
    const TableSpan& span = cells.spans[static_cast<size_t>(ti)];
    if (span.first_block < 0) return from;
    return std::min(from, span.first_block);
}

int EditorEngine::tableAlignedTo(int to) const {
    if (projection_.tables.empty()) return to;
    const int n = static_cast<int>(blocks().size());
    if (to <= 0 || to >= n) return to;
    const CellIndex& cells = projection_.structure.cells_index;
    int end = to;
    // 两侧都要看：to-1 是最后一个脏段（可能还在表里），to 是后缀首段（可能已经是同一张表的
    //   下一格）。任一侧落在表里，就得扩到那张最外层表的末尾 —— 表只排一半是个中间态。
    for (int probe = to - 1; probe <= to; ++probe) {
        if (probe < 0 || probe >= n) continue;
        const int ti = cells.outermostTable(probe);
        if (ti < 0) continue;
        const TableSpan& span = cells.spans[static_cast<size_t>(ti)];
        if (span.last_block >= 0) end = std::max(end, span.last_block + 1);
    }
    return std::min(end, n);
}

int EditorEngine::tablesFrom(int from) const {
    int n = 0;
    for (const LayoutTable& t : projection_.tables) {
        bool hit = false;
        for (const LayoutCell& c : t.cells) {
            if (c.block_count <= 0 || c.first_block < 0) continue;
            if (c.first_block + c.block_count > from) {
                hit = true;
                break;
            }
        }
        if (hit) ++n;
    }
    return n;
}

void EditorEngine::truncateLayoutAt(int para) {
    if (para <= 0) {
        layout_service_.layout = LayoutResult{};
        return;
    }
    const size_t k = static_cast<size_t>(para);
    if (layout_service_.layout.para_index.size() <= k) {
        layout_service_.layout = LayoutResult{};
        return;
    }
    const ParaIndex& cut = layout_service_.layout.para_index[k];
    const size_t line_cut = static_cast<size_t>(std::max(0, cut.line_begin));
    const size_t glyph_cut = static_cast<size_t>(std::max(0, cut.glyph_begin));
    if (line_cut > layout_service_.layout.lines.size() ||
        glyph_cut > layout_service_.layout.glyphs.size()) {
        layout_service_.layout = LayoutResult{};
        return;
    }
    // 字形与行盒严格按段序追加，所以"第 para 段的起点"就是安全的切点：
    // [0, para) 的切片与内容一个字节都不用动。
    layout_service_.layout.lines.resize(line_cut);
    layout_service_.layout.glyphs.resize(glyph_cut);
    layout_service_.layout.para_index.resize(k);
    layout_service_.layout.para_cluster_starts.resize(k);
    // 表格的绘制几何一起截：起始段 >= para 的那些属于被丢掉的那一半。留下来的话，
    //   续排重新 openTable 会再 push 一条，同一张表就有两份几何（一份是旧的，
    //   绘制时两张都画 —— 症状是表格重影，而像素判据只在"整帧对拍"里才看得见）。
    layout_service_.layout.placed_tables.erase(
        std::remove_if(layout_service_.layout.placed_tables.begin(),
                       layout_service_.layout.placed_tables.end(),
                       [para](const PlacedTable& t) { return t.first_block >= para; }),
        layout_service_.layout.placed_tables.end());
    // 浮动图的几何同理：锚点落在被丢掉那一半的图，续排会重新落一遍，留着就是同一张图
    //   两份几何（拖动时看见两张错开的框）。锚点在保留那一半的图不在这里删 —— 它们
    //   已经画在纸上了，续排也不会再产出它们。
    layout_service_.layout.placed_floats.erase(
        std::remove_if(layout_service_.layout.placed_floats.begin(),
                       layout_service_.layout.placed_floats.end(),
                       [this, para](const PlacedFloat& pf) {
                           for (const LayoutFloat& f : floats_) {
                               if (f.handle == pf.handle) return f.para >= para;
                           }
                           return false;
                       }),
        layout_service_.layout.placed_floats.end());
}

/**
 * 把"要显示括号"这件事落成块流的行内对象表（元素流的过渡形态）。
 *
 * 边界本身不在这里产：编译层按树的遍历顺序给每个内容控件都产一对（次序规则见
 * compile.cpp —— 同一处边界上闭括号先、开括号后，空控件的一对紧挨着，所以产出端没有
 * 排序步骤）。这里只剩筛：显示范围只有"全部或没有"两档（见 EngineConfig::show_brackets），
 * 于是要么原样吃，要么把控件边界全部去掉、只留原子。排版层从此只吃块流，
 * "要画"与"要占位"是同一条记录，分不开。
 *
 * 筛是破坏性的（对象表会被删项），所以原件留在 projection_.blocks_full 里 —— 名单会来回变
 * （切只读、开关括号），没有那份原件，被删掉的边界就回不来了。
 */
void EditorEngine::syncInlineObjects() {
    // 全部显示：不筛。projection_.blocks 不存东西，排版直接吃 projection_.blocks_full（同一份）。
    //   这条分支现在是常态（括号常显），所以它值得放在最前面短路。
    if (bracketsOn()) {
        projection_.blocks.clear();
        filter_active_ = false;
        return;
    }
    // 一个都不显示（只读、或开关关掉）：控件边界全部去掉，只留原子。
    //   原子是内容，不随"显示括号"的开关变 —— 它要么在文档里、要么不在。
    filter_active_ = true;
    projection_.blocks = projection_.blocks_full;
    const size_t n = std::min(projection_.blocks.size(), projection_.blocks_full.size());
    for (size_t p = 0; p < n; ++p) {
        const std::vector<InlineObject>& src = projection_.blocks_full[p].objects;
        std::vector<InlineObject>& dst = projection_.blocks[p].objects;
        dst.clear();
        for (const InlineObject& e : src) {
            if (e.kind == InlineObjectKind::Atom) dst.push_back(e);
        }
    }
}

/**
 * 括号占位从 prev 换到 cur：脏区就是那两个字段各自占的段（凸包）。
 *
 * 为什么不从"最靠前的控件段"起一直排到末尾：聚焦档下换字段是常事，而从第一个字段排到
 * 末尾等于整篇重排。两个字段各自的跨度在控件表里是已知的，取凸包就够 —— 中间那段一并
 * 重排，因为排版只有"一段连续脏区"这个形状，而后缀复用要求区间连续。
 *
 * 任何一边不是具体字段（-1 没有占位，-2 全部）时退回保守口径：那不是换了一个字段，
 * 而是占位集合整个换了形状（切档、切编辑模式），脏区无从定位。
 */
void EditorEngine::markLayoutDirtyForBracketPlans(int prev, int cur) {
    const int n = controls_.size();
    const Control* a = (prev >= 0 && prev < n) ? &controls_.at(prev) : nullptr;
    const Control* b = (cur >= 0 && cur < n) ? &controls_.at(cur) : nullptr;
    if (prev == -2 || cur == -2 || (prev >= 0 && a == nullptr) || (cur >= 0 && b == nullptr)) {
        markLayoutDirtyForBrackets();
        return;
    }
    int lo = -1;
    int hi = -1;
    for (const Control* c : {a, b}) {
        if (c == nullptr || c->range.start.para < 0) continue;
        const int s = c->range.start.para;
        const int e = std::max(s, c->range.end.para);
        lo = (lo < 0) ? s : std::min(lo, s);
        hi = (hi < 0) ? e + 1 : std::max(hi, e + 1);
    }
    if (lo < 0) {
        markLayoutDirtyForBrackets();
        return;
    }
    markLayoutDirtyRange(lo, hi);
}

void EditorEngine::relayoutIfBracketPlanChanged(int prev_plan) {
    const int plan = bracketPlanId();
    if (plan == prev_plan) return;
    markLayoutDirtyForBracketPlans(prev_plan, plan);
    relayout();
}

}  // namespace heditor
