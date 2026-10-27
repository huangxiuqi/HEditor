/**
 * 呈现：保证窗口内每页的内容新鲜，记下这一拍要上屏的矩形，并按需导出像素。
 *
 * 渲染只写页位图（内容），光标与选区是导出时才叠上去的瞬态。于是"局部重绘"
 *   退化成"页位图变了哪一块"，宿主按页上的矩形贴：没有第二份像素要跟着同步。
 * 表单标记（方括号、小点）画在页位图里，与正文共用同一份行盒几何 —— 见 rasterControlMarks。
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
 * 裁剪标记的臂长（磅）。18 磅 = 1/4 英寸：印刷业的裁切标记就用这个长度，它必须是一个
 * 与排版无关的固定物理尺寸 —— 臂长跟着字号或页边距变，同一份文档在不同页上就不成一套。
 * 页边距小于这个长度时按边距的一半收，免得标记越进正文里。
 */
constexpr float kMarginMarkPt = 18.0f;

void EditorEngine::repaint() {
    // 窗口取本次布局的页数，不能锁在"上一次画过多少页"上：文档从 N 页涨到 N+1 页时
    //   （页尾敲字、内容溢出到下一页），锁住的话新页永远不进窗口 —— 表现为"字打进去了，
    //   屏幕上什么都没多"，而且不会自愈。
    const int total = std::max(1, stats_.page_count);
    // 视口锚点把呈现范围缩到"看得见的那几页"。窗口越大，宿主每拍要维护的画布越多，
    //   而这一笔不随脏矩形变小（见 setViewportAnchor）。
    //   锚点越界时夹回文档范围内：文档变短、页度量变了都会让旧锚点失效，
    //   夹一下比要求每个调用方自己算安全。
    int first = 0;
    int count = total;
    if (vp_count_ > 0) {
        first = std::max(0, std::min(vp_first_page_, total - 1));
        count = std::max(1, std::min(vp_count_, total - first));
    }
    presentInternal(first, count);
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

void EditorEngine::paintPageRange(int first_page, int count) { presentInternal(first_page, count); }

/**
 * 行带边界：第 k 行独占的块轴区间（页内磅）。
 *
 * 上下边界取"与相邻行盒的中线"，而不是行自己的 block_lo/block_hi：行盒给的是
 *   ascent/descent，个别字形（标点、带钩部件）的墨迹会探出去 —— 用行盒边界会啃掉
 *   相邻行的抗锯齿边（增量帧与从零重画差 339~3550 px、最大通道差 49）。相邻带因此
 *   正好相接，擦一条带碰不到没重画的那一行。
 *
 * 表格是这条推论的例外：同一表格行里各格的行数可以不同，图片格的行盒（97..131）
 *   与邻格第二行（115..132）纵向相交，"相邻中线"就落进其中一组的内部，那半截从此
 *   不属于任何带 —— 擦除按带做，于是它被擦掉却不再重画（格内折行后半截字变空白或
 *   与旧墨重叠）。所以分组按纵向重叠合并，组内共一条带：宁可多画几十像素。
 */
void EditorEngine::pageLineBands(int page, std::vector<float>* lo, std::vector<float>* hi) const {
    const std::vector<LineBox>& ls = layout_.lines;
    const std::vector<int>& ids = layout_.linesOnPage(page);
    if (lo && lo->size() != ls.size()) lo->assign(ls.size(), 0.0f);
    if (hi && hi->size() != ls.size()) hi->assign(ls.size(), 0.0f);
    if (ids.empty()) return;

    /**
     * 一行在块轴上的位置（排序用：页内的行不按块轴有序 —— 表格里一条行切页之后，
     *   同一行其余各格的行还在上一页，数组顺序与版面上下顺序不是一回事）。
     */
    struct Item {
        /**
         * 行的块轴上沿（磅）。
         */
        float top = 0.0f;

        /**
         * 行的块轴下沿（磅）。
         */
        float bottom = 0.0f;

        /**
         * 行下标（lines 里那一条）。
         */
        int line = 0;
    };

    /**
     * 一组同高的行：表格一行的几个格各有自己的行盒，块轴位置完全相同。
     */
    struct Group {
        /**
         * 组的块轴上沿（磅）。
         */
        float top = 0.0f;

        /**
         * 组的块轴下沿（磅）。
         */
        float bottom = 0.0f;
    };

    std::vector<Item> items;
    items.reserve(ids.size());
    for (int k : ids) {
        const LineBox& lb = ls[static_cast<size_t>(k)];
        items.push_back(Item{lb.block_lo_pt, lb.block_hi_pt, k});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.top != b.top) return a.top < b.top;
        return a.line < b.line;
    });
    std::vector<Group> groups;
    groups.reserve(items.size());
    for (const Item& it : items) {
        // 纵向重叠的并成一组，不只是"top 相同"的那些。表格一行里各格的行数可以不同：
        //   一格是图（行盒 97..131）、隔壁那格折了两行（97..115、115..132），第二行
        //   的 top 就落在图片那一行的范围里。两组纵向相交时，"相邻两组的中线"会落在
        //   其中一组的内部，那一组自己的上半截从此不属于任何带 —— 擦除按带做，于是
        //   这半截字被擦掉却不再重画（页位图与全量光栅差一条横带）。
        if (!groups.empty() && it.top < groups.back().bottom - 0.01f) {
            groups.back().bottom = std::max(groups.back().bottom, it.bottom);
            continue;
        }
        groups.push_back(Group{it.top, it.bottom});
    }

    // 边界取相邻两组的中线：这样相邻两条带正好拼满，字形不会被裁成两截。
    //
    //   这里必须按"块轴上有序的组"算，不能看数组里的相邻项：高格的第 2 行之后，
    //   数组紧跟着的是同一行另一格的第 1 行（它在上面），拿它当"下一行"会把这条带
    //   压成零高 —— 那一行的字与段落标记从此不再重画（屏幕上留的是旧墨迹）。
    const float page_bottom = static_cast<float>(pagePxHeight()) / cfg_.zoom;
    size_t gi = 0;
    for (const Item& it : items) {
        // 按"item 的 top 落在哪一组"找，不按 top 相等找：组可能是几个纵向重叠的行
        //   并出来的，它们的 top 各不相同（见上面分组的说明）。
        while (gi + 1 < groups.size() && it.top >= groups[gi + 1].top - 0.01f) ++gi;
        const float top_edge = (gi == 0) ? 0.0f : 0.5f * (groups[gi - 1].bottom + groups[gi].top);
        const float bottom_edge = (gi + 1 >= groups.size())
                                      ? page_bottom
                                      : 0.5f * (groups[gi].bottom + groups[gi + 1].top);
        if (lo) (*lo)[static_cast<size_t>(it.line)] = top_edge;
        if (hi) (*hi)[static_cast<size_t>(it.line)] = bottom_edge;
    }
}

void EditorEngine::snapshotLineGeometry() {
    line_fp_prev_ = line_fp_new_;
    line_fp_prev_count_ = static_cast<int>(line_fp_new_.size());
    // 表格片段摘要与行指纹在同一处快照：computeBand 拿两边的新旧值比，
    //   两者错开一轮时，切过页的那一页会被判成"只动了一行"。
    table_fp_prev_ = table_fp_new_;
    table_fp_prev_count_ = static_cast<int>(table_fp_new_.size());
    // 浮动图摘要同理：它与表格一样"不是行盒画出来的东西"，快照必须在同一处。
    float_fp_prev_ = float_fp_new_;
    float_fp_prev_count_ = static_cast<int>(float_fp_new_.size());
    line_lo_prev_.clear();
    line_hi_prev_.clear();
    const int n = static_cast<int>(layout_.lines.size());
    line_lo_prev_.resize(static_cast<size_t>(n));
    line_hi_prev_.resize(static_cast<size_t>(n));
    std::vector<float> lo;
    std::vector<float> hi;
    for (int p = 0; p < layout_.stats.page_count; ++p) {
        pageLineBands(p, &lo, &hi);
        for (int k : layout_.linesOnPage(p)) {
            line_lo_prev_[static_cast<size_t>(k)] = lo[static_cast<size_t>(k)];
            line_hi_prev_[static_cast<size_t>(k)] = hi[static_cast<size_t>(k)];
        }
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
    // 必须与页位图共用字体表：字形缓存按 strike 规格索引，规格里含字体对象的身份，
    //   各建一份 SkTypeface 就是两份互不相见的缓存：预热会静默地什么都不做
    //   （症状只是"没变快"）。
    prewarm_.shareFonts(font_owner_);
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
    // 只过这一页的字形，不整篇筛。预热是逐页调用的，整篇筛会让每次预热都付 O(全篇)。
    for (int k : layout_.glyphsOnPage(page)) {
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

/**
 * 一页的光栅，坐标全部页内化（原点 = 页左上角）。
 *
 * 所有纵向量都不加任何窗口偏移 —— 页位图不知道自己会被摆到画布的哪一页上，
 *   这正是"窗口平移不产生像素工作"的来源。
 * band 非空时只画一条横带，带外像素保持原样 —— 前提是调用方保证带外已经有一份
 *   同一页、同一光栅配置的完整像素（见 presentInternal 的 has_prev）。
 */
void EditorEngine::rasterIntoBitmap(Bitmap& target, int page_index, const float* band_lo_pt,
                                    const float* band_hi_pt) {
    const float zoom = cfg_.zoom;
    const int page_px_w = pagePxWidth();
    const int page_px_h = pagePxHeight();
    const float units_to_px = zoom / kFontScalePerPt;
    const bool band = band_lo_pt && band_hi_pt;

    const auto t_fill0 = Clock::now();
    if (band) {
        // 擦除范围向外扩 1 像素：相邻两条带的边界取两行中线，留 1 像素免得接缝上留一条线。
        const float y0 = std::max(0.0f, (*band_lo_pt) * zoom - 1.0f);
        const float y1 = std::min(static_cast<float>(page_px_h), (*band_hi_pt) * zoom + 1.0f);
        target.fillRect(Rect{0.0f, y0, static_cast<float>(page_px_w), std::max(0.0f, y1 - y0)},
                        cfg_.page_fill);
    } else {
        // 整页：页内是一片页面底色。页间隙不在这里 —— 它归宿主（两层纸底之间
        //   的那条缝），页位图只要页本身。
        target.fillRect(
            Rect{0.0f, 0.0f, static_cast<float>(page_px_w), static_cast<float>(page_px_h)},
            cfg_.page_fill);
    }
    // 页边框每次都补：上面那一笔底色会把它擦掉一条。连页下不画 —— 没有页，就没有页的边。
    if (cfg_.draw_page_border && !cfg_.page.continuous) {
        target.strokeRect(
            Rect{0.0f, 0.0f, static_cast<float>(page_px_w), static_cast<float>(page_px_h)},
            cfg_.page_border, 1.0f);
    }
    // 裁剪标记同理每次都补：分带重画时那一带的底色会把它擦掉，而它只由这一段代码画。
    //   八条短线（四角各两条）够便宜，不必按带筛。
    if (cfg_.show_margin_marks && !cfg_.page.continuous) {
        const PageMargins& pm = cfg_.page.margins;
        const float x0 = std::round(pm.left * zoom);
        const float y0 = std::round(pm.top * zoom);
        const float x1 = std::round(static_cast<float>(page_px_w) - pm.right * zoom);
        const float y1 = std::round(static_cast<float>(page_px_h) - pm.bottom * zoom);
        // 臂长固定 18 磅；边距本身比它窄时按边距的一半收（标记要留在页边距里，
        //   越进正文就变成压字的脏墨了）。
        const float narrow = std::min(std::min(pm.left, pm.right), std::min(pm.top, pm.bottom));
        const float arm_pt = std::min(kMarginMarkPt, narrow * 0.5f);
        const float len = std::round(arm_pt * zoom);
        if (len >= 1.0f && x1 - x0 > 2.0f * len && y1 - y0 > 2.0f * len) {
            const Color c = cfg_.margin_mark_color;
            // 每个角两条臂，都朝页边距那一侧伸（裁切标记的老规矩：线在成品之外）。
            //   伸进正文里会贴着第一个字的左上角，看着像一撇脏墨。
            //   朝外那一条多画一格，两个臂才在最角上那一点接住，不留 1 像素缺口。
            auto hArm = [&](float x, float y, bool left) {
                target.fillRect(Rect{left ? x - len : x, y, len + (left ? 1.0f : 0.0f), 1.0f}, c);
            };
            auto vArm = [&](float x, float y, bool up) {
                target.fillRect(Rect{x, up ? y - len : y, 1.0f, len + (up ? 1.0f : 0.0f)}, c);
            };
            hArm(x0, y0, true);
            vArm(x0, y0, true);
            hArm(x1, y0, false);
            vArm(x1, y0, true);
            hArm(x0, y1, true);
            vArm(x0, y1, false);
            hArm(x1, y1, false);
            vArm(x1, y1, false);
        }
    }
    raster_fill_ms_ += msSince(t_fill0);

    // 水印：衬在正文下方的那一层。画在格线与字形之前 —— 它是纸上的底纹，
    //   正文要压在它上面（Word 的"衬于文字下方"同一条）。连页下也画：条带就是一页。
    rasterWatermark(target, page_index, band_lo_pt, band_hi_pt);

    // 连页接缝：行按"行顶所在的条带"归属，一行最多跨一条边界，所以溢出只朝下流。
    //   上一条带的末行越过带底的那一截，要在本带的顶部再画一遍 —— 两条带各画自己
    //   看得见的那半，拼起来才是完整的一行。分页模式下不会发生（断页把行整个挪走）。
    const bool cont = cfg_.page.continuous;
    const float slab_pt = cfg_.page.slabHeightPt();
    // 位移量取位图高度（整像素），不取"带高 x 缩放"：后者带小数（A4 下 842 磅 x 1.333
    //   是 1122.7，而位图 1123 行），合上去之后段落标记的横笔正好落在两行之间 ——
    //   抗锯齿把它摊成两行浅灰，看着比别的标记粗一档。位图第 0 行就是本带顶端，
    //   减一整页高才是两带之间的真位移。
    const float spill_dy = -static_cast<float>(pagePxHeight());
    const bool spill = cont && page_index > 0;

    // 表格格线：画在页位图里（它不随光标变），在字形之前 —— 线压不到字上。
    //   每个格只画自己的外框，于是合并格内部不会多出几条线；跨页切开的那条边要补画
    //   （表格接着长在下一页，不是被裁掉），连页的条带切口不画（那不是页边界）。
    //
    //   分页处有两种断法，两种都要有那条边：一种是行被切开（cont_top / cont_bottom 记着），
    //   另一种是整行挪到下一页。后一种里"页脚最后一行"没有任何人给它画下边（下一行在
    //   下一页），于是表格在换页处看着没封口 —— 本片是本页最后一片时由它自己补上。
    //   上边不用管：每一片都画自己的上边（见下面那条），续页第一行本来就有。
    auto emitTables = [&](int page, float dy, bool spill_only) {
        if (layout_.placed_tables.empty()) return;
        for (const PlacedTable& pt : layout_.placed_tables) {
            for (size_t pi = 0; pi < pt.placed.size(); ++pi) {
                const PlacedRow& pr = pt.placed[pi];
                if (pr.page != page) continue;
                if (spill_only && !(pr.y + pr.h > slab_pt)) continue;
                // 下一片在别的页（或没有下一片）⇒ 这一片的下边没人画，得自己画。
                //   连页不算：那里的"下一片"只差一条条带，切口不是页边界。
                const bool seam_bottom =
                    (!cont) && (pi + 1 >= pt.placed.size() || pt.placed[pi + 1].page != page);
                const float top = pr.y * zoom + dy;
                const float bot = top + pr.h * zoom;
                if (bot <= top) continue;
                int row_seen = -1;
                int grid_col = 0;
                for (const LayoutCell& lc : pt.cells) {
                    if (lc.row != pr.row) continue;
                    if (row_seen != lc.row) {
                        row_seen = lc.row;
                        grid_col = 0;
                    }
                    const int c0 = grid_col;
                    grid_col += std::max(lc.col_span, 1);
                    const int c1 = grid_col;
                    if (c0 >= static_cast<int>(pt.col_x.size())) continue;
                    const int c1s = std::min(c1, static_cast<int>(pt.col_x.size()) - 1);
                    const float left = (pt.x + pt.col_x[static_cast<size_t>(c0)]) * zoom;
                    const float right = (pt.x + pt.col_x[static_cast<size_t>(c1s)]) * zoom;
                    if (right <= left) continue;
                    const int xl = static_cast<int>(std::lround(left));
                    const int xr = static_cast<int>(std::lround(right));
                    const int yt = static_cast<int>(std::lround(top));
                    const int yb = static_cast<int>(std::lround(bot));
                    if (xr <= xl || yb <= yt) continue;
                    // 先取整再画：fillRect 按 floor/ceil 取像素范围，浮点坐标下
                    //   "宽 1 磅"会被放大成两个像素 —— 那正是"边框比颜色更粗"的真因。
                    const float fxl = static_cast<float>(xl);
                    const float fyt = static_cast<float>(yt);
                    const float fh = static_cast<float>(yb - yt);
                    const float fw = static_cast<float>(xr - xl);
                    // 线一律画在边界坐标上（右边界就是 xr、下边界就是 yb），不要往格内
                    //   缩一格：缩了之后"本格的右边"与"邻格的左边"会落在相邻像素上，
                    //   中间那几条列边界就成了 2 像素宽，而外框仍是 1 像素 ——
                    //   看起来就是"竖线比横线粗"。
                    target.fillRect(Rect{fxl, fyt, 1.0f, fh}, cfg_.table_line);
                    target.fillRect(Rect{static_cast<float>(xr), fyt, 1.0f, fh}, cfg_.table_line);
                    const bool skip_cut = cont;
                    // 上边：每一片都画自己的上边（表的顶边、每一行的分隔线都靠它）。
                    //   连页的条带切口不算（那里的"上边"是同一行接下去的断点）。
                    if (!(skip_cut && pr.cont_top))
                        target.fillRect(Rect{fxl, fyt, fw, 1.0f}, cfg_.table_line);
                    // 下边：切出来的片段要补；本页最后一片也要补（下一行在下一页时没人给它画）；
                    //   表尾那一行照旧要画。
                    const bool last_row = (pr.row == pt.rows - 1);
                    if (!(skip_cut && pr.cont_bottom) &&
                        (pr.cont_bottom || last_row || seam_bottom))
                        target.fillRect(Rect{fxl, static_cast<float>(yb), fw, 1.0f},
                                        cfg_.table_line);
                }
            }
        }
    };

    // 页位图里有页面底色、格线、字形、原子框，最后是表单标记（见函数末尾）。
    //
    // 同一字体 / 颜色 / 朝向的字形攒成一段，一次交给光栅器（见 Bitmap::drawGlyphs）：
    //   逐字形调用时，光栅器那笔固定开销（Skia 侧要新建 paint、装配一遍字形运行）
    //   全摊在一个字上 —— 整页重画变成 10 ms 量级就是这么来的。
    //   段内的次序不能动：它就是绘制次序，重叠字形靠它决定压盖。
    auto emitGlyphs = [&](int page, float dy, bool spill_only) {
        std::vector<uint32_t> run_ids;
        std::vector<float> run_x;
        std::vector<float> run_y;
        hb_font_t* run_font = nullptr;
        Color run_color{};
        float run_rot = 0.0f;
        bool run_italic = false;
        auto flush_run = [&]() {
            if (run_ids.empty()) return;
            target.drawGlyphs(run_font, run_ids.data(), run_x.data(), run_y.data(),
                              static_cast<int>(run_ids.size()), units_to_px, run_color, run_rot,
                              run_italic);
            run_ids.clear();
            run_x.clear();
            run_y.clear();
        };
        // 下划线按"同一行同一段的连续字形"合成一条线：逐字形画会让每个字之间留一条
        //   抗锯齿缝，看起来是一串短划。段与段之间靠 underline 标记断开（同一行里
        //   下划线文本中间夹一段非下划线，线必须断在交界处）。
        int ul_line = -1;
        float ul_x0 = 0.0f;
        float ul_x1 = 0.0f;
        float ul_y = 0.0f;
        float ul_size = 0.0f;
        Color ul_color{};
        auto flush_ul = [&]() {
            if (ul_line < 0) return;
            if (ul_x1 > ul_x0) {
                // 位置与厚度都按这一段的字号算：线落在基线下方约 0.12 em，粗约 0.055 em，
                //   再细会碎、再粗会像删除线。
                // 粗细按字号算，并且取整到整像素：不取整时 1.28 px 会横跨两行像素，
                //   看上去比实际粗一倍（而且大字号小字号都是两行，看着像"粗细固定"）。
                const float thick = std::max(1.0f, std::round(ul_size * zoom * 0.04f));
                // 位置比 Word 再低一点点：贴着字底会把相邻字的底部连成一条。
                const float y = std::round((ul_y + ul_size * 0.14f) * zoom + dy);
                const float x0 = std::round(ul_x0 * zoom);
                const float x1 = std::round(ul_x1 * zoom);
                if (x1 > x0) target.fillRect(Rect{x0, y, x1 - x0, thick}, ul_color);
            }
            ul_line = -1;
        };
        // 这一页有没有"要裁到行盒"的行。绝大多数文档没有，于是逐字形只多一次布尔比较，
        //   一次 save/restore 都不做 —— 裁字是固定值行距那条少见的路上才付的钱。
        bool any_clip = false;
        for (int li : layout_.linesOnPage(page)) {
            if (layout_.lines[static_cast<size_t>(li)].clip_text) {
                any_clip = true;
                break;
            }
        }
        int clip_line = -1;
        // 进来时的裁剪栈深度。换行、收尾都无条件退回到这一层：裁剪一旦少退一次，
        //   位图的画布会一直带着它，往后每一拍画出来的字都缺一截。
        const int clip_mark = target.clipMark();
        // 只过这一页的字形（见 LayoutResult::page_glyphs）。
        //   整篇遍历 + 判页号：2000 段文档约 8 万个字形，每画一页都要过一遍，
        //   反推约 2 ms/页 —— 比真正画这一页的字形（742 个约 0.9 ms）还贵。
        for (int k : layout_.glyphsOnPage(page)) {
            const PlacedGlyph& pg = layout_.glyphs[static_cast<size_t>(k)];
            if (!pg.font) continue;
            // 接缝那一趟只取越过带底的字形；其余情况取整页。
            //   判据用"墨迹可能触到带底"，不是"基线越过带底"：基线上方还有整个字身，
            //   基线在带底之上一点点的字，下半截照样会越过接缝 —— 那一截只由这一趟画，
            //   漏掉就是接缝处少一行字。字形的墨迹不会超出以基线为中心的一个字身，
            //   所以按字身外扩是它的上界（多取的几个由位图自己裁掉）。
            if (spill_only && !(pg.y_pt + pg.size_pt > slab_pt - 1.0f)) continue;
            // 换行就得换裁剪框：先把手上一批画掉，再进出一次裁剪栈。行高比墨迹矮时
            //   字要裁到行盒里（固定值行距），装得下的行不进栈。
            if (any_clip && pg.line != clip_line) {
                clip_line = pg.line;
                const LineBox& lb = layout_.lines[static_cast<size_t>(pg.line)];
                // 上一行的裁剪到此为止：先把攒着的字形画掉，再退回进入时的深度。
                flush_run();
                flush_ul();
                target.restoreToMark(clip_mark);
                if (lb.clip_text) {
                    target.pushClipRect(Rect{0.0f, lb.slotLoPt() * zoom + dy,
                                             static_cast<float>(target.width()),
                                             lb.advance_pt * zoom});
                }
            }
            // 画带时按字形的纵向位置筛：行盒的块轴范围就是它的 ascent/descent，
            //   所以"落在带内"就是"属于这条带"。带向外扩了 1 像素，相邻带在中线相接。
            //   接缝那一趟要按平移之后的坐标筛（它画在本带的顶部）。
            const float y_pt = pg.y_pt + dy / zoom;
            if (band && (y_pt < *band_lo_pt - 1.0f || y_pt > *band_hi_pt + 1.0f)) continue;
            // 下划线：同一行内把连续带下划线的字形合成一条；换行或遇到不带下划线的
            //   字形时把上一段合上。右端取这个字形自己的 advance —— 按 0.6 em 估的话，
            //   行末那个汉字（1 em 宽）总会短掉四成。
            if (pg.underline) {
                if (ul_line >= 0 && pg.line != ul_line) flush_ul();
                if (ul_line < 0) {
                    ul_line = pg.line;
                    ul_x0 = pg.x_pt;
                    ul_y = pg.y_pt;
                    ul_size = pg.size_pt;
                    ul_color = pg.color;
                }
                const float adv = pg.advance_pt > 0.0f ? pg.advance_pt : pg.size_pt * 0.6f;
                ul_x1 = pg.x_pt + adv;
            } else {
                flush_ul();
            }
            // 选择类的记号由引擎自己画（方框 + 对勾 / 圆圈），而且要用记号色 ——
            //   它可能与该段文字的颜色不同，所以不能混进这一批按字色画掉。
            bool mark_on = false;
            bool mark_square = false;
            Color mark_color{};
            if (!pg.vertical && choiceMarkerOf(pg, &mark_on, &mark_square, &mark_color)) {
                flush_run();
                paintChoiceMarker(target, pg, mark_on, mark_square, mark_color, dy);
                continue;
            }
            hb_font_t* f = pg.font->hb();
            if (!run_ids.empty() && (f != run_font || pg.color != run_color ||
                                     pg.rotate_deg != run_rot || pg.italic != run_italic)) {
                flush_run();
            }
            if (run_ids.empty()) {
                run_font = f;
                run_color = pg.color;
                run_rot = pg.rotate_deg;
                run_italic = pg.italic;
            }
            run_ids.push_back(pg.glyph_id);
            run_x.push_back(pg.x_pt * zoom);
            run_y.push_back(pg.y_pt * zoom + dy);
        }
        flush_run();
        flush_ul();
        target.restoreToMark(clip_mark);
    };

    // 行内原子的占位框。与字形一样只过这一页的行（见 LayoutResult::page_lines），
    //   也照样按行带筛：一条带的边界就是行的块轴范围，框不会跨出它自己那一行。
    auto emitAtoms = [&](int page, float dy, bool spill_only) {
        for (int li : layout_.linesOnPage(page)) {
            const LineBox& lb = layout_.lines[static_cast<size_t>(li)];
            if (spill_only && !(lb.block_hi_pt > slab_pt)) continue;
            const float lo_pt = lb.block_lo_pt + dy / zoom;
            const float hi_pt = lb.block_hi_pt + dy / zoom;
            if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;
            for (size_t ci = 0; ci < lb.clusters.size(); ++ci) {
                const LineCluster& c = lb.clusters[ci];
                if (c.atom == InlineAtom::None) continue;
                // 框在行内轴上的末端 = 下一个簇的起点（最后一个簇用行末）。
                const float a1 = (ci + 1 < lb.clusters.size())
                                     ? lb.inline_origin_pt + lb.clusters[ci + 1].inline_pt
                                     : lb.inline_origin_pt + lb.inline_end_pt;
                drawAtomBox(target, lb, c, a1, dy);
            }
        }
    };

    // 浮动图。衬于文字下方的那一档画在字形之前，其余（四周型 / 上下型 / 浮于文字上方）
    //   画在字形与原子之后 —— 这个次序就是"环绕方式"在呈现上的全部含义。
    auto emitFloats = [&](int page, float dy, bool behind) {
        for (const PlacedFloat& pf : layout_.placed_floats) {
            if (pf.page != page) continue;
            if ((pf.wrap == ImageWrap::Behind) != behind) continue;
            // 拖动移动的预览：图按位移画在新位置，文字先留在原处（松手才绕排）。
            float fx = pf.x;
            float fy = pf.y;
            imagePreviewPos(pf, &fx, &fy);
            const float lo_pt = fy + dy / zoom;
            const float hi_pt = lo_pt + pf.h;
            if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;
            const Rect box{fx * zoom, lo_pt * zoom, pf.w * zoom, pf.h * zoom};
            if (!(box.w > 0.0f) || !(box.h > 0.0f)) continue;
            const rev::Node* n = rev::findNode(document(), static_cast<NodeId>(pf.handle));
            const ImageSpec* im = (n != nullptr) ? std::get_if<ImageSpec>(&n->payload) : nullptr;
            const ImageResource* res = (im != nullptr && !im->resource_id.empty())
                                           ? images_.find(im->resource_id)
                                           : nullptr;
            if (res != nullptr) {
                target.drawImage(imageResourceKey(res->id), res->version, res->rgba.data(),
                                 res->width, res->height, box);
            } else {
                // 资源没注册（文件刚打开）：画一个浅框占位，与行内图同一口径。
                target.fillRect(box, cfg_.atom_fill);
                target.strokeRect(box, cfg_.atom_border, 1.0f);
            }
        }
    };

    const auto t_glyph0 = Clock::now();
    emitFloats(page_index, 0.0f, true);
    emitTables(page_index, 0.0f, false);
    emitGlyphs(page_index, 0.0f, false);
    emitAtoms(page_index, 0.0f, false);
    emitFloats(page_index, 0.0f, false);
    if (spill) {
        emitFloats(page_index - 1, spill_dy, true);
        emitTables(page_index - 1, spill_dy, true);
        emitGlyphs(page_index - 1, spill_dy, true);
        emitAtoms(page_index - 1, spill_dy, true);
        emitFloats(page_index - 1, spill_dy, false);
    }
    raster_glyph_ms_ += msSince(t_glyph0);

    // 表单标记放在最后：括号要压在原子框之上（它是内容的最后一层），
    //   小点压在内容之上 —— 一个方块盖住几个字的角，比起被字盖掉、看着像脏点要好。
    //   编辑标记先画：它是注释性的浅灰，被括号或小点压住不心疼。
    {
        const auto t_mark0 = Clock::now();
        rasterEditingMarks(target, page_index, band_lo_pt, band_hi_pt, 0.0f, false);
        if (spill)
            rasterEditingMarks(target, page_index - 1, band_lo_pt, band_hi_pt, spill_dy, true);
        rasterControlMarks(target, page_index, band_lo_pt, band_hi_pt, 0.0f, false);
        if (spill)
            rasterControlMarks(target, page_index - 1, band_lo_pt, band_hi_pt, spill_dy, true);
        raster_glyph_ms_ += msSince(t_mark0);
    }
}

namespace {

/**
 * 两个矩形相交，或取整之后会挨在一起。
 */
bool rectsTouch(const Rect& a, const Rect& b) {
    return a.x <= b.x + b.w + 1.0f && b.x <= a.x + a.w + 1.0f && a.y <= b.y + b.h + 1.0f &&
           b.y <= a.y + a.h + 1.0f;
}

Rect rectUnion(const Rect& a, const Rect& b) {
    const float x0 = std::min(a.x, b.x);
    const float y0 = std::min(a.y, b.y);
    const float x1 = std::max(a.x + a.w, b.x + b.w);
    const float y1 = std::max(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

bool EditorEngine::pageBitmapFresh(int page) const {
    const auto it = page_cache_.find(page);
    if (it == page_cache_.end()) return false;
    const PageBitmap& pb = it->second;
    if (!pb.valid) return false;
    if (pb.px.width() != pagePxWidth() || pb.px.height() != pagePxHeight()) return false;
    // 页号超出指纹数组时不敢断言"作数"：那种页没有可供比较的版本号。
    if (page < 0 || page >= page_fp_new_count_) return false;
    // 内容指纹之外还有一份"光栅配置"的签名：底色 / 边框 / 缩放 / 页度量这些变了，
    //   内容一个字节都没动，但画出来的像素不一样。签名是这里唯一的判据 —— 不再靠
    //   setConfig 记得清缓存（TRAPS 128）。
    if (pb.raster_sig != raster_sig_) return false;
    return pb.fp == page_fp_new_[static_cast<size_t>(page)];
}

void EditorEngine::rasterPageInto(int page, const float* band_lo_pt, const float* band_hi_pt) {
    PageBitmap& pb = page_cache_[page];
    const int pw = pagePxWidth();
    const int ph = pagePxHeight();
    if (pb.px.width() != pw || pb.px.height() != ph) {
        pb.px.reset(pw, ph);
        // 与其余位图共用同一张字体表：字形缓存按 strike 规格索引，规格里含字体对象的
        //   身份，各建一份就是互不相见的几份缓存，预热会静默地什么都不做。
        pb.px.shareFonts(font_owner_);
        pb.valid = false;
    }
    pb.px.resetPaintedPixels();
    rasterIntoBitmap(pb.px, page, band_lo_pt, band_hi_pt);
    // 记的是"这一份像素对应哪一版指纹"。哨兵值也算一版：markPagesChangedFrom
    //   每次都会按已画那一版翻一下低位，同一个哨兵不会连着出现两次。
    pb.fp = (page < page_fp_new_count_) ? page_fp_new_[static_cast<size_t>(page)] : 0;
    pb.raster_sig = raster_sig_;
    pb.valid = true;
    raster_glyphs_ += static_cast<int>(pb.px.paintedGlyphs());
    raster_glyph_bounds_ms_ += pb.px.glyphBoundsMs();
}

void EditorEngine::trimPageCache(int first_page, int count) {
    // 窗口内一页都不丢：它们是下一拍很可能还要用的，而且页位图就是导出像素的唯一来源 ——
    //   丢了这一段，窗口里就有几页拿不出像素。
    //   注意别在这里做"整篇窗口就整份清掉"那种省内存的优化：它会让导出图变成一片背景色。
    const std::size_t cap = static_cast<std::size_t>(std::max(count + 2, 4));
    if (page_cache_.size() <= cap) return;
    std::vector<std::pair<int, int>> far;  // (距离, 页号)
    far.reserve(page_cache_.size());
    for (const auto& kv : page_cache_) {
        const int p = kv.first;
        if (p >= first_page && p < first_page + count) continue;
        const int d = (p < first_page) ? (first_page - p) : (p - (first_page + count - 1));
        far.push_back({d, p});
    }
    std::sort(far.begin(), far.end(),
              [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second > b.second;
              });
    for (const auto& e : far) {
        if (page_cache_.size() <= cap) break;
        page_cache_.erase(e.second);
        page_dirty_rects_.erase(e.second);
    }
}

/**
 * 记一块"这一页的像素变了"。页内像素坐标，且是累积的（自上次取走以来）。
 *
 * 合并规则：相交或相接的并成一个（光标擦旧画新就是这种），相距远的不并 ——
 *   并了会把中间一大片没变的像素也传上去。条数超过 16 时退回一个并集：
 *   一页几十个表单标记时逐条传反而更贵，而这一条只是"上界"，宁可多传不可少传。
 */
void EditorEngine::notePageDirty(int page, const Rect& r) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    std::vector<Rect>& v = page_dirty_rects_[page];
    v.push_back(r);
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < v.size() && !merged; ++i) {
            for (size_t j = i + 1; j < v.size(); ++j) {
                if (!rectsTouch(v[i], v[j])) continue;
                v[i] = rectUnion(v[i], v[j]);
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(j));
                merged = true;
                break;
            }
        }
    }
    if (v.size() > 16) {
        Rect u = v.front();
        for (const Rect& q : v) u = rectUnion(u, q);
        v.assign(1, u);
    }
}

std::vector<Rect> EditorEngine::takePageDirtyRects(int page) {
    std::vector<Rect> out;
    const auto it = page_dirty_rects_.find(page);
    if (it == page_dirty_rects_.end()) return out;
    out.swap(it->second);
    page_dirty_rects_.erase(it);
    return out;
}

/**
 * 有累积脏矩形的页号（升序）。与 takePageDirtyRects 的分工见头文件。
 */
std::string EditorEngine::pageDirtyPagesJson() const {
    std::vector<int> pages;
    pages.reserve(page_dirty_rects_.size());
    for (const auto& kv : page_dirty_rects_) {
        if (!kv.second.empty()) pages.push_back(kv.first);
    }
    std::sort(pages.begin(), pages.end());
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < pages.size(); ++i) {
        if (i != 0) o << ",";
        o << pages[i];
    }
    o << "]";
    return o.str();
}

std::string EditorEngine::takePageDirtyRectsJson(int page) {
    const std::vector<Rect> rs = takePageDirtyRects(page);
    const int pw = pagePxWidth();
    const int ph = pagePxHeight();
    std::ostringstream o;
    o << "[";
    bool first = true;
    for (const Rect& r : rs) {
        // 取整向外扩：宁可多传一行，不能少传 —— 少传的那一行会留在画布上，
        //   而它是上一帧的像素，看起来就是"版面上一条旧墨迹"。
        int x0 = static_cast<int>(std::floor(r.x));
        int y0 = static_cast<int>(std::floor(r.y));
        int x1 = static_cast<int>(std::ceil(r.x + r.w));
        int y1 = static_cast<int>(std::ceil(r.y + r.h));
        x0 = std::max(0, std::min(x0, pw));
        y0 = std::max(0, std::min(y0, ph));
        x1 = std::max(x0, std::min(x1, pw));
        y1 = std::max(y0, std::min(y1, ph));
        if (x1 <= x0 || y1 <= y0) continue;
        if (!first) o << ",";
        first = false;
        o << "{\"x\":" << x0 << ",\"y\":" << y0 << ",\"w\":" << (x1 - x0) << ",\"h\":" << (y1 - y0)
          << "}";
    }
    o << "]";
    return o.str();
}

size_t EditorEngine::pageBytes(int page) const {
    if (!pageBitmapFresh(page)) return 0;
    const auto it = page_cache_.find(page);
    if (it == page_cache_.end()) return 0;
    return it->second.px.byteSize();
}

const uint8_t* EditorEngine::pagePixels(int page) const { return pageDisplayPixels(page); }

/**
 * 一次呈现：保证窗口内每一页的内容新鲜，并记下这一拍变了的矩形。
 *
 * 这里不产生任何整窗像素：渲染只写页位图，光标与选区是导出时才叠上去的瞬态
 *   （见 pageDisplayPixels）。于是没有槽（页的位置由页号直接定，宿主自己就算得出）、
 *   没有"这一格装的是别的页"（页位图按页号索引，窗口平移不产生像素工作），
 *   也没有"擦旧瞬态"（瞬态不在任何可写位图里，下一次导出从干净的页位图重来）——
 *   最后这一条把"记漏一次就永久残留"那个失败模式整类消掉了。
 */
void EditorEngine::presentInternal(int first_page, int count) {
    const auto t0 = Clock::now();
    auto spanMs = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    ensureRenderSetup();
    // 光栅签名在这里按当前状态重算一次：它是"这一页的像素还算不算数"的唯一判据
    //   （pageBitmapFresh 只比它）。写进某几个 setter 里迟早漏 —— 水印是节属性、
    //   图片代际挂在资源表上、编辑标记看模式，漏一项的症状就是"改了之后屏幕不变"。
    {
        const uint64_t sig = rasterSignatureNow();
        if (sig != raster_sig_) {
            raster_sig_ = sig;
            page_dirty_rects_.clear();  // 旧矩形描述的是旧像素，作废
        }
    }
    // 光栅那一组由 rasterIntoBitmap 累加，末尾一次性上报。
    raster_glyphs_ = 0;
    raster_glyph_bounds_ms_ = 0.0;
    raster_fill_ms_ = 0.0;
    raster_glyph_ms_ = 0.0;

    const auto t_pre = Clock::now();
    const float zoom = cfg_.zoom;
    const int page_px_w = pagePxWidth();
    const int page_px_h = pagePxHeight();
    const int step = page_px_h + pageGapPx();

    const int total_pages = std::max(1, stats_.page_count);
    if (first_page < 0) first_page = 0;
    if (first_page > total_pages - 1) first_page = total_pages - 1;
    count = std::max(1, std::min(count, total_pages - first_page));
    count = std::min(count, 64);  // 防御：异常文档别把窗口撑爆

    win_first_page_ = first_page;
    win_page_count_ = count;
    // 导出图是派生量：这一拍之后重拼（见 ensureFrameExport）。
    frame_export_valid_ = false;

    // 脏页数（在指纹账被刷新之前算，否则比不出差异）
    {
        int dp = 0;
        const int n = std::max(page_fp_new_count_, page_fp_prev_count_);
        for (int i = 0; i < n; ++i) {
            const uint64_t a = (i < page_fp_new_count_) ? page_fp_new_[static_cast<size_t>(i)] : 0;
            const uint64_t b =
                (i < page_fp_prev_count_) ? page_fp_prev_[static_cast<size_t>(i)] : 0;
            if (a != b) ++dp;
        }
        edit_stats_.dirty_page_count = dp;
    }

    // 行带：把"这一页要重画"细化成"这一页的哪几行要重画"。
    //   一页约 1000 个字形，而敲一个字只动其中一两行 —— 页粒度下改一行与改整页同价
    //   （实测字形光栅 10~14 ms，其中九成是白画的）。
    //   结果写进 band_lo_scratch_ / band_hi_scratch_（页内磅）。
    //   返回 -1 = 这一页一行都没变；0 = 要整页重画；1 = 只重画一条带。
    //
    //   竖排页整页重画：行盒的"块轴"在竖排下是横轴，行带会变成竖条，
    //     而擦除矩形、字形筛选都按纵轴写；竖排文档少，不值得为它多一套几何。
    auto computeBand = [&](int page) -> int {
        const std::vector<int>& page_line_ids = layout_.linesOnPage(page);
        for (int k : page_line_ids) {
            if (layout_.lines[static_cast<size_t>(k)].vertical) return 0;
        }
        // 这一页的格线换了形状就整页重画：行带只盖住文字那一条，而页底的切口边
        //   （从"行底"变成"页底"、或反过来）落在带外，留着就是一条旧边框。
        if (page < table_fp_prev_count_ && page < static_cast<int>(table_fp_new_.size()) &&
            table_fp_new_[static_cast<size_t>(page)] != table_fp_prev_[static_cast<size_t>(page)]) {
            return 0;
        }
        // 浮动图同理，而且更硬：它整块落在行带之外（一张图可能比一条带还高），
        //   行带只盖住文字那一条，图那一块永远补不回来。
        if (page < float_fp_prev_count_ && page < static_cast<int>(float_fp_new_.size()) &&
            float_fp_new_[static_cast<size_t>(page)] != float_fp_prev_[static_cast<size_t>(page)]) {
            return 0;
        }
        int d0 = -1;
        for (int k : page_line_ids) {
            const bool same = k < line_fp_prev_count_ && line_fp_new_[static_cast<size_t>(k)] ==
                                                             line_fp_prev_[static_cast<size_t>(k)];
            if (same) continue;
            d0 = k;
            break;
        }
        if (d0 < 0) return -1;
        pageLineBands(page, &band_lo_scratch_all_, &band_hi_scratch_all_);
        // 带的两端取"所有变化行"的并集，而不是下标最小/最大的那两行：
        //   表格里数组顺序不等于版面上下顺序（一条行切页之后，同一行其余各格的行
        //   还在上一页），按下标取端点会把带压到页首那一条 —— 页下半截的字与标记
        //   就再也不会重画（屏幕上留的是旧墨迹）。
        float lo = 1.0e30f;
        float hi = -1.0e30f;
        for (int k : page_line_ids) {
            const bool same = k < line_fp_prev_count_ && line_fp_new_[static_cast<size_t>(k)] ==
                                                             line_fp_prev_[static_cast<size_t>(k)];
            if (same) continue;
            lo = std::min(lo, band_lo_scratch_all_[static_cast<size_t>(k)]);
            hi = std::max(hi, band_hi_scratch_all_[static_cast<size_t>(k)]);
            // 新旧两版里更靠外的那个边界：行往下挪之后旧墨迹在更靠上的位置，
            //   只擦新位置就会留下幽灵字（auditPage 正是抓这个的判据）。
            if (k < static_cast<int>(line_lo_prev_.size()))
                lo = std::min(lo, line_lo_prev_[static_cast<size_t>(k)]);
            if (k < static_cast<int>(line_hi_prev_.size()))
                hi = std::max(hi, line_hi_prev_[static_cast<size_t>(k)]);
        }
        // 页里的行数比已画那一版少（删掉了行）⇒ 尾巴一直擦到页底，否则旧墨迹留在那儿
        if (line_fp_prev_count_ > static_cast<int>(layout_.lines.size()))
            hi = static_cast<float>(page_px_h) / zoom;
        band_lo_scratch_ = lo;
        band_hi_scratch_ = hi;
        return 1;
    };

    // 这一拍的账：脏矩形面积之和 + 窗口坐标下的并集盒。
    //   口径是"要重传多少像素"，与 host 真正要贴的东西完全一致 ——
    //   旧版记的是"帧上写了多少次"，那是渲染目标的账，而渲染目标已经不在了。
    long long dirty_px = 0;
    float ux0 = 0.0f;
    float uy0 = 0.0f;
    float ux1 = -1.0f;
    float uy1 = -1.0f;
    bool any = false;
    last_dirty_pages_.clear();
    auto markDirty = [&](int page, const Rect& r) {
        if (!(r.w > 0.0f) || !(r.h > 0.0f)) return;
        notePageDirty(page, r);
        dirty_px += static_cast<long long>(std::ceil(r.w)) * static_cast<long long>(std::ceil(r.h));
        // 页内 y -> 窗口 y：页号是绝对的，窗口只是"从哪一页开始看"。
        const float wy = static_cast<float>((page - first_page) * step) + r.y;
        if (!any) {
            ux0 = r.x;
            uy0 = wy;
            ux1 = r.x + r.w;
            uy1 = wy + r.h;
            any = true;
        } else {
            ux0 = std::min(ux0, r.x);
            uy0 = std::min(uy0, wy);
            ux1 = std::max(ux1, r.x + r.w);
            uy1 = std::max(uy1, wy + r.h);
        }
        if (std::find(last_dirty_pages_.begin(), last_dirty_pages_.end(), page) ==
            last_dirty_pages_.end()) {
            last_dirty_pages_.push_back(page);
        }
    };

    // 一、内容：页位图不新鲜就重光栅（整页或一条行带），并把那一块记成脏。
    int rastered = 0;
    for (int i = 0; i < count; ++i) {
        const int page = first_page + i;
        if (pageBitmapFresh(page)) continue;
        ++rastered;
        // 行带只对"缓存里已有一份完整像素"的页成立：带外按"没变"处理。
        //   页是第一次画（或换过尺寸 / 光栅配置、被淘汰后重进窗口）时必须整页 ——
        //   否则带外留在 bitmap 里的就是零（黑块），而它永远补不回来
        //   （下一次它已经"有像素"了，这块黑永远补不回来）。整窗帧在时，"首次全建"
        //   那一支会绕过行带；现在没有那一支，判据只能落在这里。
        const auto it = page_cache_.find(page);
        const bool has_prev =
            it != page_cache_.end() && it->second.valid && it->second.raster_sig == raster_sig_ &&
            it->second.px.width() == page_px_w && it->second.px.height() == page_px_h;
        const int band = has_prev ? computeBand(page) : 0;
        if (band <= 0) {
            // -1：页指纹变了而行指纹全同（只有页数 / 页间隙 / 页边框这类页外属性变了）
            //  0：竖排页 —— 两种都退回整页重画，宁可多画也不留幽灵。
            rasterPageInto(page, nullptr, nullptr);
            markDirty(page, Rect{0.0f, 0.0f, static_cast<float>(page_px_w),
                                 static_cast<float>(page_px_h)});
            edit_stats_.band_y0 = 0;
            edit_stats_.band_y1 = page_px_h;
            continue;
        }
        rasterPageInto(page, &band_lo_scratch_, &band_hi_scratch_);
        // 取整口径必须与 notePageDirty 的取整完全一致（floor 起点 / ceil 终点）：
        //   少取一行就是"版面上留一条旧墨迹"，只在恰巧跨半像素时出现。
        const float y0 = std::max(0.0f, band_lo_scratch_ * zoom - 1.0f);
        const float y1 = std::min(static_cast<float>(page_px_h), band_hi_scratch_ * zoom + 1.0f);
        markDirty(page, Rect{0.0f, y0, static_cast<float>(page_px_w), std::max(1.0f, y1 - y0)});
        edit_stats_.band_y0 = static_cast<int>(y0);
        edit_stats_.band_y1 = static_cast<int>(y1);
    }
    edit_stats_.repainted_pages = rastered;
    page_fp_prev_ = page_fp_new_;
    page_fp_prev_count_ = page_fp_new_count_;
    // 行几何快照是 O(全部行) 的（2000 段 = 4005 行、约 1.2 ms），它描述的是"页位图里装的是
    //   哪些行"，只有排版真的跑过才可能变 —— 与窗口挪不挪、有没有脏页都无关。
    if (layout_pending_) snapshotLineGeometry();
    // 淘汰放在所有光栅之后：这一拍用到的页一页都不能丢，窗口外再按距离释放。
    trimPageCache(first_page, count);

    const auto t_swap = Clock::now();

    // 二、瞬态：按页算"这一页现在该叠哪些矩形"，与上一拍记下的那份做差。
    //   只有真的变了才记脏 —— 光标每闪一下都会走这里，没变的页不该产生任何上传。
    //   选区在前、光标在后：导出时按这个顺序叠（选区混色在下、光标实心在上）。
    for (int i = 0; i < count; ++i) {
        const int page = first_page + i;
        const auto it = page_cache_.find(page);
        if (it == page_cache_.end()) continue;
        PageTransient now;
        collectTransient(page, &now);
        const PageTransient& prev = it->second.transient;
        auto sameRect = [](const Rect& a, const Rect& b) {
            return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
        };
        auto has = [&](const std::vector<Rect>& v, const Rect& r) {
            for (const Rect& q : v) {
                if (sameRect(q, r)) return true;
            }
            return false;
        };
        for (const Rect& r : prev.sel) {
            if (!has(now.sel, r)) markDirty(page, r);
        }
        for (const Rect& r : now.sel) {
            if (!has(prev.sel, r)) markDirty(page, r);
        }
        for (const Rect& r : prev.image_ui) {
            if (!has(now.image_ui, r)) markDirty(page, r);
        }
        for (const Rect& r : now.image_ui) {
            if (!has(prev.image_ui, r)) markDirty(page, r);
        }
        if (prev.caret.w > 0.0f && !sameRect(prev.caret, now.caret)) markDirty(page, prev.caret);
        if (now.caret.w > 0.0f && !sameRect(prev.caret, now.caret)) markDirty(page, now.caret);
        it->second.transient = now;
    }
    // 窗口外的页不留瞬态账：它记的是"上一拍这一页叠了什么"，而那一页已经不在画布上。
    //   留着的话，读 pageRgba(窗口外的页) 会得到一个过期光标 —— 探针正是那么读的。
    for (auto& kv : page_cache_) {
        if (kv.first >= first_page && kv.first < first_page + count) continue;
        kv.second.transient.sel.clear();
        kv.second.transient.image_ui.clear();
        kv.second.transient.caret = Rect{};
    }

    const auto t_blit = Clock::now();

    // 三、记账
    edit_stats_.repainted_pixels = dirty_px;
    stats_.painted_glyphs = raster_glyphs_;
    edit_stats_.frame_w = frameWidth();
    edit_stats_.frame_h = frameHeight();
    edit_stats_.repainted_x = any ? static_cast<int>(std::floor(ux0)) : 0;
    edit_stats_.repainted_y = any ? static_cast<int>(std::floor(uy0)) : 0;
    edit_stats_.repainted_w = any ? static_cast<int>(std::ceil(ux1 - ux0)) : 0;
    edit_stats_.repainted_h = any ? static_cast<int>(std::ceil(uy1 - uy0)) : 0;
    std::sort(last_dirty_pages_.begin(), last_dirty_pages_.end());

    const auto t_overlay = Clock::now();

    // 光标那几个数字一次算完：caret() 是投影（含一次二分），反复调既没必要也让读数
    //   有可能落在两个状态之间（下一次投影的代价换来的是"同一帧里的数来自同一处"）。
    // 注意坐标口径：caret_x / caret_y 是页内像素，页号另给。宿主按 (页号 x 步距)
    //   加回去 —— 引擎不再产出任何"窗口内的 y"（那会让同一个光标在窗口挪动前后
    //   给出两个数，读的一方还得再减一次才能还原）。
    int caret_page = -1;
    const Rect cr = caretRectPx(&caret_page);
    const TextPos caret_now = caret();
    edit_stats_.caret_para = caret_now.para;
    edit_stats_.caret_offset = caret_now.offset;
    edit_stats_.caret_side = caretSide();
    edit_stats_.caret_vertical = caretVertical();
    edit_stats_.caret_block_reversed = caretBlockReversed();
    edit_stats_.caret_page = caret_.caretPage();
    // 光标只在可插入位置上现形：表单模式的控件之外、只读模式里一律不画（见 caretInsertable）。
    //   选中图片时也不画：那是一条对象选区，文本光标留着只会叠在图的右边线上，
    //   把把手盖住（那一段既看不清也点不准）。
    edit_stats_.caret_visible = (caretShown() && cr.w > 0.0f);
    edit_stats_.caret_x_px = static_cast<int>(cr.x);
    edit_stats_.caret_y_px = static_cast<int>(cr.y);
    edit_stats_.caret_w_px = static_cast<int>(cr.w);
    edit_stats_.caret_h_px = static_cast<int>(cr.h);
    (void)caret_page;

    // 表单
    // caret_control 用 include_end = true：光标停在字段末尾也算"在这个控件里"，
    //   与编辑归属同一口径（focusedControlIndex），两者永远不会互相矛盾。
    // 例外是点画布带进来的归属提示：共享边界上它比"最内层"更准，走同一个
    //   focusedControlIndex（面板/探针看到的必须与实际落笔的字段一致）。
    edit_stats_.edit_mode = static_cast<int>(mode_);
    edit_stats_.caret_control = focusedControlIndex();
    edit_stats_.blocked_edits = blocked_edits_;
    edit_stats_.caret_index = caret_now.offset;
    for (int i = 0; i < caret_now.para && i < static_cast<int>(blocks().size()); ++i)
        edit_stats_.caret_index += blocks()[static_cast<size_t>(i)].cp_text.size();

    // 本轮有没有重排：只有呈现这一步能回答，所以这一格仍在这里结算。数字本身由
    //   relayout() 发布，这里不改写 —— 它们是"最近一次排版"的账，不是"这一拍"的。
    edit_stats_.layout_ran = layout_pending_;
    if (layout_pending_) layout_pending_ = false;

    const auto t_ledger = Clock::now();
    const double paint_ms = msSince(t0);
    stats_.paint_ms = paint_ms;
    stats_.paint_fill_ms = raster_fill_ms_;
    stats_.paint_glyph_ms = raster_glyph_ms_;
    // 记账那一笔含在字形时间里，这里只是把它单独报出来（口径见 EngineStats）。
    //   取数的时机在整页循环之后、各页位图重置记账之前 —— 与 painted_glyphs 同一份账。
    stats_.paint_glyph_bounds_ms = raster_glyph_bounds_ms_;
    // 减掉前两段，剩下的就是"脏矩形记账 + 导出准备"这一段 —— 比再插一个计时点更不容易漏。
    stats_.paint_present_ms = std::max(0.0, paint_ms - raster_fill_ms_ - raster_glyph_ms_);
    stats_.paint_pre_ms = spanMs(t0, t_pre);
    stats_.paint_swap_ms = spanMs(t_pre, t_swap);
    stats_.paint_transient_ms = spanMs(t_swap, t_blit);
    stats_.paint_overlay_ms = spanMs(t_blit, t_overlay);
    stats_.paint_ledger_ms = spanMs(t_overlay, t_ledger);
    stats_.paint_report_ms = spanMs(t_ledger, Clock::now());
    stats_.frame_width = frameWidth();
    stats_.frame_height = frameHeight();
    edit_stats_.paint_ms = paint_ms;
    edit_stats_.total_pages = total_pages;
    edit_stats_.window_pages = count;
    edit_stats_.window_first_page = first_page;
    // 合计只算这一拍真正发生过的段：排版账是"最近一次"的，没有重排的这一拍不能把它加进来，
    //   否则光标闪烁会每次多报一次排版耗时。
    edit_stats_.total_ms = paint_ms;
    if (edit_stats_.layout_ran) {
        edit_stats_.total_ms += edit_stats_.layout_ms + edit_stats_.page_fp_ms;
    }

    // 字形缓存的预算与占用（Skia 的全局 strike 缓存）。
    stats_.glyph_cache_bytes = font_owner_.glyphCacheUsedBytes();
    stats_.glyph_cache_limit = font_owner_.glyphCacheBudgetBytes();
}

/**
 * 这一页现在该叠哪些瞬态：选区矩形（页内像素）+ 光标矩形（只落在光标那一页）。
 *
 * 一处算、两处用：presentInternal 按页算差集，pageDisplayPixels 按页取来叠。
 *   两处各写一份的话，"记脏的矩形"与"画上去的矩形"迟早分叉 —— 那正是旧版
 *   transient_dirty 漏 93 px 的形状（画了但没记，画布上留一块旧像素）。
 */
void EditorEngine::collectTransient(int page, PageTransient* out) const {
    out->sel.clear();
    out->caret = Rect{};
    // 图片拖拽框：与选区同一层（呈现时叠、导出时不叠），所以它跟着这里一起算。
    collectImageUi(page, &out->image_ui);
    for (const PageRect& pr : selectionRectsPxIn(page, 1)) out->sel.push_back(pr.r_pt);
    int cp = -1;
    const Rect cr = caretRectPx(&cp);
    const bool draw_caret = caretShown();
    if (cp == page && draw_caret && cr.w > 0.0f && cr.h > 0.0f) out->caret = cr;

    // 连页接缝：上一条带里越过带底的选区与光标，在本带顶部还有一半。
    //   两带各画自己那半，与内容的补画是同一条规则 —— 否则接缝上会出现
    //   "字补上了、底色高亮只到一半"。
    if (!cfg_.page.continuous || page <= 0) return;
    const float slab_px = cfg_.page.slabHeightPt() * cfg_.zoom;
    const float ph = static_cast<float>(pagePxHeight());
    for (const PageRect& pr : selectionRectsPxIn(page - 1, 1)) {
        const float bot = pr.r_pt.y + pr.r_pt.h;
        if (bot <= slab_px) continue;
        const float y0 = std::max(0.0f, pr.r_pt.y - slab_px);
        const float y1 = std::min(ph, bot - slab_px);
        if (y1 > y0) out->sel.push_back(Rect{pr.r_pt.x, y0, pr.r_pt.w, y1 - y0});
    }
    if (cp == page - 1 && draw_caret && cr.w > 0.0f && cr.h > 0.0f && out->caret.w <= 0.0f) {
        const float bot = cr.y + cr.h;
        const float y0 = std::max(0.0f, cr.y - slab_px);
        const float y1 = std::min(ph, bot - slab_px);
        if (bot > slab_px && y1 > y0) out->caret = Rect{cr.x, y0, cr.w, y1 - y0};
    }
}

/**
 * 一页"画到画布上应该是什么"：干净页位图 + 这一页的瞬态。
 *
 * 没有瞬态的页直接给页位图本身（窗口里通常有四页属于这一档，一页 3.5 MB 的拷贝能省就省）。
 * 有瞬态时拷进 page_export_ 再叠 —— 拷的是内容、叠的是瞬态，于是"旧光标"不可能残留：
 *   它从来没被写进任何持久位图里。
 */
const uint8_t* EditorEngine::pageDisplayPixels(int page) const {
    const auto it = page_cache_.find(page);
    if (it == page_cache_.end() || !it->second.valid) return nullptr;
    const PageBitmap& pb = it->second;
    if (pb.px.width() != pagePxWidth() || pb.px.height() != pagePxHeight()) return nullptr;
    if (pb.transient.sel.empty() && pb.transient.image_ui.empty() &&
        !(pb.transient.caret.w > 0.0f)) {
        return pb.px.data();
    }

    const int pw = pb.px.width();
    const int ph = pb.px.height();
    if (page_export_.width() != pw || page_export_.height() != ph) {
        page_export_.reset(pw, ph);
        page_export_.shareFonts(font_owner_);
    }
    page_export_.blitFrom(pb.px, Rect{0.0f, 0.0f, static_cast<float>(pw), static_cast<float>(ph)},
                          0, 0);
    for (const Rect& r : pb.transient.sel) page_export_.blendRect(r, cfg_.selection_color);
    // 拖拽框与把手画在选区之上、光标之下：它们比选区更需要看得清，而光标是最后一条。
    for (const Rect& r : pb.transient.image_ui) page_export_.fillRect(r, cfg_.image_mark);
    if (pb.transient.caret.w > 0.0f) page_export_.fillRect(pb.transient.caret, cfg_.caret_color);
    page_export_page_ = page;
    return page_export_.data();
}

/**
 * 窗口导出图：按窗口顺序把各页拼成一张。
 *
 * 它只是给"扫一遍画面"那类判据（像素断言、markCheck、inkRows、auditFrame）的一个只读视图，
 *   不是渲染目标：唯一的数据源是页位图，拼完就缓存到下一次 present。页与页之间那一格
 *   间隙填背景色 —— 引擎不再拥有它（页间隙归宿主），这里留着只是为了保持旧帧的形状，
 *   让那些按 y 扫的判据不必认识"窗口里有几页"。
 *
 * @p with_transient 为假时只拼内容（审计用它区分"内容画错"与"瞬态叠错"）。
 */
void EditorEngine::assembleWindow(bool with_transient, Bitmap* out) const {
    const int fw = pagePxWidth();
    const int fh = frameHeight();
    out->reset(fw, fh);
    out->shareFonts(font_owner_);
    out->clear(cfg_.background);
    const int ph = pagePxHeight();
    const int step = ph + pageGapPx();
    const size_t row_bytes = static_cast<size_t>(fw) * 4u;
    for (int i = 0; i < win_page_count_; ++i) {
        const int page = win_first_page_ + i;
        const uint8_t* src = nullptr;
        if (with_transient) {
            src = pageDisplayPixels(page);
        } else {
            const auto it = page_cache_.find(page);
            if (it != page_cache_.end() && it->second.valid) src = it->second.px.data();
        }
        if (src == nullptr) continue;
        // 页宽就是帧宽，所以按行整段拷：两侧都不需要裁剪，也不会出现采样。
        uint8_t* dst = out->data() + static_cast<size_t>(i * step) * row_bytes;
        const int rows = std::min(ph, fh - i * step);
        if (rows <= 0) break;
        std::memcpy(dst, src, row_bytes * static_cast<size_t>(rows));
    }
}

size_t EditorEngine::ensureFrameExport() const {
    const int fw = pagePxWidth();
    const int fh = frameHeight();
    if (frame_export_valid_ && frame_export_.width() == fw && frame_export_.height() == fh) {
        return frame_export_.byteSize();
    }
    assembleWindow(true, &frame_export_);
    frame_export_valid_ = true;
    return frame_export_.byteSize();
}

/**
 * 行内原子的显式占位框（说明见 engine.h 的同名声明）。
 *
 * 为什么画框而不是内容：图片的字节还没进来（二进制资源外置是阶段 6 的事），公式的排版
 * 后端本期不做 —— 把表达式按普通文字画出来会得到错误的字距与上下标，那是伪渲染，
 * 比一个明确的框更危险。框里写上它是什么，读者一眼看得出这里是一枚原子。
 */
void EditorEngine::drawAtomBox(Bitmap& target, const LineBox& lb, const LineCluster& c, float a1_pt,
                               float off_y) {
    const float zoom = cfg_.zoom;
    const float a0 = lb.inline_origin_pt + c.inline_pt;
    // 底边坐在基线上（与字形同一个原点），高度是原子自己的 —— 不是行盒的：一行里混排
    //   大字号文字时行盒比盒子高，拿行盒画框会画出一个不对的矩形。
    const float axis = lb.bracket_axis_pt;
    const float top = axis - c.atom_h_pt;
    Rect box;
    if (!lb.vertical) {
        box = Rect{a0 * zoom, top * zoom + off_y, (a1_pt - a0) * zoom, c.atom_h_pt * zoom};
    } else {
        box = Rect{top * zoom, a0 * zoom + off_y, c.atom_h_pt * zoom, (a1_pt - a0) * zoom};
    }
    if (!(box.w > 0.0f) || !(box.h > 0.0f)) return;

    const rev::Node* n = rev::findNode(document(), static_cast<NodeId>(c.owner_control));
    const ImageSpec* im = (n != nullptr) ? std::get_if<ImageSpec>(&n->payload) : nullptr;
    // 图片的字节不进文档（第十一节），资源表里没有它时退回占位框 —— 这是常规状态
    //   （文件刚打开、宿主还没注册），不是错误。竖排本期仍画占位框：图片在竖排里要不要
    //   跟着转 90 度是版式决定，按横排的盒子直接铺上去会得到一张被拉长的图。
    const ImageResource* res =
        (im != nullptr && !im->resource_id.empty()) ? images_.find(im->resource_id) : nullptr;
    if (c.atom == InlineAtom::Image && res != nullptr && !lb.vertical) {
        target.drawImage(imageResourceKey(res->id), res->version, res->rgba.data(), res->width,
                         res->height, box);
        return;
    }

    target.fillRect(box, cfg_.atom_fill);
    target.strokeRect(box, cfg_.atom_border, 1.0f);

    hb_font_t* hbf = (lb.bracket_font != nullptr) ? lb.bracket_font->hb() : nullptr;
    if (hbf == nullptr) return;
    // 框里写什么：图片写资源标识，公式写表达式；两者都空就写一个说明性的词 ——
    //   空框看不出是哪一种原子，而"看不出"正是这一层要避免的。
    std::string label;
    if (im != nullptr) {
        label = im->resource_id;
    } else if (n != nullptr && c.atom == InlineAtom::Formula) {
        const auto* f = std::get_if<FormulaSpec>(&n->payload);
        if (f != nullptr) label = f->expr;
    }
    if (label.empty()) label = (c.atom == InlineAtom::Formula) ? "公式" : "图片";

    const float pad = cfg_.atom_pad_pt;
    const float avail = (a1_pt - a0) - pad * 2.0f;
    if (!(avail > 0.0f)) return;
    const float units_to_px = zoom / kFontScalePerPt;
    // 框内居中。横排下"居中"指基线：基线 = 框顶 + (框高 + ascent - descent) / 2；
    //   竖排下标签转 90°，居中指的是列内横轴，取框的横向中心（与括号同一条做法）。
    const float asc = lb.bracket_font->ascenderPt();
    const float desc = lb.bracket_font->descenderPt();
    const float centered = top + (c.atom_h_pt + asc - desc) * 0.5f;
    const float cross = lb.vertical ? (top + c.atom_h_pt * 0.5f) : centered;
    const float rot = lb.vertical ? 90.0f : 0.0f;

    std::vector<uint32_t> ids;
    std::vector<float> xs;
    std::vector<float> ys;
    float pen = 0.0f;
    for (uint32_t cp : utf8ToCodepoints(label)) {
        hb_codepoint_t g = 0;
        if (!hb_font_get_nominal_glyph(hbf, cp, &g) || g == 0) continue;  // 缺字跳过这一枚
        const float adv = hb_font_get_glyph_h_advance(hbf, g) / kFontScalePerPt;
        // 放不下就截断：溢出的字会盖到相邻内容上，比少写几个字更糟。
        if (pen + adv > avail) break;
        if (!lb.vertical) {
            xs.push_back((a0 + pad + pen) * zoom);
            ys.push_back(cross * zoom + off_y);
        } else {
            xs.push_back(cross * zoom);
            ys.push_back((a0 + pad + pen) * zoom + off_y);
        }
        ids.push_back(g);
        pen += adv;
    }
    if (!ids.empty()) {
        target.drawGlyphs(hbf, ids.data(), xs.data(), ys.data(), static_cast<int>(ids.size()),
                          units_to_px, cfg_.atom_text, rot);
    }
}

/**
 * 换行符标记（形如 ↵）：一横、左端一枚实心箭头、右端向上折一竖。
 *
 * 用矩形与三角形拼，不借字体字形：这三份字体里都没有 U+21B5，箭头也只有中文字体才有
 * 一字不差的 U+2190 —— 借字形会在缺字的那一行静默消失。尺寸按这一行的字号算，于是
 * 缩放与字号一起变。
 * @param target 目标位图
 * @param along 行末位置（磅，行内轴）
 * @param cross 基线（磅，块轴）
 * @param size_pt 这一行的字号（磅）
 * @param zoom 设备像素 / 磅
 * @param vertical 竖排：整枚标记转 90 度
 * @param color 颜色
 * @param dy_px 整笔的纵向平移（连页接缝补画那半截时是负的带高）
 */
void drawBreakMark(Bitmap& target, float along, float cross, float size_pt, float zoom,
                   bool vertical, Color color, float dy_px) {
    // 尺寸与落点都取整到设备像素：连页接缝那一趟是同一枚标记整体上移一个带高再画一次，
    //   两趟必须落到同一批像素上（判据是逐字节相同）。取整之前算，两趟会差出半像素的
    //   抗锯齿覆盖率 —— 差的正好是带边界那两行。
    const float t = std::round(std::max(1.0f, size_pt * 0.065f * zoom));  // 笔画宽度
    const float w = std::round(size_pt * 0.58f * zoom);                   // 横笔长度
    const float h = std::round(size_pt * 0.28f * zoom);                   // 竖笔高度
    const float head = std::round(size_pt * 0.28f * zoom);                // 箭头长度
    const float half = std::round(size_pt * 0.15f * zoom);                // 箭头半高
    // 平移量在缩放与取整之后加：先除 zoom 再乘回来会引入末位误差，而判据是逐字节比。
    const float a0 = std::round(along * zoom + size_pt * 0.06f * zoom);
    // 横笔抬到字高中段（0.28 em）：贴着基线画会显得沉在字底，与字处理软件里那一枚
    //   标记的位置也不同 —— 它大致落在 x 高度上沿那一带。
    const float bar_lo = std::round(cross * zoom - size_pt * 0.28f * zoom) + dy_px;
    const float right = a0 + w;
    const float mid = bar_lo + t * 0.5f;
    if (!vertical) {
        target.fillRect(Rect{a0, bar_lo, w, t}, color);
        target.fillRect(Rect{right - t, bar_lo - h, t, h + t}, color);
        target.fillTriangle(a0, mid, a0 + head, mid - half - t * 0.3f, a0 + head,
                            mid + half + t * 0.3f, color);
    } else {
        // 竖排：整枚转 90 度 —— 行内轴变 y、块轴变 x。
        target.fillRect(Rect{bar_lo, a0, t, w}, color);
        target.fillRect(Rect{bar_lo - h, right - t, h + t, t}, color);
        target.fillTriangle(mid, a0, mid - half - t * 0.3f, a0 + head, mid + half + t * 0.3f,
                            a0 + head, color);
    }
}

bool EditorEngine::choiceMarkerOf(const PlacedGlyph& pg, bool* checked, bool* square,
                                  Color* color) const {
    if (checked != nullptr) *checked = false;
    if (square != nullptr) *square = false;
    if (color != nullptr) *color = Color{};
    if (pg.paragraph < 0 || pg.paragraph >= static_cast<int>(blocks().size())) return false;
    const std::vector<uint32_t>& text = blocks()[static_cast<size_t>(pg.paragraph)].cp_text;
    if (pg.cluster < 0 || static_cast<size_t>(pg.cluster) >= text.size()) return false;
    const uint32_t cp = text[static_cast<size_t>(pg.cluster)];
    const bool box = (cp == kCheckboxMarkOff || cp == kCheckboxMarkOn);
    const bool dot = (cp == kRadioMarkOff || cp == kRadioMarkOn);
    if (!box && !dot) return false;

    // 落在哪个控件上：控件集合的区间是按段给码点偏移的，问一次就知道。
    for (size_t i = 0; i < control_node_ids_.size(); ++i) {
        const Control& c = controls().at(static_cast<int>(i));
        if (c.range.start.para != pg.paragraph) continue;
        const int off = pg.cluster - static_cast<int>(c.range.start.offset);
        if (off < 0) continue;
        const rev::Node* n = rev::findNode(document(), control_node_ids_[i]);
        if (n == nullptr) continue;
        const auto* spec = std::get_if<ControlSpec>(&n->payload);
        if (spec == nullptr) continue;
        bool on = false;
        if (!controlMarkerAt(*spec, off, &on)) continue;
        if (checked != nullptr) *checked = on;
        if (square != nullptr) *square = box;
        if (color != nullptr) *color = spec->appearance.glyph.color;
        return true;
    }
    return false;
}

void EditorEngine::paintChoiceMarker(Bitmap& target, const PlacedGlyph& pg, bool checked,
                                     bool square, Color color, float dy) const {
    const float zoom = cfg_.zoom;
    if (!(zoom > 0.0f)) return;
    // 记号色没配（全透明）就跟随文字：记号是内容的一部分，正文什么颜色它就什么颜色。
    if (color.a == 0) color = pg.color;

    const float pen_x = pg.x_pt * zoom;
    const float pen_y = pg.y_pt * zoom + dy;

    // 记号的外径统一按字号定：方框与圆圈共用同一个数，两族图标的视觉大小才对得上。
    //   圆圈若借字体里的字形，它的直径就取决于字体 —— 同一个字号下与方框并排放着
    //   一个偏小一个偏大，一眼就看出来。
    const float size_px = pg.size_pt * zoom;
    const float adv_px = (pg.advance_pt > 0.0f ? pg.advance_pt : pg.size_pt) * zoom;
    if (!(size_px > 1.0f)) return;
    const float side = std::max(5.0f, std::round(size_px * 0.72f));
    const float thick = std::max(1.0f, std::round(size_px * 0.075f));
    // 格宽决定左右居中；底边坐在基线上，与字体里那些方框字一致。
    const float cx = pen_x + adv_px * 0.5f;
    const float cy = pen_y - side * 0.5f;

    if (!square) {
        // 圆圈：圆环 + 选中时圆心一个实心点。半径收到"外沿正好等于 side/2"，
        //   于是方框与圆圈的外接框一样大，切换时看着不跳。
        const float radius = std::max(1.0f, side * 0.5f - thick * 0.5f);
        target.strokeCircle(cx, cy, radius, color, thick);
        if (!checked) return;
        target.fillCircle(cx, cy, std::max(1.0f, radius - thick * 1.6f), color);
        return;
    }

    // 方框：四条边由整数矩形拼出来，边长与线宽都取整 —— 右 / 下边才不会比左 / 上边多占一像素。
    const float left = std::round(cx - side * 0.5f);
    const float top = std::round(cy - side * 0.5f);
    target.strokeRect(Rect{left, top, side, side}, color, thick);
    if (!checked) return;

    // 对勾：一条折线，圆头圆角，覆盖率按距离算 —— 与方框同为三像素宽的笔画，
    //   斜线上不会出现锯齿台阶。比例按框边长算，字号变了跟着缩。
    const float mark[6] = {
        left + side * 0.26f, top + side * 0.52f,  left + side * 0.43f,
        top + side * 0.70f,  left + side * 0.76f, top + side * 0.30f,
    };
    const float w = std::max(1.0f, std::round(side * 0.14f));
    target.strokePolyline(mark, 3, color, w);
}

/**
 * 不可见字符标记（页内坐标，画进页位图）：段落标记、空格的中点、制表符的箭头。
 *
 * 只画进位图、不占宽度 —— 标记是版面之外的一层注释，断行、命中、光标落点都不认它，
 * 所以开关它只需要重光栅（签名里混了档位与颜色，见 rasterSignature）。
 *
 * 几何全部从行盒派生：段落标记坐在"这一段的最后一行"的行末（段内码点数 == 行盒的
 * last_cp），空格的中点坐在该簇的 inline_pt 与下一簇的 inline_pt 之间。字体与基线取
 * 行盒的 bracket_font / bracket_axis_pt —— 那是块主字体，标记的粗细与风格才跟正文一致。
 */
void EditorEngine::rasterWatermark(Bitmap& target, int page_index, const float* band_lo_pt,
                                   const float* band_hi_pt) {
    (void)page_index;
    const WatermarkSpec w = watermark();
    if (!w.enabled || w.text.empty()) return;
    const std::vector<uint32_t> cps = utf8ToCodepoints(w.text);
    if (cps.empty()) return;

    // 挑一张画得出第一个字的脸：引擎不知道"哪一号是中文"，只能试。
    //   水印是一条辅助文字，选错字体的代价是缺字，不会影响正文。
    FontInstance* face = nullptr;
    for (int i = 0; i < fonts_.faceCount(); ++i) {
        FontInstance* f = fonts_.instance(i, w.size_pt, kWeightRegular);
        if (f == nullptr) continue;
        hb_codepoint_t g = 0;
        if (hb_font_get_nominal_glyph(f->hb(), cps.front(), &g) && g != 0) {
            face = f;
            break;
        }
    }
    if (face == nullptr) return;

    hb_font_t* hb = face->hb();
    std::vector<uint32_t> gids;
    gids.reserve(cps.size());
    std::vector<float> advs;
    advs.reserve(cps.size());
    float total = 0.0f;
    for (const uint32_t cp : cps) {
        hb_codepoint_t g = 0;
        if (!hb_font_get_nominal_glyph(hb, cp, &g)) g = 0;
        const float a = static_cast<float>(hb_font_get_glyph_h_advance(hb, g)) / kFontScalePerPt;
        gids.push_back(static_cast<uint32_t>(g));
        advs.push_back(a);
        total += a;
    }
    if (!(total > 0.0f)) return;

    const float zoom = cfg_.zoom;
    const float units_to_px = zoom / kFontScalePerPt;
    // 落在整页的中心，不是正文区的中心：Word 的水印以纸面为参照，页边距改了水印不跟着挪。
    const float cx = cfg_.page.width_pt * 0.5f;
    const float cy = cfg_.page.height_pt * 0.5f;
    const float rad = w.angle_deg * 3.14159265f / 180.0f;
    const float dx = std::cos(rad);
    const float dy = std::sin(rad);
    // 基线沿法线让开一点：字形坐在基线上，不让开的话整行会偏到中心之上。
    const float nx = -dy;
    const float ny = dx;
    const float off = w.size_pt * 0.36f;

    float s = -total * 0.5f;
    for (size_t i = 0; i < gids.size(); ++i) {
        const float px = cx + dx * s + nx * off;
        const float py = cy + dy * s + ny * off;
        s += advs[i];
        if (gids[i] == 0) continue;
        // 分带重画：只画这一带里的字（上下各留一个字身，免得半个字被裁掉）。
        if (band_lo_pt != nullptr && band_hi_pt != nullptr &&
            (py < *band_lo_pt - w.size_pt || py > *band_hi_pt + w.size_pt)) {
            continue;
        }
        const uint32_t one = gids[i];
        const float x = px * zoom;
        const float y = py * zoom;
        target.drawGlyphs(hb, &one, &x, &y, 1, units_to_px, w.color, w.angle_deg);
    }
}

void EditorEngine::rasterEditingMarks(Bitmap& target, int page_index, const float* band_lo_pt,
                                      const float* band_hi_pt, float dy_px, bool spill_only) {
    // 档位与只读态的合成判断（只读是呈现态，标记会被印出来，与表单括号同一条）。
    if (!marksShown()) return;
    const MarksMode mode = cfg_.marks;

    const float zoom = cfg_.zoom;
    const float units_to_px = zoom / kFontScalePerPt;
    const float slab_pt = cfg_.page.slabHeightPt();
    const bool band = band_lo_pt && band_hi_pt;
    const std::vector<BlockSpec>& bs = blocks();
    const bool dot_mode = (mode == MarksMode::All);

    const std::vector<LineBox>& lines = layout_.lines;
    for (int k : layout_.linesOnPage(page_index)) {
        const LineBox& lb = lines[static_cast<size_t>(k)];
        if (!lb.bracket_font) continue;
        if (spill_only && !(lb.block_hi_pt > slab_pt)) continue;
        const float lo_pt = lb.block_lo_pt + dy_px / zoom;
        const float hi_pt = lb.block_hi_pt + dy_px / zoom;
        if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;

        hb_font_t* hbf = lb.bracket_font->hb();
        const float x0 = lb.inline_origin_pt;
        const float x1 = lb.inline_origin_pt + lb.inline_end_pt;
        const float axis = lb.bracket_axis_pt;
        if (lb.paragraph < 0 || lb.paragraph >= static_cast<int>(bs.size())) continue;
        const std::vector<uint32_t>& text = bs[static_cast<size_t>(lb.paragraph)].cp_text;

        // 空格与制表符：每个簇各画一枚，位置取簇的起点、宽度取到下一簇的起点。
        //   簇的 cp 是段内码点下标（不是码点本身），要拿它回块流里查那一个字。
        if (dot_mode) {
            // 中点的纵向位置按这一行的字号算：太小看不见，太低就跟正文混在一起。
            const float mid = axis - lb.size_pt * 0.30f;
            for (size_t ci = 0; ci < lb.clusters.size(); ++ci) {
                const LineCluster& c = lb.clusters[ci];
                if (c.placeholder || c.atom != InlineAtom::None) continue;
                if (c.cp >= static_cast<uint32_t>(text.size())) continue;
                const float a0 = x0 + c.inline_pt;
                const float a1 =
                    (ci + 1 < lb.clusters.size()) ? x0 + lb.clusters[ci + 1].inline_pt : x1;
                if (!(a1 > a0)) continue;
                const uint32_t ch = text[c.cp];
                if (ch == 0x20 || ch == 0x3000) {
                    const float cx = (a0 + a1) * 0.5f;
                    // 半角空格画中点，全角空格画空心小方块：两者的占位差四倍，同一个点
                    //   放在全角格里会显得那一格是空的（与字处理软件的画法也一致）。
                    const bool full = (ch == 0x3000);
                    const float s0 = full ? std::max(3.0f, lb.size_pt * 0.42f * zoom) : 0.0f;
                    // 半角空格的格宽只有 0.22 em：点若按字号取（0.09 em）就占了四成，
                    //   再经 fillRect 的 floor/ceil 向外取整，1 px 的框会撑成 2 px，
                    //   连续空格于是挤成一条线。按格宽的三成取，并吸附到整像素。
                    const float dot_px = std::max(1.0f, std::round((a1 - a0) * 0.30f * zoom));
                    if (!lb.vertical) {
                        if (full) {
                            const float x0 = cx * zoom - s0 * 0.5f;
                            const float y0 = mid * zoom + dy_px - s0 * 0.5f;
                            const float st = std::max(1.0f, s0 * 0.12f);
                            target.fillRect(Rect{x0, y0, s0, st}, cfg_.mark_color);
                            target.fillRect(Rect{x0, y0 + s0 - st, s0, st}, cfg_.mark_color);
                            target.fillRect(Rect{x0, y0, st, s0}, cfg_.mark_color);
                            target.fillRect(Rect{x0 + s0 - st, y0, st, s0}, cfg_.mark_color);
                        } else {
                            target.fillRect(Rect{std::round(cx * zoom - dot_px * 0.5f),
                                                 std::round(mid * zoom + dy_px), dot_px, dot_px},
                                            cfg_.mark_color);
                        }
                    } else {
                        // 竖排：行内轴是 y、块轴是 x，中点跟着换轴（方块不必转）。
                        target.fillRect(
                            Rect{std::round(mid * zoom),
                                 std::round(cx * zoom + dy_px - dot_px * 0.5f), dot_px, dot_px},
                            cfg_.mark_color);
                    }
                } else if (ch == 0x09) {
                    // 制表符用右箭头字形；字体里没有这一枚就跳过，不画半个箭头。
                    hb_codepoint_t g = 0;
                    if (!hb_font_get_nominal_glyph(hbf, 0x2192, &g) || !g) continue;
                    if (!lb.vertical) {
                        target.drawGlyph(hbf, g, a0 * zoom, axis * zoom + dy_px, units_to_px,
                                         cfg_.mark_color, 0.0f);
                    } else {
                        target.drawGlyph(hbf, g, axis * zoom, a0 * zoom + dy_px, units_to_px,
                                         cfg_.mark_color, 90.0f);
                    }
                }
            }
        }

        // 段落标记只画在这一段的最后一行：段内码点数 == 行盒的 last_cp。
        //   空段（码点 0 个）的最后一行就是它唯一那一行，标记落在行首。
        if (lb.last_cp != static_cast<uint32_t>(text.size())) continue;
        drawBreakMark(target, x1, axis, lb.size_pt, zoom, lb.vertical, cfg_.mark_color, dy_px);
    }
}

/**
 * 表单标记（页内坐标，画进页位图）：方括号常显（开关关掉则一个都不显示）；小点是独立
 * 开关，开时所有控件都画，与焦点无关。
 * 括号用字体里的字形而非矩形拼几何：真字形与正文同粗细、同风格，随字号缩放；无法纵向
 * 拉伸，所以跨行时每行各括一次。
 *
 * 几何从行盒派生：位置取占位簇的 inline_pt、朝向取 bracket_side、字体与基线取行盒的
 * bracket_font 与 bracket_axis_pt —— 要画与要占位共用同一份数据，槽在就画得出。
 *
 * 画在页位图里而不是帧上：每一次变化都伴随重排或光栅签名变化（见 docs/步5-渲染层方案.md
 * 的 5D 小节），不需要"矩形出口"；跟着内容缓存反而少一份逐帧擦除的账。
 */
void EditorEngine::rasterControlMarks(Bitmap& target, int page_index, const float* band_lo_pt,
                                      const float* band_hi_pt, float dy_px, bool spill_only) {
    // 只读不画：它是呈现与打印态，括号落进版面就印出来了。
    if (mode_ == EditMode::ReadOnly) return;

    const float zoom = cfg_.zoom;
    const float units_to_px = zoom / kFontScalePerPt;
    const float dot = std::max(1.0f, cfg_.control_dot_pt * zoom);
    const bool band = band_lo_pt && band_hi_pt;
    // 接缝那一趟：只画越过带底的那一截（见 rasterIntoBitmap），坐标整体上移一个带高。
    const float slab_pt = cfg_.page.slabHeightPt();

    // 括号：只过这一页的行（见 LayoutResult::page_lines），带外的行跳过。
    //   与原子框那段同一个走法：一条带的边界就是行的块轴范围。
    const std::vector<LineBox>& lines = layout_.lines;
    for (int k : layout_.linesOnPage(page_index)) {
        const LineBox& lb = lines[static_cast<size_t>(k)];
        if (!lb.bracket_font) continue;
        if (spill_only && !(lb.block_hi_pt > slab_pt)) continue;
        const float lo_pt = lb.block_lo_pt + dy_px / zoom;
        const float hi_pt = lb.block_hi_pt + dy_px / zoom;
        if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;

        for (const LineCluster& c : lb.clusters) {
            if (!c.placeholder) continue;
            // 占位簇不止括号一种：行内原子也占一个槽（它不产生字形，所以同样是占位）。
            //   这里必须显式把 None 挡掉，不能写成"不是闭括号就是开括号"：原子的
            //   bracket_side 正是 None，那样写会给每枚原子左边长出一枚开括号，
            //   而它用的是括号色、贴在框的左沿上，看着像原子自带的装饰。
            if (c.bracket_side == BracketSide::None) continue;
            // 括号跟着内容 run 的字体走：排版的槽宽按同一份字体算，绘制必须用同一个，
            //   否则字形的 advance 与槽宽对不上，括号会压字或留一条空缝。
            hb_font_t* hbf = (c.bracket_font != nullptr ? c.bracket_font : lb.bracket_font)->hb();
            const bool open = (c.bracket_side == BracketSide::Open);
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
                target.drawGlyph(hbf, g, along_pt * zoom, lb.bracket_axis_pt * zoom + dy_px,
                                 units_to_px, col, 0.0f);
            } else {
                // 竖排：括号跟着转 90°（顺时针）。字形轮廓本身是横排形态，
                //   [ 转过来开口朝下 —— 正好是"内容继续往下排"的意思。
                //   不转的话括号会横躺在列里，开口方向与行内轴垂直（错的）。
                //   旋转后 advance 沿 +y，所以位置分量是 (列内横轴笔位, 行内轴)。
                target.drawGlyph(hbf, g, lb.bracket_axis_pt * zoom, along_pt * zoom + dy_px,
                                 units_to_px, col, 90.0f);
            }
        }
    }

    // 提示文字：空控件里那句"该填什么"。几何取占位簇的位置与宽度（槽宽就是它排出来的
    //   那一段），字体与基线取括号那一套。它替内容占位，所以用灰色 —— 与正文同色会被
    //   当成已经填好的字读走。
    //   只读态上面已经返回了：提示是编辑期的辅助，印在纸上就是凭空多出一句话。
    for (int k : layout_.linesOnPage(page_index)) {
        const LineBox& lb = lines[static_cast<size_t>(k)];
        if (!lb.bracket_font) continue;
        if (spill_only && !(lb.block_hi_pt > slab_pt)) continue;
        const float lo_pt = lb.block_lo_pt + dy_px / zoom;
        const float hi_pt = lb.block_hi_pt + dy_px / zoom;
        if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;

        for (size_t ci = 0; ci < lb.clusters.size(); ++ci) {
            const LineCluster& c = lb.clusters[ci];
            if (!c.control_hint) continue;
            const rev::Node* n = rev::findNode(document(), static_cast<NodeId>(c.owner_control));
            const auto* spec = (n != nullptr) ? std::get_if<ControlSpec>(&n->payload) : nullptr;
            if (spec == nullptr || spec->appearance.placeholder.empty()) continue;
            const FontInstance* face =
                (c.bracket_font != nullptr) ? c.bracket_font : lb.bracket_font;
            hb_font_t* hbf = (face != nullptr) ? face->hb() : nullptr;
            if (hbf == nullptr) continue;

            const float a0 = lb.inline_origin_pt + c.inline_pt;
            const float a1 = (ci + 1 < lb.clusters.size())
                                 ? lb.inline_origin_pt + lb.clusters[ci + 1].inline_pt
                                 : lb.inline_origin_pt + lb.inline_end_pt;
            // 宽度上限就是槽宽：排版按它截过一次，这里按同一个上限截第二次 ——
            //   换成另一个上限，截断点就会落在槽外或槽里空一段（见 hintAdvancePt）。
            const float avail = a1 - a0;
            if (!(avail > 0.0f)) continue;

            std::vector<uint32_t> ids;
            std::vector<float> xs;
            std::vector<float> ys;
            float pen = 0.0f;
            for (uint32_t cp : utf8ToCodepoints(spec->appearance.placeholder)) {
                hb_codepoint_t g = 0;
                if (!hb_font_get_nominal_glyph(hbf, cp, &g) || g == 0) continue;  // 缺字跳过
                const float adv = hb_font_get_glyph_h_advance(hbf, g) / kFontScalePerPt;
                if (pen + adv > avail + 0.01f) break;
                if (!lb.vertical) {
                    xs.push_back((a0 + pen) * zoom);
                    ys.push_back(lb.bracket_axis_pt * zoom + dy_px);
                } else {
                    // 竖排：与括号同一套做法，整笔转 90 度，沿列往下推进。
                    xs.push_back(lb.bracket_axis_pt * zoom);
                    ys.push_back((a0 + pen) * zoom + dy_px);
                }
                ids.push_back(g);
                pen += adv;
            }
            if (ids.empty()) continue;
            target.drawGlyphs(hbf, ids.data(), xs.data(), ys.data(), static_cast<int>(ids.size()),
                              units_to_px, cfg_.control_hint, lb.vertical ? 90.0f : 0.0f);
        }
    }

    // 小点是独立开关，与括号无关：关着就到这儿结束。
    if (!cfg_.show_control_dots) return;

    // 开启时所有控件都画 —— 不区分焦点。它只表示"这里有一个字段"，
    //   不表示"光标不在这里"，所以这里既没有 innermostAt，也没有"跳过聚焦的那个"。
    for (int ci = 0; ci < controls_.size(); ++ci) {
        const Control& c = controls_.at(ci);
        const Color col = c.read_only ? cfg_.control_mark_ro : cfg_.control_mark;
        for (const ControlRect& cr : controlRects(ci)) {
            if (cr.page != page_index) continue;
            if (spill_only && !(cr.r_pt.y + cr.r_pt.h > slab_pt)) continue;
            const float lo_pt = cr.r_pt.y + dy_px / zoom;
            const float hi_pt = cr.r_pt.y + cr.r_pt.h + dy_px / zoom;
            if (band && (hi_pt < *band_lo_pt - 1.0f || lo_pt > *band_hi_pt + 1.0f)) continue;
            // 小点：右下角、内缩一点
            const float x1 = (cr.r_pt.x + cr.r_pt.w) * zoom;
            const float y1 = (cr.r_pt.y + cr.r_pt.h) * zoom + dy_px;
            target.fillRect(Rect{x1 - dot, y1 - dot, dot, dot}, col);
        }
    }
}

void EditorEngine::repaintAfterEdit() {
    // 编辑之后光标必须立即可见。这不仅是观感，也是宿主记账的依赖：
    //   editStats.caretVisible 为 false 时宿主会再 present 一次，并把上屏矩形换成
    //   那一轮的，于是编辑那次的全帧重绘被丢掉（auditFrame 差 0、selfCheck 差上千
    //   像素，画布停在旧排版上）。
    //   该不变量属于引擎：放在宿主里就成了补救路径，而补救路径本身会引入记账歧义。
    //   顺带重置闪烁相位，符合输入法/编辑器的常见手感。
    caret_visible_ = true;
    // 编辑之后尺寸预览作废：文档已经按提交后的样子重排，再留一个旧框只会画错位置。
    clearImageResizePreview();
    repaint();
}

}  // namespace heditor
