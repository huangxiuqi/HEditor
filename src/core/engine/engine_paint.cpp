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
#include "core/engine/page_scene.h"
#include "core/render/page_recorder.h"

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
 * 覆盖场景的值指纹：层序固定，矩形按位混入。
 * @param s 覆盖场景
 * @returns 指纹
 */
uint64_t overlayRoot(const OverlayScene& s) {
    uint64_t h = kFnvBasis;
    for (const OverlayItem& it : s.items) {
        h = fnvMix(h, static_cast<uint64_t>(it.layer));
        h = fnvMixFloat(h, it.rect.x);
        h = fnvMixFloat(h, it.rect.y);
        h = fnvMixFloat(h, it.rect.w);
        h = fnvMixFloat(h, it.rect.h);
    }
    return h;
}

/**
 * 覆盖场景里有没有一层上的同一个矩形。
 * @param s 覆盖场景
 * @param layer 层
 * @param r 矩形
 * @returns 有没有
 */
bool overlayHas(const OverlayScene& s, OverlayLayer layer, const Rect& r) {
    for (const OverlayItem& it : s.items) {
        if (it.layer != layer) continue;
        if (it.rect.x == r.x && it.rect.y == r.y && it.rect.w == r.w && it.rect.h == r.h)
            return true;
    }
    return false;
}

}  // namespace

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
    return std::max(0, static_cast<int>(std::lround(cfg_.page_gap_pt * cfg_.density)));
}

void EditorEngine::paintPageRange(int first_page, int count) { presentInternal(first_page, count); }

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
    if (layout_service_.layout.glyphs.empty() || page < 0) return 0;
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

    const float density = cfg_.density;
    const float units_to_px = density / kFontScalePerPt;
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
    for (int k : layout_service_.layout.glyphsOnPage(page)) {
        const PlacedGlyph& pg = layout_service_.layout.glyphs[static_cast<size_t>(k)];
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
        const float y = pg.y_pt * density;
        ids.push_back(pg.glyph_id);
        xs.push_back(pg.x_pt * density);
        ys.push_back(y - std::floor(y) + kPrewarmYOffsetPx);
        ++n;
    }
    flush();
    return n;
}

bool EditorEngine::pageBitmapUsable(int page) const {
    return render_.pages.usable(page, pagePxWidth(), pagePxHeight());
}

RenderInput EditorEngine::renderInput() {
    RenderInput in;
    in.doc = &document();
    in.layout = &layout_service_.layout;
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

void EditorEngine::rasterPageInto(int page, const float* band_lo_pt, const float* band_hi_pt,
                                  PageScene* scene) {
    PageSlot& pb = render_.pages.at(page);
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
    if (band_lo_pt == nullptr && band_hi_pt == nullptr) {
        // 整页：先只记录（不碰像素），再按场景重放。同一份 drawPage 既定义了几何，
        //   也定义了重放顺序；记录与重放分两步，绘制只有一份实现。
        if (scene != nullptr) scene->ops.clear();
        PageScene local;
        PageScene* target = (scene != nullptr) ? scene : &local;
        RecordingSink recorder(target, pb.px.width(), pb.px.height(), nullptr);
        drawPage(recorder, renderInput(), page, nullptr, nullptr, nullptr);
        rasterPageScene(pb.px, *target, &cost);
    } else {
        // 分带：按这一带记录，指令只覆盖带内；重放的是带内指令，带外像素不动。
        //   记录进临时场景，不写回 PageSlot —— 带内指令不能当作整页指令复用。
        PageScene band_scene;
        RecordingSink recorder(&band_scene, pb.px.width(), pb.px.height(), nullptr);
        drawPage(recorder, renderInput(), page, band_lo_pt, band_hi_pt, nullptr);
        rasterPageScene(pb.px, band_scene, &cost);
    }
    raster_fill_ms_ += cost.fill_ms;
    raster_glyph_ms_ += cost.glyph_ms;
    // 场景由 present 在光栅之后写回；这里只记光栅配置版本，供查询类入口判断可读性。
    pb.raster_sig = render_.pages.signature();
    pb.valid = true;
    raster_glyphs_ += static_cast<int>(pb.px.paintedGlyphs());
    raster_glyph_bounds_ms_ += pb.px.glyphBoundsMs();
}

void EditorEngine::trimPageCache(int first_page, int count) {
    render_.pages.trim(first_page, count);
}

/**
 * 记一块"这一页的像素变了"。页内像素坐标，且是累积的（自上次取走以来）。
 *
 * 合并规则：相交或相接的并成一个（光标擦旧画新就是这种），相距远的不并 ——
 *   并了会把中间一大片没变的像素也传上去。条数超过 16 时退回一个并集：
 *   一页几十个表单标记时逐条传反而更贵，而这一条只是"上界"，宁可多传不可少传。
 */
void EditorEngine::notePageDirty(int page, const Rect& r) { render_.pages.noteDirty(page, r); }

std::vector<Rect> EditorEngine::takePendingRects(int page) { return render_.pages.takeDirty(page); }

/**
 * 有累积脏矩形的页号（升序）。与 takePendingRects 的分工见头文件。
 */
std::string EditorEngine::pagesPendingBlitJson() const {
    const std::vector<int> pages = render_.pages.pagesWrittenThisFrame();
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < pages.size(); ++i) {
        if (i != 0) o << ",";
        o << pages[i];
    }
    o << "]";
    return o.str();
}

std::string EditorEngine::takePendingRectsJson(int page) {
    const std::vector<Rect> rs = takePendingRects(page);
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
    if (!pageBitmapUsable(page)) return 0;
    const PageSlot* pb = render_.pages.find(page);
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
    // 光栅签名在这里按当前状态重算一次：它只管缩放、页度量这些所有页共同的参数。
    //   内容是否变了由 PageScene 的根判断；两者分开之后，新增一种画法只需要在场景里
    //   加一项，不必再往这里的清单上补一项。
    render_.pages.setSignature(rasterSignatureNow());
    // 光栅那一组由 drawPage 累加，末尾一次性上报。
    raster_glyphs_ = 0;
    raster_glyph_bounds_ms_ = 0.0;
    raster_fill_ms_ = 0.0;
    raster_glyph_ms_ = 0.0;

    const auto t_pre = Clock::now();
    const float density = cfg_.density;
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

    // 场景在这里按页构建；新旧两版一比就知道哪几项变了、脏矩形在哪。
    //   旧实现用页/行/表/浮动图四组指纹分别回答“哪里变了”，现在只剩一份场景。
    edit_stats_.dirty_page_count = 0;
    edit_stats_.scene_built_pages = 0;
    edit_stats_.scene_reused_pages = 0;
    const auto pageRectPt = [&]() {
        return Rect{0.0f, 0.0f, cfg_.page.width_pt, cfg_.page.height_pt};
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

    // 一、内容：按场景判断这一页画出来变了没有；变了就重光栅，脏矩形由场景 diff 给出。
    const RenderInput render_in = renderInput();
    const Rect page_bounds = pageRectPt();
    // 场景输入键：这一拍没有重排，且这些输入都没变，场景本身就不必重建。
    //   光标闪烁走的是这条路 —— 它只改覆盖层，不该重新哈希整页的字形。
    uint64_t scene_key = render_.pages.signature();
    scene_key = fnvMix(scene_key, static_cast<uint64_t>(mode_));
    for (int ci = 0; ci < controls_.size(); ++ci) {
        scene_key = fnvMix(scene_key, controls_.at(ci).read_only ? 1ull : 0ull);
    }
    scene_key = fnvMix(scene_key, static_cast<uint64_t>(render_in.float_preview.id));
    scene_key = fnvMixFloat(scene_key, render_in.float_preview.dx_pt);
    scene_key = fnvMixFloat(scene_key, render_in.float_preview.dy_pt);
    scene_key = fnvMix(scene_key, render_in.float_preview.placed ? 1ull : 0ull);

    int rastered = 0;
    double scene_ms = 0.0;
    double diff_ms = 0.0;
    for (int i = 0; i < count; ++i) {
        const int page = first_page + i;
        const PageSlot* prev_slot = render_.pages.find(page);
        PageScene scene;
        if (!layout_pending_ && prev_slot != nullptr && prev_slot->scene != nullptr &&
            prev_slot->scene_input_key == scene_key) {
            scene = *prev_slot->scene;
            ++edit_stats_.scene_reused_pages;
        } else {
            const auto t_scene = Clock::now();
            scene = render_.buildScene(render_in, page);
            // 场景构建本身也要计时：全篇重排那一拍通常所有页都新鲜，若只在“不新鲜”分支累加，
            //   统计会读成 0，看起来像这一项已经不干活了。
            scene_ms += msSince(t_scene);
            ++edit_stats_.scene_built_pages;
        }
        if (render_.pages.fresh(page, scene, page_px_w, page_px_h)) continue;
        ++rastered;
        ++edit_stats_.dirty_page_count;

        const bool has_prev = prev_slot != nullptr && prev_slot->valid &&
                              prev_slot->raster_sig == render_.pages.signature() &&
                              prev_slot->px.width() == page_px_w &&
                              prev_slot->px.height() == page_px_h;
        // 竖排页仍整页重画：行带在竖排下会变成竖条，而擦除与字形筛选都按纵轴写。
        //   本条与旧实现同口径；换成任意矩形重放之后再单独处理。
        bool vertical = false;
        for (int k : layout_service_.layout.linesOnPage(page)) {
            if (layout_service_.layout.lines[static_cast<size_t>(k)].vertical) {
                vertical = true;
                break;
            }
        }
        SceneDiff scene_diff;
        if (has_prev) {
            const auto t_diff = Clock::now();
            scene_diff = diffPageScene(prev_slot->scene.get(), scene, page_bounds);
            diff_ms += msSince(t_diff);
        }

        const bool full = !has_prev || vertical || scene_diff.full_page;
        if (full) {
            rasterPageInto(page, nullptr, nullptr, &scene);
            markDirty(page, Rect{0.0f, 0.0f, static_cast<float>(page_px_w),
                                 static_cast<float>(page_px_h)});
            edit_stats_.band_y0 = 0;
            edit_stats_.band_y1 = page_px_h;
        } else if (scene_diff.band_hi_pt > scene_diff.band_lo_pt) {
            rasterPageInto(page, &scene_diff.band_lo_pt, &scene_diff.band_hi_pt, nullptr);
            // 取整口径与 notePageDirty 的取整完全一致（floor 起点 / ceil 终点）：
            //   少取一行就是"版面上留一条旧墨迹"，只在恰巧跨半像素时出现。
            const float y0 = std::max(0.0f, scene_diff.band_lo_pt * density - 1.0f);
            const float y1 =
                std::min(static_cast<float>(page_px_h), scene_diff.band_hi_pt * density + 1.0f);
            markDirty(page, Rect{0.0f, y0, static_cast<float>(page_px_w), std::max(1.0f, y1 - y0)});
            edit_stats_.band_y0 = static_cast<int>(y0);
            edit_stats_.band_y1 = static_cast<int>(y1);
        } else {
            // root 变了却没有可用的差异：保守起见整页重画，不能停在旧像素上。
            rasterPageInto(page, nullptr, nullptr, &scene);
            markDirty(page, Rect{0.0f, 0.0f, static_cast<float>(page_px_w),
                                 static_cast<float>(page_px_h)});
            edit_stats_.band_y0 = 0;
            edit_stats_.band_y1 = page_px_h;
        }
        PageSlot& slot = render_.pages.at(page);
        slot.scene = std::make_shared<PageScene>(std::move(scene));
        slot.scene_input_key = scene_key;
        slot.raster_sig = render_.pages.signature();
        slot.valid = true;
    }
    edit_stats_.repainted_pages = rastered;
    // 这一项记“构建场景 + 比新旧两版”的耗时。字段名沿用旧的 page_fp_ms，
    //   宿主与探针的面板不必跟着改。
    stats_.page_fp_ms = scene_ms + diff_ms;
    edit_stats_.page_fp_ms = scene_ms + diff_ms;
    // 淘汰放在所有光栅之后：这一拍用到的页一页都不能丢，窗口外再按距离释放。
    trimPageCache(first_page, count);

    const auto t_swap = Clock::now();

    // 二、覆盖层：按页算"这一页现在该叠哪些矩形"，与上一拍记下的那份做差。
    //   只有真的变了才记脏 —— 光标每闪一下都会走这里，没变的页不该产生任何上传。
    //   层序由 OverlayScene 的数组顺序表达：选区、图片手柄、光标。
    for (int i = 0; i < count; ++i) {
        const int page = first_page + i;
        PageSlot* slot = render_.pages.find(page);
        if (slot == nullptr) continue;
        OverlayScene now;
        collectTransient(page, &now);
        now.root = overlayRoot(now);
        const OverlayScene& prev = slot->overlay;
        if (prev.root == now.root) continue;
        for (const OverlayItem& it : prev.items) {
            if (!overlayHas(now, it.layer, it.rect)) markDirty(page, it.rect);
        }
        for (const OverlayItem& it : now.items) {
            if (!overlayHas(prev, it.layer, it.rect)) markDirty(page, it.rect);
        }
        slot->overlay = std::move(now);
    }
    // 窗口外的页不留覆盖账：它记的是"上一拍这一页叠了什么"，而那一页已经不在画布上。
    //   留着的话，读 pageRgba(窗口外的页) 会得到一个过期光标 —— 探针正是那么读的。
    for (auto& kv : render_.pages.slots()) {
        if (kv.first >= first_page && kv.first < first_page + count) continue;
        kv.second.overlay.items.clear();
        kv.second.overlay.root = 0;
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
    edit_stats_.caret_line_aff = caretLineAffinity();
    {
        const Range sel_now = selection();
        edit_stats_.selection_anchor = sel_now.anchor;
        edit_stats_.selection_focus = sel_now.focus;
    }
    edit_stats_.caret_vertical = caretVertical();
    edit_stats_.caret_block_reversed = caretBlockReversed();
    edit_stats_.caret_page = edit_session_.caret.caretPage();
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
 * 这一页现在该叠哪些覆盖项：选区矩形（页内像素）+ 图片手柄 + 光标矩形。
 *
 * 一处算、两处用：presentInternal 按页算差集，pageDisplayPixels 按页取来叠。
 *   两处各写一份的话，"记脏的矩形"与"画上去的矩形"迟早分叉 —— 那正是旧版
 *   transient_dirty 漏 93 px 的形状（画了但没记，画布上留一块旧像素）。
 */
void EditorEngine::collectTransient(int page, OverlayScene* out) const {
    out->items.clear();
    auto add = [&](OverlayLayer layer, const Rect& r) {
        if (r.w <= 0.0f || r.h <= 0.0f) return;
        out->items.push_back(OverlayItem{layer, r});
    };

    // 选区在下。
    for (const PageRect& pr : selectionRectsPxIn(page, 1)) add(OverlayLayer::Selection, pr.r_pt);

    // 图片拖拽框随后。
    std::vector<Rect> image_ui;
    collectImageUi(page, &image_ui);
    for (const Rect& r : image_ui) add(OverlayLayer::ImageUi, r);

    int cp = -1;
    const Rect cr = caretRectPx(&cp);
    const bool draw_caret = caretShown();
    Rect caret{};
    if (cp == page && draw_caret && cr.w > 0.0f && cr.h > 0.0f) caret = cr;

    // 连页接缝：上一条带里越过带底的选区与光标，在本带顶部还有一半。
    //   两带各画自己那半，与内容的补画是同一条规则 —— 否则接缝上会出现
    //   "字补上了、底色高亮只到一半"。
    if (cfg_.page.continuous && page > 0) {
        const float slab_px = cfg_.page.slabHeightPt() * cfg_.density;
        const float ph = static_cast<float>(pagePxHeight());
        for (const PageRect& pr : selectionRectsPxIn(page - 1, 1)) {
            const float bot = pr.r_pt.y + pr.r_pt.h;
            if (bot <= slab_px) continue;
            const float y0 = std::max(0.0f, pr.r_pt.y - slab_px);
            const float y1 = std::min(ph, bot - slab_px);
            if (y1 > y0) add(OverlayLayer::Selection, Rect{pr.r_pt.x, y0, pr.r_pt.w, y1 - y0});
        }
        if (cp == page - 1 && draw_caret && cr.w > 0.0f && cr.h > 0.0f && caret.w <= 0.0f) {
            const float bot = cr.y + cr.h;
            const float y0 = std::max(0.0f, cr.y - slab_px);
            const float y1 = std::min(ph, bot - slab_px);
            if (bot > slab_px && y1 > y0) caret = Rect{cr.x, y0, cr.w, y1 - y0};
        }
    }

    // 光标最后叠。
    add(OverlayLayer::Caret, caret);
}

/**
 * 一页"画到画布上应该是什么"：干净页位图 + 这一页的覆盖层。
 *
 * 没有覆盖项的页直接给页位图本身（窗口里通常有四页属于这一档，一页 3.5 MB 的拷贝能省就省）。
 * 有覆盖项时拷进 page_export_ 再叠 —— 拷的是内容、叠的是覆盖项，于是"旧光标"不可能残留：
 *   它从来没被写进任何持久位图里。
 */
const uint8_t* EditorEngine::pageDisplayPixels(int page) const {
    const PageSlot* pb = render_.pages.find(page);
    if (pb == nullptr || !pb->valid) return nullptr;
    if (pb->px.width() != pagePxWidth() || pb->px.height() != pagePxHeight()) return nullptr;
    if (pb->overlay.items.empty()) return pb->px.data();

    const int pw = pb->px.width();
    const int ph = pb->px.height();
    if (page_export_.width() != pw || page_export_.height() != ph) {
        page_export_.reset(pw, ph);
        page_export_.shareFonts(font_owner_);
    }
    page_export_.blitFrom(pb->px, Rect{0.0f, 0.0f, static_cast<float>(pw), static_cast<float>(ph)},
                          0, 0);
    for (const OverlayItem& it : pb->overlay.items) {
        if (it.layer == OverlayLayer::Selection) {
            page_export_.blendRect(it.rect, cfg_.selection_color);
        } else if (it.layer == OverlayLayer::ImageUi) {
            page_export_.fillRect(it.rect, cfg_.image_mark);
        } else if (it.layer == OverlayLayer::Caret) {
            page_export_.fillRect(it.rect, cfg_.caret_color);
        }
    }
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
            const PageSlot* slot = render_.pages.find(page);
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
    edit_session_.caret_visible = true;
    // 编辑之后尺寸预览作废：文档已经按提交后的样子重排，再留一个旧框只会画错位置。
    clearImageResizePreview();
    repaint();
}

}  // namespace heditor
