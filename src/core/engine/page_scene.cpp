/**
 * 页场景的实现：把一页的绘制输入枚举成可比较的项。
 *
 * 每一项的 version 覆盖该项的全部绘制输入，页位图按 root 复用；整页光栅时同一份
 * drawPage 还可通过 RecordingSink 留下 ops，供 rasterPageScene 重放。
 *
 * 哈希一律按位混浮点，不做量化：量化会让亚像素移动被判成“没变”，那正是要消灭的失效。
 */
#include "core/engine/page_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/control.h"
#include "core/engine/page_raster.h"
#include "core/layout/layout.h"
#include "core/render/bitmap.h"
#include "core/utils/hash.h"
#include "core/utils/time.h"

namespace heditor {
namespace {

/**
 * 混入一个颜色（四个通道）。
 * @param h 累加器
 * @param c 颜色
 * @returns 新的累加器
 */
uint64_t mixColor(uint64_t h, const Color& c) {
    h = fnvMix(h, static_cast<uint64_t>(c.r));
    h = fnvMix(h, static_cast<uint64_t>(c.g));
    h = fnvMix(h, static_cast<uint64_t>(c.b));
    h = fnvMix(h, static_cast<uint64_t>(c.a));
    return h;
}

/**
 * 混入一个矩形（四个浮点按位）。
 * @param h 累加器
 * @param r 矩形
 * @returns 新的累加器
 */
uint64_t mixRect(uint64_t h, const Rect& r) {
    h = fnvMixFloat(h, r.x);
    h = fnvMixFloat(h, r.y);
    h = fnvMixFloat(h, r.w);
    h = fnvMixFloat(h, r.h);
    return h;
}

/**
 * 混入一个字符串（长度一起混）。
 * @param h 累加器
 * @param s 字符串
 * @returns 新的累加器
 */
uint64_t mixString(uint64_t h, const std::string& s) {
    h = fnvMix(h, s.size());
    if (!s.empty()) h = fnvMixBytes(h, s.data(), s.size());
    return h;
}

/**
 * 页底色、页边框、裁切标记与水印的版本。
 * @param in 绘制输入
 * @param kind 场景项种类
 * @returns 版本
 */
uint64_t pageDecorVersion(const RenderInput& in, SceneKind kind) {
    const EngineConfig& cfg = *in.cfg;
    uint64_t h = kFnvBasis;
    h = fnvMix(h, static_cast<uint64_t>(kind));
    h = fnvMixFloat(h, cfg.page.width_pt);
    h = fnvMixFloat(h, cfg.page.height_pt);
    h = fnvMixFloat(h, cfg.page.margins.left);
    h = fnvMixFloat(h, cfg.page.margins.top);
    h = fnvMixFloat(h, cfg.page.margins.right);
    h = fnvMixFloat(h, cfg.page.margins.bottom);
    h = fnvMix(h, cfg.page.continuous ? 1ull : 0ull);
    if (kind == SceneKind::PageFill) {
        return mixColor(h, cfg.page_fill);
    }
    if (kind == SceneKind::PageBorder) {
        h = fnvMix(h, cfg.draw_page_border ? 1ull : 0ull);
        return mixColor(h, cfg.page_border);
    }
    if (kind == SceneKind::MarginMarks) {
        h = fnvMix(h, cfg.show_margin_marks ? 1ull : 0ull);
        return mixColor(h, cfg.margin_mark_color);
    }
    if (kind == SceneKind::Watermark) {
        h = fnvMix(h, in.watermark.enabled ? 1ull : 0ull);
        h = mixString(h, in.watermark.text);
        h = fnvMixFloat(h, in.watermark.size_pt);
        h = fnvMixFloat(h, in.watermark.angle_deg);
        return mixColor(h, in.watermark.color);
    }
    return h;
}

/**
 * 一行的绘制版本：行盒几何、簇、对象摘要与这一行的字形。
 *
 * 比只看字形坐标多三样：浮点按位而不做 1/16 磅量化、字体实例与字重进版本、
 * 对象表的提示文字进版本。后两项是“改了但画面不变”的两个已知缺口。
 * @param in 绘制输入
 * @param li 行下标
 * @returns 版本
 */
uint64_t lineVersion(const RenderInput& in, int li) {
    const LayoutResult& L = *in.layout;
    const LineBox& lb = L.lines[static_cast<size_t>(li)];
    uint64_t h = kFnvBasis;
    h = fnvMixFloat(h, lb.block_lo_pt);
    h = fnvMixFloat(h, lb.block_hi_pt);
    h = fnvMixFloat(h, lb.inline_origin_pt);
    h = fnvMixFloat(h, lb.inline_end_pt);
    h = fnvMixFloat(h, lb.size_pt);
    h = fnvMixFloat(h, lb.bracket_axis_pt);
    h = fnvMixFloat(h, lb.advance_pt);
    h = fnvMixFloat(h, lb.lead_top_pt);
    h = fnvMix(h, lb.clip_text ? 1ull : 0ull);
    h = fnvMix(h, lb.vertical ? 1ull : 0ull);
    h = fnvMix(h, lb.sideways ? 1ull : 0ull);
    h = fnvMix(h, static_cast<uint64_t>(lb.paragraph));
    h = fnvMix(h, static_cast<uint64_t>(lb.first_cp));
    h = fnvMix(h, static_cast<uint64_t>(lb.last_cp));

    for (const LineCluster& c : lb.clusters) {
        h = fnvMix(h, static_cast<uint64_t>(c.cp));
        h = fnvMixFloat(h, c.inline_pt);
        h = fnvMix(h, (c.placeholder ? 1ull : 0ull) | (c.read_only ? 2ull : 0ull) |
                          (static_cast<uint64_t>(c.bracket_side) << 2) |
                          (c.control_hint ? 32ull : 0ull));
        h = fnvMix(h, static_cast<uint64_t>(c.atom));
        h = fnvMixFloat(h, c.atom_h_pt);
        h = fnvMix(h, c.content_hash);
    }

    // 对象表里的提示文字不在簇上：它挂在起始边界旁，绘制时按 owner 回树里取。
    //   不混字符串的话，换一句等宽的提示文字会被判成“这一行没变”。
    if (in.blocks != nullptr && lb.paragraph >= 0 &&
        lb.paragraph < static_cast<int>(in.blocks->size())) {
        const BlockSpec& b = (*in.blocks)[static_cast<size_t>(lb.paragraph)];
        for (const InlineObject& o : b.objects) {
            if (o.cp < lb.first_cp || o.cp > lb.last_cp) continue;
            h = fnvMix(h, o.content_hash);
            h = mixString(h, o.hint);
        }
    }

    // 字形按 line 单调，二分到这一行；与旧实现的定位口径相同。
    const std::vector<PlacedGlyph>& gs = L.glyphs;
    const auto gb = std::lower_bound(gs.begin(), gs.end(), li,
                                     [](const PlacedGlyph& g, int line) { return g.line < line; });
    for (auto it = gb; it != gs.end() && it->line == li; ++it) {
        h = fnvMix(h, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(it->font)));
        h = fnvMix(h, static_cast<uint64_t>(it->glyph_id));
        h = fnvMixFloat(h, it->x_pt);
        h = fnvMixFloat(h, it->y_pt);
        h = fnvMixFloat(h, it->advance_pt);
        h = fnvMixFloat(h, it->rotate_deg);
        h = fnvMixFloat(h, it->size_pt);
        h = fnvMixFloat(h, it->weight);
        h = fnvMix(h, it->italic ? 1ull : 0ull);
        h = fnvMix(h, it->underline ? 1ull : 0ull);
        h = mixColor(h, it->color);
    }
    return h;
}

/**
 * 一页各行独占的块轴区间（页内磅）。
 *
 * 行带规则：相邻两组的分界取中线，第一组从页顶起、最后一组到页底止。
 * 单行页因此是整页 —— 这条判据保证擦除范围不会啃到相邻行的抗锯齿边。
 *
 * @param in 绘制输入
 * @param page 页号
 * @param page_w 页宽（磅）
 * @param page_h 页高（磅）
 * @returns 与 layout.lines 同长的范围表；不在这一页的行留空
 */
std::vector<Rect> lineBandsForPage(const RenderInput& in, int page, float page_w, float page_h) {
    const LayoutResult& L = *in.layout;
    std::vector<Rect> out(L.lines.size());
    const std::vector<int>& ids = L.linesOnPage(page);
    if (ids.empty()) return out;

    struct Item {
        float top = 0.0f;
        float bottom = 0.0f;
        int line = 0;
    };
    struct Group {
        float top = 0.0f;
        float bottom = 0.0f;
    };
    std::vector<Item> items;
    items.reserve(ids.size());
    for (int k : ids) {
        const LineBox& lb = L.lines[static_cast<size_t>(k)];
        items.push_back(Item{lb.block_lo_pt, lb.block_hi_pt, k});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.top != b.top) return a.top < b.top;
        return a.line < b.line;
    });
    std::vector<Group> groups;
    groups.reserve(items.size());
    for (const Item& it : items) {
        if (!groups.empty() && it.top < groups.back().bottom - 0.01f) {
            groups.back().bottom = std::max(groups.back().bottom, it.bottom);
            continue;
        }
        groups.push_back(Group{it.top, it.bottom});
    }

    size_t gi = 0;
    for (const Item& it : items) {
        while (gi + 1 < groups.size() && it.top >= groups[gi + 1].top - 0.01f) ++gi;
        const float top_edge = (gi == 0) ? 0.0f : 0.5f * (groups[gi - 1].bottom + groups[gi].top);
        const float bottom_edge =
            (gi + 1 >= groups.size()) ? page_h : 0.5f * (groups[gi].bottom + groups[gi + 1].top);
        const LineBox& lb = L.lines[static_cast<size_t>(it.line)];
        if (lb.vertical) {
            out[static_cast<size_t>(it.line)] = Rect{0.0f, 0.0f, page_w, page_h};
        } else {
            out[static_cast<size_t>(it.line)] =
                Rect{0.0f, top_edge, page_w, std::max(0.0f, bottom_edge - top_edge)};
        }
    }
    return out;
}

/**
 * 一行表格的版本：表的列边界、这一行的位置与切口、这一行各格的跨度。
 * @param in 绘制输入
 * @param pt 已放置的表
 * @param pi placed 里的下标
 * @returns 版本
 */
uint64_t tableRowVersion(const RenderInput& in, const PlacedTable& pt, size_t pi) {
    const PlacedRow& pr = pt.placed[pi];
    uint64_t h = kFnvBasis;
    h = fnvMix(h, pt.handle);
    h = fnvMixFloat(h, pt.x);
    h = fnvMixFloat(h, pt.w);
    h = fnvMix(h, static_cast<uint64_t>(pt.rows));
    h = fnvMix(h, static_cast<uint64_t>(pt.cols));
    for (float cx : pt.col_x) h = fnvMixFloat(h, cx);
    h = fnvMix(h, static_cast<uint64_t>(pr.row));
    h = fnvMix(h, static_cast<uint64_t>(pr.page));
    h = fnvMixFloat(h, pr.y);
    h = fnvMixFloat(h, pr.h);
    h = fnvMix(h, (pr.cont_top ? 1ull : 0ull) | (pr.cont_bottom ? 2ull : 0ull));
    // 这一片的下边有没有补画，取决于下一片是否同页。把它进版本，跨页切点变化才不会漏。
    const bool seam_bottom = (!in.cfg->page.continuous) &&
                             (pi + 1 >= pt.placed.size() || pt.placed[pi + 1].page != pr.page);
    h = fnvMix(h, seam_bottom ? 1ull : 0ull);
    for (const LayoutCell& lc : pt.cells) {
        if (lc.row != pr.row) continue;
        h = fnvMix(h, static_cast<uint64_t>(lc.row));
        h = fnvMix(h, static_cast<uint64_t>(lc.cell));
        h = fnvMix(h, static_cast<uint64_t>(lc.row_span));
        h = fnvMix(h, static_cast<uint64_t>(lc.col_span));
        h = fnvMix(h, static_cast<uint64_t>(lc.v_align));
    }
    return mixColor(h, in.cfg->table_line);
}

/**
 * 一张浮动图的版本：位置、尺寸、环绕、拖动预览与图片代际。
 * @param in 绘制输入
 * @param pf 已放置的浮动图
 * @returns 版本
 */
uint64_t floatVersion(const RenderInput& in, const PlacedFloat& pf) {
    uint64_t h = kFnvBasis;
    h = fnvMix(h, pf.handle);
    h = fnvMix(h, static_cast<uint64_t>(pf.page));
    h = fnvMixFloat(h, pf.x);
    h = fnvMixFloat(h, pf.y);
    h = fnvMixFloat(h, pf.w);
    h = fnvMixFloat(h, pf.h);
    h = fnvMix(h, static_cast<uint64_t>(pf.wrap));
    h = fnvMix(h, pf.content_hash);
    if (in.images != nullptr) h = fnvMix(h, in.images->generation());
    float fx = pf.x;
    float fy = pf.y;
    floatPreviewPos(*in.cfg, in.float_preview, pf, &fx, &fy);
    h = fnvMixFloat(h, fx);
    h = fnvMixFloat(h, fy);
    h = fnvMix(h, in.float_preview.placed ? 1ull : 0ull);
    return h;
}

/**
 * 控件标记的版本：只放“画法开关”，不放任何行/簇的几何。
 *
 * 括号、提示、小点的位置会随着文字编辑移动，但那一位的 LineBox 同时也在变；
 * 行场景项已经会把那条带标脏，并顺带重画带内的标记。这里若再混入几何，任何一次
 * 打字都会退化成整页重画 —— 旧实现的行带收益就没了。
 * @param in 绘制输入
 * @returns 版本
 */
uint64_t controlMarksVersion(const RenderInput& in) {
    uint64_t h = kFnvBasis;
    h = fnvMix(h, static_cast<uint64_t>(in.mode));
    h = fnvMix(h, in.cfg->show_brackets ? 1ull : 0ull);
    h = fnvMix(h, in.cfg->show_control_dots ? 1ull : 0ull);
    h = mixColor(h, in.cfg->control_mark);
    h = mixColor(h, in.cfg->control_mark_ro);
    h = fnvMixFloat(h, in.cfg->control_dot_pt);
    return h;
}

/**
 * 编辑标记的版本：只放开关、模式与颜色。
 *
 * 段落标记的位置由行盒给出；行场景项变了就重画那条带，标记跟着重画。这里混入行几何
 * 只会把每次打字变成整页重画。
 * @param in 绘制输入
 * @returns 版本
 */
uint64_t editingMarksVersion(const RenderInput& in) {
    uint64_t h = kFnvBasis;
    h = fnvMix(h, in.marks_shown ? 1ull : 0ull);
    h = fnvMix(h, static_cast<uint64_t>(in.mode));
    h = mixColor(h, in.cfg->mark_color);
    return h;
}

/**
 * 往场景里追加一项。
 * @param scene 场景
 * @param layer 层
 * @param kind 种类
 * @param owner 稳定身份
 * @param sub 同一 owner 下的序号
 * @param spill 是否是上一页溢到本页的那一份
 * @param version 版本
 * @param bounds 页内范围
 */
void addItem(PageScene* scene, SceneLayer layer, SceneKind kind, uint64_t owner, uint32_t sub,
             bool spill, uint64_t version, const Rect& bounds) {
    SceneItem it;
    it.key = SceneItemKey{layer, kind, owner, sub, spill};
    it.version = version;
    it.bounds_pt = bounds;
    scene->items.push_back(it);
}

/**
 * 算场景根：按绘制序混 key、version 与 bounds。
 * @param scene 场景
 */
void finalizeScene(PageScene* scene) {
    uint64_t h = kFnvBasis;
    for (const SceneItem& it : scene->items) {
        h = fnvMix(h, static_cast<uint64_t>(it.key.layer));
        h = fnvMix(h, static_cast<uint64_t>(it.key.kind));
        h = fnvMix(h, it.key.owner);
        h = fnvMix(h, static_cast<uint64_t>(it.key.sub));
        h = fnvMix(h, it.key.spill ? 1ull : 0ull);
        h = fnvMix(h, it.version);
        h = mixRect(h, it.bounds_pt);
    }
    scene->root = h;
}

/**
 * 两个矩形是否相同（按位比较，不按容差）。
 * @param a 左矩形
 * @param b 右矩形
 * @returns 是否相同
 */
bool sameRect(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/**
 * 两个矩形的并集。
 * @param a 左矩形
 * @param b 右矩形
 * @returns 并集
 */
Rect unionRect(const Rect& a, const Rect& b) {
    const float x0 = std::min(a.x, b.x);
    const float y0 = std::min(a.y, b.y);
    const float x1 = std::max(a.x + a.w, b.x + b.w);
    const float y1 = std::max(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

/**
 * 把矩形裁进页内；完全在外时返回零矩形。
 * @param r 矩形
 * @param page 页面范围
 * @returns 裁剪后的矩形
 */
Rect clipRect(const Rect& r, const Rect& page) {
    const float x0 = std::max(r.x, page.x);
    const float y0 = std::max(r.y, page.y);
    const float x1 = std::min(r.x + r.w, page.x + page.w);
    const float y1 = std::min(r.y + r.h, page.y + page.h);
    if (x1 <= x0 || y1 <= y0) return Rect{};
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

PageScene buildPageScene(const RenderInput& in, int page) {
    PageScene scene;
    if (in.layout == nullptr || in.cfg == nullptr) return scene;
    const LayoutResult& L = *in.layout;
    const EngineConfig& cfg = *in.cfg;
    const int page_count = std::max(1, L.stats.page_count);
    if (page < 0 || page >= page_count) return scene;

    const float page_w = cfg.page.width_pt;
    const float page_h = cfg.page.height_pt;
    const Rect page_rect{0.0f, 0.0f, page_w, page_h};
    const bool spill = cfg.page.continuous && page > 0;
    const float slab_pt = cfg.page.slabHeightPt();

    // 页底色、页边框、裁切标记、水印与图片资源代际：这些都决定“纸上有什么”，与具体字形无关。
    addItem(&scene, SceneLayer::Background, SceneKind::PageFill, 0, 0, false,
            pageDecorVersion(in, SceneKind::PageFill), page_rect);
    if (cfg.draw_page_border && !cfg.page.continuous) {
        addItem(&scene, SceneLayer::PageBorder, SceneKind::PageBorder, 0, 0, false,
                pageDecorVersion(in, SceneKind::PageBorder), page_rect);
    }
    if (cfg.show_margin_marks && !cfg.page.continuous) {
        addItem(&scene, SceneLayer::MarginMark, SceneKind::MarginMarks, 0, 0, false,
                pageDecorVersion(in, SceneKind::MarginMarks), page_rect);
    }
    if (in.watermark.enabled && !in.watermark.text.empty()) {
        addItem(&scene, SceneLayer::Watermark, SceneKind::Watermark, 0, 0, false,
                pageDecorVersion(in, SceneKind::Watermark), page_rect);
    }
    if (in.images != nullptr) {
        addItem(&scene, SceneLayer::Image, SceneKind::ImageState, 0, 0, false,
                fnvMix(kFnvBasis, in.images->generation()), page_rect);
    }

    // 衬于文字下方的浮动图先画。
    auto addFloats = [&](int p, bool want_spill) {
        for (const PlacedFloat& pf : L.placed_floats) {
            if (pf.page != p) continue;
            if (pf.wrap != ImageWrap::Behind) continue;
            if (want_spill && !(pf.y + pf.h > slab_pt)) continue;
            const Rect b = want_spill ? page_rect : Rect{pf.x, pf.y, pf.w, pf.h};
            addItem(&scene, SceneLayer::Image, SceneKind::Float, pf.handle, 0, want_spill,
                    floatVersion(in, pf), b);
        }
    };
    addFloats(page, false);

    // 表格格线：一页上的每一片行各一项。
    auto addTables = [&](int p, bool want_spill) {
        for (const PlacedTable& pt : L.placed_tables) {
            for (size_t pi = 0; pi < pt.placed.size(); ++pi) {
                const PlacedRow& pr = pt.placed[pi];
                if (pr.page != p) continue;
                if (want_spill && !(pr.y + pr.h > slab_pt)) continue;
                const Rect b = want_spill ? page_rect : Rect{0.0f, pr.y, page_w, pr.h};
                addItem(&scene, SceneLayer::TableLine, SceneKind::TableRow, pt.handle,
                        static_cast<uint32_t>(pr.row), want_spill, tableRowVersion(in, pt, pi), b);
            }
        }
    };
    addTables(page, false);

    // 字形：一行一项。
    auto addLines = [&](int p, bool want_spill) {
        const std::vector<Rect> bands = lineBandsForPage(in, p, page_w, page_h);
        for (int fi : L.fragments.fragmentsOnPage(p)) {
            const FragmentIndex::Fragment& f = L.fragments.fragments[static_cast<size_t>(fi)];
            for (int li = f.first_line; li <= f.last_line; ++li) {
                const LineBox& lb = L.lines[static_cast<size_t>(li)];
                if (want_spill && !(lb.block_hi_pt > slab_pt)) continue;
                const Rect b = want_spill ? page_rect : bands[static_cast<size_t>(li)];
                addItem(&scene, SceneLayer::Glyph, SceneKind::GlyphLine, static_cast<uint64_t>(li),
                        0, want_spill, lineVersion(in, li), b);
            }
        }
    };
    addLines(page, false);

    // 浮于文字上方的浮动图在字形之后。
    auto addFloatsAbove = [&](int p, bool want_spill) {
        for (const PlacedFloat& pf : L.placed_floats) {
            if (pf.page != p) continue;
            if (pf.wrap == ImageWrap::Behind) continue;
            if (want_spill && !(pf.y + pf.h > slab_pt)) continue;
            const Rect b = want_spill ? page_rect : Rect{pf.x, pf.y, pf.w, pf.h};
            addItem(&scene, SceneLayer::Image, SceneKind::Float, pf.handle, 0, want_spill,
                    floatVersion(in, pf), b);
        }
    };
    addFloatsAbove(page, false);

    // 连续模式的接缝：上一页越过带底的那一份在本页顶部再画一遍。整页作为范围，
    //   接缝上的变化宁可多画一页，也不要在下一页留半行旧墨。
    if (spill) {
        addFloats(page - 1, true);
        addTables(page - 1, true);
        addLines(page - 1, true);
        addFloatsAbove(page - 1, true);
    }

    // 控件标记与编辑标记是页面级项：它们的几何来自行盒与控件集合，范围取整页。
    //   这样任何一项变化都会整页重画，和旧实现“表单标记变化就清页缓存”同口径。
    addItem(&scene, SceneLayer::ControlMark, SceneKind::ControlMarks, 0, 0, false,
            controlMarksVersion(in), page_rect);
    addItem(&scene, SceneLayer::EditingMark, SceneKind::EditingMarks, 0, 0, false,
            editingMarksVersion(in), page_rect);

    finalizeScene(&scene);
    return scene;
}

SceneDiff diffPageScene(const PageScene* before, const PageScene& after, const Rect& page_bounds) {
    SceneDiff diff;
    if (before == nullptr || before->root == after.root) return diff;
    if (before->items.size() != after.items.size()) {
        diff.full_page = true;
        return diff;
    }
    // 先比 key 序列：顺序变了说明增删或重排，直接整页，省得算一堆互相重叠的矩形。
    for (size_t i = 0; i < after.items.size(); ++i) {
        if (!(before->items[i].key == after.items[i].key)) {
            diff.full_page = true;
            return diff;
        }
    }

    bool any = false;
    float band_lo = 0.0f;
    float band_hi = 0.0f;
    for (size_t i = 0; i < after.items.size(); ++i) {
        const SceneItem& a = before->items[i];
        const SceneItem& b = after.items[i];
        if (a.version == b.version && sameRect(a.bounds_pt, b.bounds_pt)) continue;
        Rect r = unionRect(a.bounds_pt, b.bounds_pt);
        r = clipRect(r, page_bounds);
        if (r.w <= 0.0f || r.h <= 0.0f) continue;
        if (r.w >= page_bounds.w * 0.9f && r.h >= page_bounds.h * 0.9f) {
            diff.full_page = true;
            return diff;
        }
        diff.rects.push_back(r);
        if (!any) {
            band_lo = r.y;
            band_hi = r.y + r.h;
            any = true;
        } else {
            band_lo = std::min(band_lo, r.y);
            band_hi = std::max(band_hi, r.y + r.h);
        }
    }
    if (!any) {
        // root 变了却没有任何项变化：只可能是哈希碰撞或场景根算法有漏洞。整页重画，
        //   宁可多画一页，也不要让画面停在旧像素上。
        diff.full_page = true;
        return diff;
    }
    diff.band_lo_pt = band_lo;
    diff.band_hi_pt = band_hi;
    return diff;
}

void rasterPageScene(Bitmap& target, const PageScene& scene, PageRasterCost* cost,
                     const Rect* clip) {
    struct Replay {
        Bitmap& b;

        void operator()(const FillRectOp& o) const { b.fillRect(o.r, o.color); }

        void operator()(const StrokeRectOp& o) const { b.strokeRect(o.r, o.color, o.thickness); }

        void operator()(const FillCircleOp& o) const {
            b.fillCircle(o.cx, o.cy, o.radius, o.color);
        }

        void operator()(const StrokeCircleOp& o) const {
            b.strokeCircle(o.cx, o.cy, o.radius, o.color, o.thickness);
        }

        void operator()(const FillTriangleOp& o) const {
            b.fillTriangle(o.p[0], o.p[1], o.p[2], o.p[3], o.p[4], o.p[5], o.color);
        }

        void operator()(const StrokePolylineOp& o) const {
            if (!o.pts.empty()) {
                b.strokePolyline(o.pts.data(), static_cast<int>(o.pts.size() / 2), o.color,
                                 o.width);
            }
        }

        void operator()(const DrawGlyphOp& o) const {
            b.drawGlyph(o.font, o.glyph_id, o.x, o.y, o.units_to_px, o.color, o.rotate_deg);
        }

        void operator()(const DrawGlyphsOp& o) const {
            if (o.ids.empty()) return;
            b.drawGlyphs(o.font, o.ids.data(), o.xs.data(), o.ys.data(),
                         static_cast<int>(o.ids.size()), o.units_to_px, o.color, o.rotate_deg,
                         o.italic);
        }

        void operator()(const DrawImageOp& o) const {
            b.drawImage(o.key, o.version, o.rgba, o.src_w, o.src_h, o.dst);
        }

        void operator()(const PushClipOp& o) const { b.pushClipRect(o.r); }

        void operator()(const RestoreMarkOp& o) const { b.restoreToMark(o.mark); }
    };

    int clip_mark = 0;
    if (clip != nullptr) {
        clip_mark = target.clipMark();
        target.pushClipRect(*clip);
    }
    for (const PageOp& op : scene.ops) {
        const auto t0 = Clock::now();
        std::visit(Replay{target}, op);
        if (cost == nullptr) continue;
        const bool is_fill =
            std::holds_alternative<FillRectOp>(op) || std::holds_alternative<StrokeRectOp>(op) ||
            std::holds_alternative<FillCircleOp>(op) ||
            std::holds_alternative<StrokeCircleOp>(op) ||
            std::holds_alternative<FillTriangleOp>(op) ||
            std::holds_alternative<StrokePolylineOp>(op) ||
            std::holds_alternative<PushClipOp>(op) || std::holds_alternative<RestoreMarkOp>(op);
        if (is_fill) {
            cost->fill_ms += msSince(t0);
        } else {
            cost->glyph_ms += msSince(t0);
        }
    }
    if (clip != nullptr) target.restoreToMark(clip_mark);
}

}  // namespace heditor
