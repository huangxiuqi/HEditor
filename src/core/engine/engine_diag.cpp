/**
 * 诊断：把内部状态摊平成可读文本，供宿主与探针取证。
 *
 * 与审计的区别：审计给"符不符合"的判定，这里给"现在长什么样"的原始材料，人看的是它。
 * 所以口径是尽量多、尽量直白，不做取舍。
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

namespace {

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

std::string EditorEngine::describeControls() const {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << "edit-mode=" << editModeName(mode_) << "  controls=" << controls_.size()
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
       << (controls_.empty() ? -1 : controls_.innermostAt(caret(), /*include_end=*/true)) << "\n";
    return os.str();
}

/**
 * 诊断：把内部状态摊平成可读文本，供宿主与探针取证。
 */

std::string EditorEngine::describeCaret() const {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(3);
    os << "caret: para=" << caret().para << " offset(段内码点)=" << caret().offset
       << " 全文下标=" << edit_stats_.caret_index << " 全文码点=" << documentCodepoints() << "\n";

    const LineBox* lb = caret_.findLineFor(caret());
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
    os << "  本偏移的沿轴位置="
       << lb->offsetAtSide(static_cast<uint32_t>(caret().offset), caretSide()) << " pt\n";

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

}  // namespace heditor
