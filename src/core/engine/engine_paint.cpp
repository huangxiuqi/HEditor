/**
 * 呈现：保证窗口内每页的内容新鲜，记下这一拍要上屏的矩形，并按需导出像素。
 *
 * 渲染只写页位图（内容），光标与选区是导出时才叠上去的瞬态。于是"局部重绘"
 *   退化成"页位图变了哪一块"，宿主按页上的矩形贴：没有第二份像素要跟着同步。
 * 表单标记（方括号、小点）画在页位图里，与正文共用同一份行盒几何 —— 见 page_marks.h 的 drawControlMarks。
 */

#include "core/engine/engine.h"

#include "core/engine/page_marks.h"
#include "core/engine/page_raster.h"

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

int EditorEngine::pagePxWidth() const { return pagePxWidthOf(cfg_); }

int EditorEngine::pagePxHeight() const { return pagePxHeightOf(cfg_); }

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
    // 表格片段摘要与行指纹在同一处快照：computeBand 拿两边的新旧值比，
    //   两者错开一轮时，切过页的那一页会被判成"只动了一行"。
    table_fp_prev_ = table_fp_new_;
    // 浮动图摘要同理：它与表格一样"不是行盒画出来的东西"，快照必须在同一处。
    float_fp_prev_ = float_fp_new_;
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
    // 页号超出指纹数组时不敢断言"作数"：那种页没有可供比较的版本号。
    if (page < 0 || page >= static_cast<int>(page_fp_new_.size())) return false;
    return pages_.fresh(page, page_fp_new_[static_cast<size_t>(page)], pagePxWidth(),
                        pagePxHeight());
}

RenderInput EditorEngine::renderInput() {
    RenderInput in;
    in.doc = &document();
    in.layout = &layout_;
    in.blocks = &blocks();
    in.control_node_ids = &control_node_ids_;
    in.images = &images_;
    in.controls = &controls_;
    in.fonts = &fonts_;
    in.cfg = &cfg_;
    in.mode = mode_;
    in.watermark = watermark();
    in.marks_shown = marksShown();
    in.control_geometry = &control_geometry_;
    in.float_preview = floatPreview();
    return in;
}

void EditorEngine::rasterPageInto(int page, const float* band_lo_pt, const float* band_hi_pt) {
    PageSlot& pb = pages_.at(page);
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
    PageRasterCost cost;
    drawPage(pb.px, renderInput(), page, band_lo_pt, band_hi_pt, &cost);
    raster_fill_ms_ += cost.fill_ms;
    raster_glyph_ms_ += cost.glyph_ms;
    // 记的是"这一份像素对应哪一版指纹"。哨兵值也算一版：markPagesChangedFrom
    //   每次都会按已画那一版翻一下低位，同一个哨兵不会连着出现两次。
    pb.fp = (page < static_cast<int>(page_fp_new_.size())) ? page_fp_new_[static_cast<size_t>(page)]
                                                           : 0;
    pb.raster_sig = pages_.signature();
    pb.valid = true;
    raster_glyphs_ += static_cast<int>(pb.px.paintedGlyphs());
    raster_glyph_bounds_ms_ += pb.px.glyphBoundsMs();
}

void EditorEngine::trimPageCache(int first_page, int count) { pages_.trim(first_page, count); }

/**
 * 记一块"这一页的像素变了"。页内像素坐标，且是累积的（自上次取走以来）。
 *
 * 合并规则：相交或相接的并成一个（光标擦旧画新就是这种），相距远的不并 ——
 *   并了会把中间一大片没变的像素也传上去。条数超过 16 时退回一个并集：
 *   一页几十个表单标记时逐条传反而更贵，而这一条只是"上界"，宁可多传不可少传。
 */
void EditorEngine::notePageDirty(int page, const Rect& r) { pages_.noteDirty(page, r); }

std::vector<Rect> EditorEngine::takePageDirtyRects(int page) { return pages_.takeDirty(page); }

/**
 * 有累积脏矩形的页号（升序）。与 takePageDirtyRects 的分工见头文件。
 */
std::string EditorEngine::pageDirtyPagesJson() const {
    const std::vector<int> pages = pages_.dirtyPages();
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
    const PageSlot* pb = pages_.find(page);
    return (pb == nullptr) ? 0 : pb->px.byteSize();
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
    pages_.setSignature(rasterSignatureNow());
    // 光栅那一组由 drawPage 累加，末尾一次性上报。
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
        const int new_pages = static_cast<int>(page_fp_new_.size());
        const int prev_pages = static_cast<int>(page_fp_prev_.size());
        const int n = std::max(new_pages, prev_pages);
        for (int i = 0; i < n; ++i) {
            const uint64_t a = (i < new_pages) ? page_fp_new_[static_cast<size_t>(i)] : 0;
            const uint64_t b = (i < prev_pages) ? page_fp_prev_[static_cast<size_t>(i)] : 0;
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
        if (page < static_cast<int>(table_fp_prev_.size()) &&
            page < static_cast<int>(table_fp_new_.size()) &&
            table_fp_new_[static_cast<size_t>(page)] != table_fp_prev_[static_cast<size_t>(page)]) {
            return 0;
        }
        // 浮动图同理，而且更硬：它整块落在行带之外（一张图可能比一条带还高），
        //   行带只盖住文字那一条，图那一块永远补不回来。
        if (page < static_cast<int>(float_fp_prev_.size()) &&
            page < static_cast<int>(float_fp_new_.size()) &&
            float_fp_new_[static_cast<size_t>(page)] != float_fp_prev_[static_cast<size_t>(page)]) {
            return 0;
        }
        int d0 = -1;
        for (int k : page_line_ids) {
            const bool same =
                k < static_cast<int>(line_fp_prev_.size()) &&
                line_fp_new_[static_cast<size_t>(k)] == line_fp_prev_[static_cast<size_t>(k)];
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
            const bool same =
                k < static_cast<int>(line_fp_prev_.size()) &&
                line_fp_new_[static_cast<size_t>(k)] == line_fp_prev_[static_cast<size_t>(k)];
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
        if (line_fp_prev_.size() > layout_.lines.size()) hi = static_cast<float>(page_px_h) / zoom;
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
        const PageSlot* prev_slot = pages_.find(page);
        const bool has_prev = prev_slot != nullptr && prev_slot->valid &&
                              prev_slot->raster_sig == pages_.signature() &&
                              prev_slot->px.width() == page_px_w &&
                              prev_slot->px.height() == page_px_h;
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
        PageSlot* slot = pages_.find(page);
        if (slot == nullptr) continue;
        PageTransient now;
        collectTransient(page, &now);
        const PageTransient& prev = slot->transient;
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
        slot->transient = now;
    }
    // 窗口外的页不留瞬态账：它记的是"上一拍这一页叠了什么"，而那一页已经不在画布上。
    //   留着的话，读 pageRgba(窗口外的页) 会得到一个过期光标 —— 探针正是那么读的。
    for (auto& kv : pages_.slots()) {
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
    const PageSlot* pb = pages_.find(page);
    if (pb == nullptr || !pb->valid) return nullptr;
    if (pb->px.width() != pagePxWidth() || pb->px.height() != pagePxHeight()) return nullptr;
    if (pb->transient.sel.empty() && pb->transient.image_ui.empty() &&
        !(pb->transient.caret.w > 0.0f)) {
        return pb->px.data();
    }

    const int pw = pb->px.width();
    const int ph = pb->px.height();
    if (page_export_.width() != pw || page_export_.height() != ph) {
        page_export_.reset(pw, ph);
        page_export_.shareFonts(font_owner_);
    }
    page_export_.blitFrom(pb->px, Rect{0.0f, 0.0f, static_cast<float>(pw), static_cast<float>(ph)},
                          0, 0);
    for (const Rect& r : pb->transient.sel) page_export_.blendRect(r, cfg_.selection_color);
    // 拖拽框与把手画在选区之上、光标之下：它们比选区更需要看得清，而光标是最后一条。
    for (const Rect& r : pb->transient.image_ui) page_export_.fillRect(r, cfg_.image_mark);
    if (pb->transient.caret.w > 0.0f) page_export_.fillRect(pb->transient.caret, cfg_.caret_color);
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
            const PageSlot* slot = pages_.find(page);
            if (slot != nullptr && slot->valid) src = slot->px.data();
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
