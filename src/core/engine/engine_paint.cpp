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
    //   presentInternal 里换窗判据（尺寸与页范围）都不成立，新页永远不进帧 —— 表现为
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

/**
 * 预热带的像素高度。两行多一点：字形画进去不会被上下裁掉，缓存才存得住。
 */
constexpr int kPrewarmBandPx = 96;

/**
 * 预热时字形的纵向落点（预热带的局部像素）。
 *
 * 取在带的中部：给上行伸展与带下缘都留出余地，免得幅面大的字形被裁。
 */
constexpr float kPrewarmYOffsetPx = 40.0f;

/**
 * 预热一页的字形：把 mask 光栅一遍送进字形缓存，画面一个像素都不动。
 *
 * 靶子是一条页宽的窄带（见 prewarm_ 的说明）。字形按真实横向坐标画进去 ——
 *   横向是亚像素定位（Skia 侧 setSubpixel），横向小数相位不同就是另一份 mask，
 *   水平挪一下就把预热做废了。纵向只把整数部分挪走、小数相位原样保留，让字形
 *   落进这条带里；纵向相位不进缓存键（掩码的纵向落点由 blit 决定）。
 */
int EditorEngine::prewarmPageGlyphs(int page) {
    if (layout_.glyphs.empty() || page < 0) return 0;
    ensureRenderSetup();
    const int page_px_w = pagePxWidth();
    const int page_px_h = pagePxHeight();
    if (page_px_w <= 0 || page_px_h <= 0) return 0;
    // 带高取两行多一点：字形不会被上下裁掉，缓存才存得住（见 rasterizeGlyph 的 uncut）。
    if (prewarm_.width() != page_px_w || prewarm_.height() != kPrewarmBandPx) {
        prewarm_.reset(page_px_w, kPrewarmBandPx);
    }
    // 必须与底图共用字体表：字形缓存按 strike 规格索引，规格里含字体对象的身份，
    //   各建一份 SkTypeface 就是两份互不相见的缓存：预热会静默地什么都不做
    //   （症状只是"没变快"）。
    prewarm_.shareFonts(base_);
    prewarm_.setGlyphCache(glyph_cache_);
    prewarm_.clear(cfg_.page_fill);

    const float zoom = cfg_.zoom;
    const float units_to_px = zoom / kFontScalePerPt;
    std::vector<uint32_t> ids;
    std::vector<float> xs;
    std::vector<float> ys;
    hb_font_t* run_font = nullptr;
    Color run_color{};
    float run_rot = 0.0f;
    auto flush = [&]() {
        if (ids.empty()) return;
        prewarm_.drawGlyphs(run_font, ids.data(), xs.data(), ys.data(),
                            static_cast<int>(ids.size()), units_to_px, run_color, run_rot);
        ids.clear();
        xs.clear();
        ys.clear();
    };
    int n = 0;
    // 只过这一页的字形区间，不整篇筛。预热是逐页调用的，整篇筛会让每次预热都付 O(全篇)。
    int gb = 0;
    int ge = 0;
    glyphRangeOnPage(page, &gb, &ge);
    for (int k = gb; k < ge; ++k) {
        const PlacedGlyph& pg = layout_.glyphs[static_cast<size_t>(k)];
        if (!pg.font) continue;
        hb_font_t* f = pg.font->hb();
        if (!ids.empty() && (f != run_font || pg.color != run_color || pg.rotate_deg != run_rot)) {
            flush();
        }
        if (ids.empty()) {
            run_font = f;
            run_color = pg.color;
            run_rot = pg.rotate_deg;
        }
        const float y = pg.y_pt * zoom;
        ids.push_back(pg.glyph_id);
        xs.push_back(pg.x_pt * zoom);
        ys.push_back(y - std::floor(y) + kPrewarmYOffsetPx);
        ++n;
    }
    flush();
    return n;
}

void EditorEngine::presentInternal(int first_page, int count, bool force_full) {
    const auto t0 = Clock::now();
    // 五段分解的取值：两个时间点之差。用差值而不是"到这儿的累计时长"减，
    //   免得同一个时刻被读两次而算出一点零头。
    auto spanMs = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    ensureRenderSetup();
    bitmap_.resetPaintedPixels();
    // 底图那份账单独清、单独读：字形是画在底图上的（帧那边只从底图搬像素过来），
    //   两块的写入量口径不同 —— 帧的记账回答"上屏搬了多少"，底图的回答"光栅画了多少"。
    base_.resetPaintedPixels();
    // 本次呈现的缓存命中率 —— 与 paintedPixels 同一条纪律：统计只描述这一轮。
    if (glyph_cache_) glyph_cache_->resetStats();

    const auto t_pre = Clock::now();
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
    // 槽数至少要装得下这一窗：宿主报来的值偶尔会晚到一条消息（例如刚换了视口），
    //   而"两页共用一个槽"的后果是那几页永远缺画布、每一拍都在重贴。
    if (frame_slot_count_ < count || slot_page_.size() != static_cast<size_t>(frame_slot_count_)) {
        frame_slot_count_ = std::max(frame_slot_count_, count);
        slot_page_.assign(static_cast<size_t>(frame_slot_count_), -1);
        base_valid_ = false;
    }
    // 帧高 = 槽数 x 步距（不是"窗口页数 x 页高 + 间隙和"）—— 它是槽的容器，不是窗口的条带。
    //   与改前相比只多一格页间隙（槽数等于窗口页数时），实测 96 dpi 下是 18.07 -> 18.14 MB。
    const int frame_h = frame_slot_count_ * (page_px_h + gap_px);
    const float units_to_px = zoom / kFontScalePerPt;

    // 页在帧内的矩形（含上下半格页间隙；间隙是背景色，重复擦无副作用）
    auto pageRectOfPage = [&](int page) -> Rect {
        const float off_y = static_cast<float>(frameOffsetOfPage(page));
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
    auto paintPageIntoBase = [&](int page_index, const float* band_lo_pt = nullptr,
                                 const float* band_hi_pt = nullptr) {
        const float off_y = static_cast<float>(frameOffsetOfPage(page_index));
        const auto t_fill0 = Clock::now();
        // 擦除范围向外扩 1 px：相邻两条带的边界取两行中线，留 1 px 免得接缝上留一条线。
        const bool band = band_lo_pt && band_hi_pt;
        const float y0 = band ? std::max(0.0f, (*band_lo_pt) * zoom - 1.0f) : 0.0f;
        const float y1 = band ? std::min(static_cast<float>(page_px_h), (*band_hi_pt) * zoom + 1.0f)
                              : static_cast<float>(page_px_h);
        if (band) {
            base_.fillRect(
                Rect{0.0f, off_y + y0, static_cast<float>(page_px_w), std::max(0.0f, y1 - y0)},
                cfg_.page_fill);
        } else if (cfg_.page.continuous) {
            // 连页：没有页，也就没有页间隙与页边框 —— 整条内容列从头到尾是同一片底色，
            //   而条带首尾相接（宿主把页间隙设成 0），于是相邻条带在屏幕上连成一片。
            //   这一笔就是"一整条连续白底"的全部：条带的整幅都填页面底色。
            base_.fillRect(
                Rect{0.0f, off_y, static_cast<float>(page_px_w), static_cast<float>(page_px_h)},
                cfg_.page_fill);
        } else {
            // 整页：上下半格页间隙擦成背景色，页内填页面底色。
            //   间隙只有上下半格，所以分两条擦即可 —— 若按整个页矩形擦一次（含页内），
            //   紧接着又要把页内填回页面底色，那笔里九成九是页内区域，等于多写一遍
            //   整页（一页 4.4 MB）。两条的边界按整数像素算，与页矩形的 floor/ceil 同值。
            const float half = static_cast<float>(gap_px) * 0.5f + 1.0f;
            const int bg_top = static_cast<int>(std::floor(std::max(0.0f, off_y - half)));
            const int bg_bot = static_cast<int>(std::ceil(std::min(
                static_cast<float>(frame_h), off_y + static_cast<float>(page_px_h) + half)));
            const int pg_top = static_cast<int>(std::floor(off_y));
            const int pg_bot = static_cast<int>(std::ceil(off_y + static_cast<float>(page_px_h)));
            if (pg_top > bg_top)
                base_.fillRect(Rect{0.0f, static_cast<float>(bg_top),
                                    static_cast<float>(page_px_w),
                                    static_cast<float>(pg_top - bg_top)},
                               cfg_.background);
            if (bg_bot > pg_bot)
                base_.fillRect(Rect{0.0f, static_cast<float>(pg_bot),
                                    static_cast<float>(page_px_w),
                                    static_cast<float>(bg_bot - pg_bot)},
                               cfg_.background);
            base_.fillRect(
                Rect{0.0f, off_y, static_cast<float>(page_px_w), static_cast<float>(page_px_h)},
                cfg_.page_fill);
        }
        // 页边框必须每次都补：它在页矩形的边上，而上面那次底色填充会把它擦掉一条。
        //   漏掉这一笔的后果是"帧差 4308 像素"—— 正好是 2×(893+1263)-4，页边框的长度。
        //   （先量出来的，不是推出来的：增量帧与从零重画的帧差 4308、最大通道差 49。）
        //   连页下不画：没有页，也就没有"页的边"可画。
        if (cfg_.draw_page_border && !cfg_.page.continuous)
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
        // 只过这一页的字形区间（二分，见 glyphRangeOnPage）。
        //   整篇遍历 + 判页号：2000 段文档约 8 万个字形，每画一页都要过一遍，
        //   反推约 2 ms/页 —— 比真正画这一页的字形（742 个约 0.9 ms）还贵。
        int gb = 0;
        int ge = 0;
        glyphRangeOnPage(page_index, &gb, &ge);
        for (int k = gb; k < ge; ++k) {
            const PlacedGlyph& pg = layout_.glyphs[static_cast<size_t>(k)];
            if (!pg.font) continue;
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

    // 帧是"按页号分槽"的：第 page 页占第 (page mod 槽数) 个槽，位置与窗口开在哪里无关。
    //   于是窗口平移不再是特殊路径 —— 它只是"有些槽里装的是别的页了"，与"页内容变了"
    //   合并成同一套增量判据。改前这里要分三种走（全建 / 平移复用 / 页行增量），
    //   其中平移复用每次换窗要把底图与帧各 memmove 一遍（实测 11 ms），现在整段没有了。
    const bool size_changed = (base_.width() != frame_w) || (base_.height() != frame_h);

    if (force_full || size_changed || !base_valid_) {
        // 底图全建
        base_.reset(frame_w, frame_h);
        base_.clear(cfg_.background);
        // 首页必须在画页之前落定：页的位置由 frameOffsetOfPage 现算，它读的就是这个成员。
        //   原先传的是"窗口内下标"，算式里没有这个依赖，所以顺序随意；改成绝对页号之后
        //   它是显式的先后关系 —— 顺序反了会把整窗页画偏若干页（实测差 300 万像素）。
        base_first_page_ = first_page;
        for (int i = 0; i < count; ++i) paintPageIntoBase(first_page + i);
        // 全建之后每一个在窗口内的槽都装着它自己那一页 —— 表要跟着填，
        //   否则下一次增量会把"其实已经画好了"的页再画一遍。
        slot_page_.assign(static_cast<size_t>(frame_slot_count_), -1);
        for (int i = 0; i < count; ++i) {
            slot_page_[static_cast<size_t>(slotOfPage(first_page + i))] = first_page + i;
        }
        base_valid_ = true;
        base_page_count_ = count;
        page_fp_in_base_ = page_fp_new_;
        page_fp_base_count_ = page_fp_new_count_;
        snapshotLineGeometry();
        edit_stats_.repainted_pages = count;
        for (int i = 0; i < count; ++i) refresh.push_back(pageRectOfPage(first_page + i));
    } else {
        // 底图增量。两条独立的理由，任一成立就要重画这一页：
        //   槽过期 —— 帧里那一格装的是别的页（窗口平移进来的、或换过缩放）；
        //   页指纹变了 —— 同一页的内容改了（这一条还按行带缩到最小）。
        // 顺序不能颠倒：槽过期时必须整页重画，行带判据在那时无效 —— 它比的是
        //   "同一页的上一个版本"，而帧里那一格装的根本不是这一页。
        base_first_page_ = first_page;
        int dirty = 0;
        for (int i = 0; i < count; ++i) {
            const int page = first_page + i;
            const int slot = slotOfPage(page);
            const bool stale =
                slot >= static_cast<int>(slot_page_.size()) || slot_page_[static_cast<size_t>(slot)] != page;
            if (stale) {
                ++dirty;
                paintPageIntoBase(page);
                if (slot < static_cast<int>(slot_page_.size()))
                    slot_page_[static_cast<size_t>(slot)] = page;
                refresh.push_back(pageRectOfPage(page));
                edit_stats_.band_y0 = 0;
                edit_stats_.band_y1 = page_px_h;
                continue;
            }
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
                paintPageIntoBase(page);
                refresh.push_back(pageRectOfPage(page));
                edit_stats_.band_y0 = 0;
                edit_stats_.band_y1 = page_px_h;
                continue;
            }
            paintPageIntoBase(page, &band_lo_scratch_, &band_hi_scratch_);
            // 帧那边也只贴这条带：底图本轮只动了这一条带，其余像素帧里已经是对的。
            const float off_y = static_cast<float>(frameOffsetOfPage(page));
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
        // 行几何快照是 O(全部行) 的：它按 layout_.lines.size() 重新开数组并逐行算带范围，
        //   2000 段那一份是 4005 行、实测约 1.2 ms。而它描述的是"底图里装的是哪些行"，
        //   只有排版真的跑过才可能变 —— 与窗口挪不挪、有没有脏页都无关。
        //   加判据之后，空转与"只动光标"的那几拍都不再重取；全建那一支保持无条件重取
        //   （它本身就是换窗，重取一次相对它自己的代价可以忽略，而漏取会留下幽灵行）。
        if (layout_pending_) snapshotLineGeometry();
        edit_stats_.repainted_pages = dirty;
    }
    base_page_count_ = count;
    edit_stats_.total_pages = total_pages;
    edit_stats_.window_pages = count;
    edit_stats_.window_first_page = first_page;

    const auto t_swap = Clock::now();

    // 把帧缓冲刷新到与底图一致
    //
    // 关键：bitmap_ 只在尺寸变化时才 reset。尺寸不变时它保留上一帧内容，
    //   于是"没有脏页、只有光标动了一下"时只贴回两个小矩形；写下的像素数由 Bitmap 记账。
    // 选区矩形要在 refresh 定下来之前算：这一拍新出现的那几块必须进 refresh，
    //   否则它们永远等不到被画（帧是增量的，不在 refresh 里的像素被当成已经是对的）。
    //   矩形含槽偏移，而槽号只由页号决定 —— 所以窗口内平移时同一页给出的是同一个矩形，
    //   新旧两批各只差"进窗口那一页"与"出窗口那一页"。
    const std::vector<Rect> sel_rects = selectionRectsPxIn(first_page, count);
    const auto same_rect = [](const Rect& a, const Rect& b) {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    };

    const bool frame_geom_changed = (bitmap_.width() != frame_w) || (bitmap_.height() != frame_h);
    if (frame_geom_changed || frame_needs_full_refresh_) {
        if (frame_geom_changed) {
            bitmap_.reset(frame_w, frame_h);
            // 帧比窗口大：帧高 = 槽数 x 步距，而槽数可以多于窗口页数，多出来的是空槽。
            //   只有窗口内那几页会被上屏，所以这里只把"底"铺好，不整帧搬一遍底图 ——
            //   单段文档下实测整帧 544 万像素，而真正用到的只有 91 万。
            //   空槽也必须铺成背景色：扫整帧的判据（ink / markCheck）数的是"非背景"像素，
            //   留 0（透明黑）会被当成墨迹。
            //   clear 不记账，所以这一笔不进 repaintedPixels —— 那个数描述的是"上屏搬了多少"。
            bitmap_.clear(cfg_.background);
        }
        refresh.clear();
        for (int i = 0; i < count; ++i) refresh.push_back(pageRectOfPage(first_page + i));
        frame_needs_full_refresh_ = false;
        control_marks_prev_.clear();  // 整窗重贴，旧标记自然没了
    } else {
        // 上一帧的光标也要擦掉 —— 它不一定落在任何脏页上
        if (caret_rect_px_prev_.w > 0.0f) refresh.push_back(caret_rect_px_prev_);
        // 上一帧的表单标记同样要擦，而且这条是功能性的（不只是省像素）：
        //   光标移出某个字段时，那个字段的方括号必须消失。
        for (const Rect& r : control_marks_prev_) refresh.push_back(r);
        control_marks_prev_.clear();
        // 选区按新旧差集处理：不再有的擦掉、新出现的铺底后画，交集那一大片不动。
        //   它画在帧上，而帧是增量的 —— 这一拍没被重贴的像素本来就是对的。
        //   窗口内平移时新旧各只差一页的矩形（页的槽号与窗口无关，矩形本身不变），
        //   于是这一项从"整窗五页"降到"一页"；换窗那一拍两边几乎不相交，退化成整窗重画。
        for (const Rect& q : selection_rects_prev_) {
            bool still = false;
            for (const Rect& r : sel_rects) {
                if (same_rect(r, q)) {
                    still = true;
                    break;
                }
            }
            if (!still) refresh.push_back(q);
        }
        for (const Rect& r : sel_rects) {
            bool had = false;
            for (const Rect& q : selection_rects_prev_) {
                if (same_rect(r, q)) {
                    had = true;
                    break;
                }
            }
            if (!had) refresh.push_back(r);
        }
    }
    // 这一拍要重画的那几块选区：与 refresh 相交的（帧是增量的，不相交的像素本来就是对的）。
    //
    //   整块都要进 refresh，这是幂等性的前提：混色是叠加式的（把当前像素按 alpha 再混一次），
    //   而 refresh 里常常只有光标那一小条 —— 光标每闪一次就把它自己那一格铺回底图，
    //   若只铺那一小条、却把整块矩形再混一遍，就等于"这一行的选区每闪一次深一层"：
    //   越闪越深，最后连字都被叠成与底色同色（看上去是一条空白的蓝条）。
    //   先整块铺回底图（bg + 字形）再混一次，颜色就与"只混一次"完全相同。
    std::vector<Rect> sel_paint;
    for (const Rect& r : sel_rects) {
        for (const Rect& q : refresh) {
            if (r.x < q.x + q.w && q.x < r.x + r.w && r.y < q.y + q.h && q.y < r.y + r.h) {
                sel_paint.push_back(r);
                break;
            }
        }
    }
    for (const Rect& r : sel_paint) refresh.push_back(r);
    for (const Rect& r : refresh) bitmap_.copyRectFrom(base_, r);

    const auto t_blit = Clock::now();

    // 选区
    //
    //   与光标、表单标记同一条纪律：画在帧上、不进底图（底图的更新判据是页指纹，
    //   而那里面没有选区 —— 带上的话拖一次选就要整页重光栅）。
    //   必须先擦后画（擦在 refresh 那一步），否则拖动时会留下一路累积的淡蓝。
    //   用混合而不是实心填充：选区盖在文字上，实心填充会把字一起抹掉。
    //   画在光标之前，光标才能压在它上面。
    //
    //   只取窗口内的矩形：帧就是这一窗，窗口外的矩形画不到也擦不着，
    //     而它们的条数是"文档行数"这个量级（2000 段 ≈ 4000 条）。全取的话，
    //     全选之后每一拍都要混合 1.8 Mpx（实测 present 从 3.6 ms 涨到 9.8 ms），
    //     而真正的画面一个字都没变。
    //
    //   再只画落在 refresh 里的那些：帧是增量的，refresh 之外的像素本来就是对的
    //     （底图没变、选区也没变），重画一遍纯属白干 —— 而选区是逐像素混色，
    //     一页的量就是几毫秒。新旧差集那一套保证了"该画的都在 refresh 里"，
    //     窗口内平移时 refresh 只覆盖进出窗口的那两页，这一项因此从整窗降到一页。
    //   混色一律作用在"刚从底图铺回来"的那一块上（见上面 sel_paint 的说明）——
    //   这一条不成立时颜色会随重画次数累积变深。
    for (const Rect& r : sel_paint) bitmap_.blendRect(r, cfg_.selection_color);
    // 帧上真有选区的区域就是这一批：交集那一大片没被重贴、仍在帧上，差集那几块刚画过。
    //   下一拍要擦的正是它们 —— 记少了会留下残影，记多了会把没画的也擦一遍。
    selection_rects_prev_ = sel_rects;

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
    // 只读不画：它是呈现与打印态，括号落进版面就印出来了。
    if (mode_ != EditMode::ReadOnly) drawControlMarks(first_page, count);

    const auto t_overlay = Clock::now();

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
        // 帧矩形 -> 页号：先按步距折成槽号，再由槽表查出那一格装的是哪一页。
        //   改前是"帧矩形 -> 窗口内下标 -> first_page + i"，那个算法在槽布局下会把窗口外
        //   的矩形夹成"窗口内第一页或最后一页"，凭空多标脏。
        auto notePage = [&](const Rect& r) {
            if (r.w <= 0.0f || r.h <= 0.0f) return;
            const int y0 = static_cast<int>(std::floor(r.y));
            const int y1 = static_cast<int>(std::ceil(r.y + r.h)) - 1;
            const int s0 = std::max(0, y0 / step);
            const int s1 = std::min(frame_slot_count_ - 1, y1 / step);
            for (int s = s0; s <= s1; ++s) {
                if (s >= static_cast<int>(slot_page_.size())) continue;
                const int page = slot_page_[static_cast<size_t>(s)];
                // 只记窗口内的页：别的槽里可能还留着窗口外的旧内容，那些页不该上屏。
                if (page < first_page || page >= first_page + count) continue;
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

    const auto t_ledger = Clock::now();

    // 光标那几个数字一次算完：caret() 是投影（含一次二分），反复调既没必要也让读数
    //   有可能落在两个状态之间（下一次投影的代价换来的是"同一帧里的数来自同一处"）。
    const TextPos caret_now = caret();
    edit_stats_.caret_para = caret_now.para;
    edit_stats_.caret_offset = caret_now.offset;
    edit_stats_.caret_side = caretSide();
    edit_stats_.caret_vertical = caretVertical();
    edit_stats_.caret_block_reversed = caretBlockReversed();
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
    edit_stats_.caret_index = caret_now.offset;
    for (int i = 0; i < caret_now.para && i < static_cast<int>(blocks_.size()); ++i)
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

    const auto t_end = Clock::now();
    const double paint_ms = msSince(t0);
    stats_.paint_ms = paint_ms;
    stats_.paint_fill_ms = sub_fill_ms;
    stats_.paint_glyph_ms = sub_glyph_ms;
    // 记账那一笔含在 sub_glyph_ms 里，这里只是把它单独报出来（口径见 EngineStats）。
    //   取数的时机在整页循环之后、resetPaintedPixels 之前 —— 与 painted_glyphs 同一份账。
    stats_.paint_glyph_bounds_ms = base_.glyphBoundsMs();
    // 减掉前两段，剩下的就是"帧缓冲搬移"这一段 —— 比再插一个计时点更不容易漏
    // （中间还有光标擦除/填充那一小块）。
    stats_.paint_present_ms = std::max(0.0, paint_ms - sub_fill_ms - sub_glyph_ms);
    stats_.paint_pre_ms = spanMs(t0, t_pre);
    stats_.paint_swap_ms = spanMs(t_pre, t_swap);
    stats_.paint_blit_ms = spanMs(t_swap, t_blit);
    stats_.paint_overlay_ms = spanMs(t_blit, t_overlay);
    stats_.paint_ledger_ms = spanMs(t_overlay, t_ledger);
    stats_.paint_report_ms = spanMs(t_ledger, t_end);
    stats_.frame_width = frame_w;
    stats_.frame_height = frame_h;
    edit_stats_.paint_ms = paint_ms;
    edit_stats_.total_ms = edit_stats_.layout_ms + edit_stats_.page_fp_ms + paint_ms;

    if (glyph_cache_) {
        stats_.glyph_cache_hits = static_cast<long long>(glyph_cache_->hits());
        stats_.glyph_cache_misses = static_cast<long long>(glyph_cache_->misses());
        stats_.glyph_cache_entries = static_cast<int>(glyph_cache_->entries());
    }
    // Skia 那一侧的预算与占用：自写后端这两个入口是空实现 / 0。
    stats_.glyph_cache_bytes = base_.glyphCacheUsedBytes();
    stats_.glyph_cache_limit = base_.glyphCacheBudgetBytes();
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
void EditorEngine::drawControlMarks(int first_page, int count) {
    const float zoom = cfg_.zoom;

    const float units_to_px = zoom / kFontScalePerPt;
    const float dot = std::max(1.0f, cfg_.control_dot_pt * zoom);

    // 括号：只过窗口内的行，不遍历整份 layout_.lines。
    //
    //   原先遍历全篇：2000 段文档是 4005 行，每行还要再过一遍簇（绝大多数行没有
    //   占位簇，走的就是跳过那一支），实测每次呈现 2.0 ms —— 而"什么都没改的空转
    //   呈现"的全部耗时恰好就是这 2.0 ms（同一条路径在 1 页文档上只有 0.00 ms）。
    //   限窗同时也是正确性上的修正：帧坐标的 off_y 按 base_first_page_ 折算，窗口外的
    //   行算出来是帧外坐标，画不进帧，却会被记进 control_marks_prev_，于是进 refresh
    //   做一趟无用的搬运，还会把窗口外的页算进脏页清单与上屏框。
    //   行数组按页单调，所以窗口的行区间两端各二分一次就够。
    const std::vector<LineBox>& lines = layout_.lines;
    const int n_lines = static_cast<int>(lines.size());
    int k_begin = 0;
    int k_end = n_lines - 1;
    if (count > 0) {
        int lo = 0, hi = -1;
        linesOnPage(first_page, &lo, &hi);
        int lo2 = 0, hi2 = -1;
        linesOnPage(first_page + count - 1, &lo2, &hi2);
        // 首页没有行时 lo 已经指向"页号更大的第一行"；末页没有行时取相邻页边界减一。
        // 这两处的多取/少取都由下面逐行判页号兜住 —— 判据是页号，不是下标。
        k_begin = lo;
        k_end = (hi2 >= lo2) ? hi2 : lo2 - 1;
    }
    for (int k = k_begin; k <= k_end && k < n_lines; ++k) {
        if (k < 0) continue;
        const LineBox& lb = lines[static_cast<size_t>(k)];
        if (count > 0 && (lb.page < first_page || lb.page >= first_page + count)) continue;
        if (!lb.bracket_font) continue;
        const float off_y = static_cast<float>(frameOffsetOfPage(lb.page));

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
            if (count > 0 && (cr.page < first_page || cr.page >= first_page + count)) continue;
            const float off_y = static_cast<float>(frameOffsetOfPage(cr.page));
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
