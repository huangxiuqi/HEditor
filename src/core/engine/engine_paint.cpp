/**
 * 呈现：把底图与光标提交到帧，并维护上屏脏矩形。
 *
 * 帧缓冲与底图分离是为了"只贴变化的部分"：底图是上一轮的完整画面，帧是它的副本，
 * 脏矩形决定往帧上重贴哪一块。绘制表单标记（方括号、焦点小点）也在这里，
 * 因为它与正文共用同一份行盒几何，另起一处会让"槽在不在"与"画不画"两份数据分叉。
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

/**
 * 呈现：把底图与光标提交到帧，并维护上屏脏矩形。
 */

void EditorEngine::repaint() {
    // 帧范围取本次布局的页数，不能锁在"底图已画过多少页"上。
    //
    //   否则文档从 N 页涨到 N+1 页时（页尾敲字、内容溢出到下一页）帧高不变，
    //   presentInternal 里 geom_changed 恒为 false，新页永远不进帧 —— 表现为
    //   "字打进去了，屏幕上什么都没多"，且之后每次重绘都按 N 页算帧高，不会自愈。
    //   页数增加时全帧重建；页数不变时仍走脏页增量。
    const int total = std::max(1, stats_.page_count);
    // 视口锚点把帧缩到"看得见的那几页"。帧高就是宿主画布的高度，而画布越大，
    //   浏览器每帧要重新合成的图层越大 —— 这一笔不随脏矩形变小（见 setViewportAnchor）。
    //   锚点越界时夹回文档范围内：文档变短、页度量变了都会让旧锚点失效，
    //   夹一下比要求每个调用方自己算安全。
    int first = 0;
    int count = total;
    if (vp_count_ > 0) {
        first = std::max(0, std::min(vp_first_page_, total - 1));
        count = std::max(1, std::min(vp_count_, total - first));
    }
    presentInternal(first, count, /*force_full=*/false);
}

void EditorEngine::setViewportAnchor(int first_page, int page_count) {
    vp_first_page_ = first_page;
    vp_count_ = page_count;
}

int EditorEngine::pagePxWidth() const {
    return std::max(1, static_cast<int>(std::lround(cfg_.page.width_pt * cfg_.zoom)));
}

int EditorEngine::pagePxHeight() const {
    return std::max(1, static_cast<int>(std::lround(cfg_.page.height_pt * cfg_.zoom)));
}

int EditorEngine::pageGapPx() const {
    return std::max(0, static_cast<int>(std::lround(cfg_.page_gap_pt * cfg_.zoom)));
}

void EditorEngine::paintPageRange(int first_page, int count) {
    presentInternal(first_page, count, /*force_full=*/false);
}

/**
 * 行带边界：第 k 行独占的块轴区间（页内磅）。
 *
 * 上下边界都取"与相邻行盒的中线"，而不是这一行自己的 block_lo/block_hi：
 *   行盒给的是字体的 ascent/descent，而个别字形（标点、带钩的部件）的墨迹会略微
 *   探出这个范围。取中线之后，相邻两行的带正好相接、互不重叠 ——
 *   于是"擦一条带"绝不会碰到没重画的那一行的墨迹，而这一行自己的全部墨迹
 *   一定落在带里（字形在行盒内）。用行盒自己的边界会啃掉相邻行的抗锯齿边：
 *   增量帧与从零重画的帧差 339~3550 px、最大通道差 49。
 */
void EditorEngine::lineBand(int k, float* lo, float* hi) const {
    const std::vector<LineBox>& ls = layout_.lines;
    if (k < 0 || k >= static_cast<int>(ls.size())) {
        *lo = 0.0f;
        *hi = 0.0f;
        return;
    }
    const LineBox& lb = ls[static_cast<size_t>(k)];
    const bool first_on_page = (k == 0) || (ls[static_cast<size_t>(k - 1)].page != lb.page);
    const bool last_on_page =
        (k + 1 >= static_cast<int>(ls.size())) || (ls[static_cast<size_t>(k + 1)].page != lb.page);
    *lo =
        first_on_page ? 0.0f : (ls[static_cast<size_t>(k - 1)].block_hi_pt + lb.block_lo_pt) * 0.5f;
    *hi = last_on_page ? static_cast<float>(pagePxHeight()) / cfg_.zoom
                       : (lb.block_hi_pt + ls[static_cast<size_t>(k + 1)].block_lo_pt) * 0.5f;
}

void EditorEngine::snapshotLineGeometry() {
    line_fp_in_base_ = line_fp_new_;
    line_fp_base_count_ = static_cast<int>(line_fp_new_.size());
    line_lo_in_base_.clear();
    line_hi_in_base_.clear();
    const int n = static_cast<int>(layout_.lines.size());
    line_lo_in_base_.resize(static_cast<size_t>(n));
    line_hi_in_base_.resize(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        float lo = 0.0f;
        float hi = 0.0f;
        lineBand(k, &lo, &hi);
        line_lo_in_base_[static_cast<size_t>(k)] = lo;
        line_hi_in_base_[static_cast<size_t>(k)] = hi;
    }
}

void EditorEngine::presentInternal(int first_page, int count, bool force_full) {
    const auto t0 = Clock::now();
    bitmap_.resetPaintedPixels();
    // 底图那份账单独清、单独读：字形是画在底图上的（帧那边只从底图搬像素过来），
    //   两块的写入量口径不同 —— 帧的记账回答"上屏搬了多少"，底图的回答"光栅画了多少"。
    base_.resetPaintedPixels();
    // 本次呈现的缓存命中率 —— 与 paintedPixels 同一条纪律：统计只描述这一轮。
    if (glyph_cache_) glyph_cache_->resetStats();

    const float zoom = cfg_.zoom;
    const int page_px_w = pagePxWidth();
    const int page_px_h = pagePxHeight();
    const int gap_px = pageGapPx();

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

    // 批量画字形的攒段缓冲。在 lambda 外面声明，跨页复用 ——
    //   否则"批量"自己会变成每页三次 malloc，一页上千个字形时分配合计能盖过省下的钱。
    std::vector<uint32_t> run_ids;
    std::vector<float> run_x;
    std::vector<float> run_y;

    // 把一页（或它的一条横带）画进底图。
    //   band_lo_pt / band_hi_pt 是"页内磅"的纵向范围，两者都非空时只画这一条带 ——
    //   带以外的底图内容保持不变（那正是"只重画脏行"能省下九成字形的原因）。
    //   带一定落在页内（行盒的块轴范围本来就夹在页里），所以底色直接用页面底色。
    auto paintPageIntoBase = [&](int local, int page_index, const float* band_lo_pt = nullptr,
                                 const float* band_hi_pt = nullptr) {
        const float off_y = static_cast<float>(local * (page_px_h + gap_px));
        const auto t_fill0 = Clock::now();
        // 擦除范围向外扩 1 px：相邻两条带的边界取两行中线，留 1 px 免得接缝上留一条线。
        const bool band = band_lo_pt && band_hi_pt;
        const float y0 = band ? std::max(0.0f, (*band_lo_pt) * zoom - 1.0f) : 0.0f;
        const float y1 = band ? std::min(static_cast<float>(page_px_h), (*band_hi_pt) * zoom + 1.0f)
                              : static_cast<float>(page_px_h);
        base_.fillRect(
            Rect{0.0f, off_y + y0, static_cast<float>(page_px_w), std::max(0.0f, y1 - y0)},
            cfg_.page_fill);
        if (!band) {
            // 整页时还要把上下半格页间隙也擦成背景色
            base_.fillRect(pageRectLocal(local), cfg_.background);
            base_.fillRect(
                Rect{0.0f, off_y, static_cast<float>(page_px_w), static_cast<float>(page_px_h)},
                cfg_.page_fill);
        }
        // 页边框必须每次都补：它在页矩形的边上，而上面那次底色填充会把它擦掉一条。
        //   漏掉这一笔的后果是"帧差 4308 像素"—— 正好是 2×(893+1263)-4，页边框的长度。
        //   （先量出来的，不是推出来的：增量帧与从零重画的帧差 4308、最大通道差 49。）
        if (cfg_.draw_page_border)
            base_.strokeRect(
                Rect{0.0f, off_y, static_cast<float>(page_px_w), static_cast<float>(page_px_h)},
                cfg_.page_border, 1.0f);
        sub_fill_ms += msSince(t_fill0);

        // 底图里只有页面底色与字形 —— 表单标记（小点/方括号）不在这里画。
        //   它们依赖光标位置，所以画在帧上；见 presentInternal 里那段说明。
        //
        // 同一字体 / 颜色 / 朝向的字形攒成一段，一次交给光栅器（见 Bitmap::drawGlyphs）：
        //   逐字形调用时，光栅器那笔固定开销（Skia 侧要新建 paint、装配一遍字形运行）
        //   全摊在一个字上 —— 整页重画变成 10 ms 量级就是这么来的。
        //   段内的次序不能动：它就是绘制次序，重叠字形靠它决定压盖。
        const auto t_glyph0 = Clock::now();
        hb_font_t* run_font = nullptr;
        Color run_color{};
        float run_rot = 0.0f;
        run_ids.clear();
        run_x.clear();
        run_y.clear();
        auto flush_run = [&]() {
            if (run_ids.empty()) return;
            base_.drawGlyphs(run_font, run_ids.data(), run_x.data(), run_y.data(),
                             static_cast<int>(run_ids.size()), units_to_px, run_color, run_rot);
            run_ids.clear();
            run_x.clear();
            run_y.clear();
        };
        for (const PlacedGlyph& pg : layout_.glyphs) {
            if (pg.page != page_index || !pg.font) continue;
            // 画带时按字形的纵向位置筛：行盒的块轴范围就是它的 ascent/descent，
            //   所以"落在带内"就是"属于这条带"。带向外扩了 1 px，相邻带在中线相接。
            if (band && (pg.y_pt < *band_lo_pt - 1.0f || pg.y_pt > *band_hi_pt + 1.0f)) continue;
            hb_font_t* f = pg.font->hb();
            if (!run_ids.empty() &&
                (f != run_font || pg.color != run_color || pg.rotate_deg != run_rot)) {
                flush_run();
            }
            if (run_ids.empty()) {
                run_font = f;
                run_color = pg.color;
                run_rot = pg.rotate_deg;
            }
            run_ids.push_back(pg.glyph_id);
            run_x.push_back(pg.x_pt * zoom);
            run_y.push_back(pg.y_pt * zoom + off_y);
        }
        flush_run();
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

    // 行带：把"这一页要重画"细化成"这一页的哪几行要重画"。
    //   一页约 1000 个字形，而敲一个字只动其中一两行 —— 页粒度下改一行与改整页同价
    //   （实测字形光栅 10~14 ms，其中九成是白画的）。
    //   结果写进 band_lo_scratch_ / band_hi_scratch_（页内磅）。
    //   返回 -1 = 这一页一行都没变；0 = 要整页重画；1 = 只重画一条带。
    //
    //   竖排页整页重画：行盒的"块轴"在竖排下是横轴，行带会变成竖条，
    //     而擦除矩形、字形筛选都按纵轴写；竖排文档少，不值得为它多一套几何。
    auto computeBand = [&](int page) -> int {
        int la = 0;
        int lb = 0;
        linesOnPage(page, &la, &lb);
        for (int k = la; k <= lb; ++k) {
            if (layout_.lines[static_cast<size_t>(k)].vertical) return 0;
        }
        int d0 = -1;
        int d1 = -1;
        for (int k = la; k <= lb; ++k) {
            const bool same =
                k < line_fp_base_count_ &&
                line_fp_new_[static_cast<size_t>(k)] == line_fp_in_base_[static_cast<size_t>(k)];
            if (same) continue;
            if (d0 < 0) d0 = k;
            d1 = k;
        }
        if (d0 < 0) return -1;
        float lo = 0.0f;
        float hi = 0.0f;
        lineBand(d0, &lo, &hi);
        float lo2 = 0.0f;
        float hi2 = 0.0f;
        lineBand(d1, &lo2, &hi2);
        hi = std::max(hi, hi2);
        // 新旧两版里更靠外的那个边界：行往下挪之后旧墨迹在更靠上的位置，
        //   只擦新位置就会留下幽灵字（auditFrame 正是抓这个的判据）。
        if (d0 < static_cast<int>(line_lo_in_base_.size()))
            lo = std::min(lo, line_lo_in_base_[static_cast<size_t>(d0)]);
        if (d1 < static_cast<int>(line_hi_in_base_.size()))
            hi = std::max(hi, line_hi_in_base_[static_cast<size_t>(d1)]);
        // 底图里的行数比现在多（删掉了行）⇒ 尾巴一直擦到页底，否则旧墨迹留在那儿
        if (line_fp_base_count_ > static_cast<int>(layout_.lines.size()))
            hi = static_cast<float>(page_px_h) / zoom;
        band_lo_scratch_ = lo;
        band_hi_scratch_ = hi;
        return 1;
    };

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
        snapshotLineGeometry();
        edit_stats_.repainted_pages = count;
        for (int i = 0; i < count; ++i) refresh.push_back(pageRectLocal(i));
    } else {
        // 底图增量：只重画指纹变了的页 —— 页里再按行指纹缩到那几条带
        int dirty = 0;
        for (int i = 0; i < count; ++i) {
            const int page = first_page + i;
            const bool changed = (page >= page_fp_base_count_) ||
                                 (page_fp_base_count_ != page_fp_new_count_) ||
                                 (page_fp_new_[static_cast<size_t>(page)] !=
                                  page_fp_in_base_[static_cast<size_t>(page)]);
            if (!changed) continue;
            ++dirty;
            const int band = computeBand(page);
            if (band <= 0) {
                // -1：页指纹变了而行指纹全同（只有页数/页间隙/页边框这类页外属性变了）
                //  0：竖排页 —— 两种都退回整页重画，宁可多画也不留幽灵。
                paintPageIntoBase(i, page);
                refresh.push_back(pageRectLocal(i));
                edit_stats_.band_y0 = 0;
                edit_stats_.band_y1 = page_px_h;
                continue;
            }
            paintPageIntoBase(i, page, &band_lo_scratch_, &band_hi_scratch_);
            // 帧那边也只贴这条带：底图本轮只动了这一条带，其余像素帧里已经是对的。
            const float off_y = static_cast<float>(i * (page_px_h + gap_px));
            const float y0 = std::max(0.0f, band_lo_scratch_ * zoom - 1.0f);
            const float y1 =
                std::min(static_cast<float>(page_px_h), band_hi_scratch_ * zoom + 1.0f);
            refresh.push_back(
                Rect{0.0f, off_y + y0, static_cast<float>(frame_w), std::max(1.0f, y1 - y0)});
            edit_stats_.band_y0 = static_cast<int>(y0);
            edit_stats_.band_y1 = static_cast<int>(y1);
        }
        page_fp_in_base_ = page_fp_new_;
        page_fp_base_count_ = page_fp_new_count_;
        snapshotLineGeometry();
        edit_stats_.repainted_pages = dirty;
    }
    edit_stats_.total_pages = total_pages;
    edit_stats_.window_pages = count;
    edit_stats_.window_first_page = first_page;

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
    if (mode_ == EditMode::Form) drawControlMarks();

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
    stats_.painted_glyphs = static_cast<int>(base_.paintedGlyphs());
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

        // 顺带记下"这一帧写过哪几页"，口径与上面这份 bbox 完全一致：同样这几份矩形，
        //   一个都不多、一个都不少。宿主按页上屏靠的就是它 —— 页指纹只能说明
        //   "底图哪几页变了"，而光标与表单标记画在帧上、不进底图，它们所在的那一页
        //   同样要重贴，页指纹里却看不见。
        last_dirty_pages_.clear();
        const int step = page_px_h + gap_px;
        auto notePage = [&](const Rect& r) {
            if (r.w <= 0.0f || r.h <= 0.0f) return;
            const int y0 = static_cast<int>(std::floor(r.y));
            const int y1 = static_cast<int>(std::ceil(r.y + r.h)) - 1;
            const int i0 = std::max(0, y0 / step);
            const int i1 = std::min(count - 1, y1 / step);
            for (int i = i0; i <= i1; ++i) {
                const int page = first_page + i;
                if (std::find(last_dirty_pages_.begin(), last_dirty_pages_.end(), page) ==
                    last_dirty_pages_.end())
                    last_dirty_pages_.push_back(page);
            }
        };
        for (const Rect& r : refresh) notePage(r);
        for (const Rect& r : control_marks_prev_) notePage(r);
        notePage(cr);
        std::sort(last_dirty_pages_.begin(), last_dirty_pages_.end());

        // 同时累积进"待上屏"—— 宿主上屏必须用它，不能用上面这几个"本轮"数字。
        //   理由（一次操作两次 present）写在 takeBlitBox 的注释里。
        if (any) noteBlitDirty(uni);
    }

    edit_stats_.caret_para = caret().para;
    edit_stats_.caret_offset = caret().offset;
    edit_stats_.caret_side = caretSide();
    edit_stats_.caret_page = caretPage();
    edit_stats_.caret_visible = (caret_visible_ && cr.w > 0.0f);
    edit_stats_.caret_x_px = static_cast<int>(cr.x);
    edit_stats_.caret_y_px = static_cast<int>(cr.y);
    edit_stats_.caret_w_px = static_cast<int>(cr.w);
    edit_stats_.caret_h_px = static_cast<int>(cr.h);

    // 表单
    // caret_control 用 include_end = true：光标停在字段末尾也算"在这个控件里"，
    //   与编辑归属同一口径（focusedControlIndex），两者永远不会互相矛盾。
    // 例外是点画布带进来的归属提示：共享边界上它比"最内层"更准，走同一个
    //   focusedControlIndex（面板/探针看到的必须与实际落笔的字段一致）。
    edit_stats_.edit_mode = static_cast<int>(mode_);
    edit_stats_.control_count = controls_.size();
    edit_stats_.caret_control = focusedControlIndex();
    edit_stats_.blocked_edits = blocked_edits_;
    edit_stats_.caret_index = caret().offset;
    for (int i = 0; i < caret().para && i < static_cast<int>(blocks_.size()); ++i)
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
        edit_stats_.page_fp_ms = 0.0;
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
    edit_stats_.total_ms = edit_stats_.layout_ms + edit_stats_.page_fp_ms + paint_ms;

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
    // 用 include_end = true：光标停在字段末尾也算"在这个字段里" ——
    // 否则刚打到最后一个字时括号会突然消失，而那一刻正是最需要看到边界的时候。
    const int focused = controls_.empty() ? -1 : controls_.innermostAt(caret(), true);

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

/**
 * 上屏契约：待上屏脏矩形累加器
 * 详见 engine/engine.h 里 takeBlitBox 的注释。一句话：宿主需要的不是"这一轮写了多少"
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

}  // namespace heditor
