/**
 * 记录型绘制接收端的实现：保存参数，并在有目标位图时原样转发。
 */
#include "core/render/page_recorder.h"

#include <utility>

namespace heditor {

RecordingSink::RecordingSink(PageScene* scene, int w, int h, Bitmap* draw)
    : scene_(scene), draw_(draw), w_(w), h_(h) {}

void RecordingSink::fillRect(const Rect& r, Color c) {
    scene_->ops.emplace_back(FillRectOp{r, c});
    if (draw_ != nullptr) draw_->fillRect(r, c);
}

void RecordingSink::strokeRect(const Rect& r, Color c, float thickness) {
    scene_->ops.emplace_back(StrokeRectOp{r, c, thickness});
    if (draw_ != nullptr) draw_->strokeRect(r, c, thickness);
}

void RecordingSink::fillCircle(float cx, float cy, float radius, Color c) {
    scene_->ops.emplace_back(FillCircleOp{cx, cy, radius, c});
    if (draw_ != nullptr) draw_->fillCircle(cx, cy, radius, c);
}

void RecordingSink::strokeCircle(float cx, float cy, float radius, Color c, float thickness) {
    scene_->ops.emplace_back(StrokeCircleOp{cx, cy, radius, c, thickness});
    if (draw_ != nullptr) draw_->strokeCircle(cx, cy, radius, c, thickness);
}

void RecordingSink::fillTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
                                 Color c) {
    FillTriangleOp op;
    op.p[0] = x0;
    op.p[1] = y0;
    op.p[2] = x1;
    op.p[3] = y1;
    op.p[4] = x2;
    op.p[5] = y2;
    op.color = c;
    scene_->ops.emplace_back(std::move(op));
    if (draw_ != nullptr) draw_->fillTriangle(x0, y0, x1, y1, x2, y2, c);
}

void RecordingSink::strokePolyline(const float* pts, int count, Color c, float width) {
    StrokePolylineOp op;
    if (pts != nullptr && count > 0) op.pts.assign(pts, pts + count * 2);
    op.color = c;
    op.width = width;
    scene_->ops.emplace_back(std::move(op));
    if (draw_ != nullptr) draw_->strokePolyline(pts, count, c, width);
}

void RecordingSink::drawGlyph(hb_font_t* font, uint32_t glyph_id, float pen_x, float pen_y,
                              float units_to_px, Color color, float rotate_deg) {
    scene_->ops.emplace_back(
        DrawGlyphOp{font, glyph_id, pen_x, pen_y, units_to_px, color, rotate_deg});
    if (draw_ != nullptr)
        draw_->drawGlyph(font, glyph_id, pen_x, pen_y, units_to_px, color, rotate_deg);
}

void RecordingSink::drawGlyphs(hb_font_t* font, const uint32_t* ids, const float* xs,
                               const float* ys, int count, float units_to_px, Color color,
                               float rotate_deg, bool italic) {
    DrawGlyphsOp op;
    op.font = font;
    op.units_to_px = units_to_px;
    op.color = color;
    op.rotate_deg = rotate_deg;
    op.italic = italic;
    if (ids != nullptr && xs != nullptr && ys != nullptr && count > 0) {
        op.ids.assign(ids, ids + count);
        op.xs.assign(xs, xs + count);
        op.ys.assign(ys, ys + count);
    }
    scene_->ops.emplace_back(std::move(op));
    if (draw_ != nullptr)
        draw_->drawGlyphs(font, ids, xs, ys, count, units_to_px, color, rotate_deg, italic);
}

void RecordingSink::drawImage(uint64_t key, uint32_t version, const uint8_t* rgba, int src_w,
                              int src_h, const Rect& dst) {
    DrawImageOp op;
    op.key = key;
    op.version = version;
    op.rgba = rgba;
    op.src_w = src_w;
    op.src_h = src_h;
    op.dst = dst;
    scene_->ops.emplace_back(std::move(op));
    if (draw_ != nullptr) draw_->drawImage(key, version, rgba, src_w, src_h, dst);
}

void RecordingSink::pushClipRect(const Rect& r) {
    scene_->ops.emplace_back(PushClipOp{r});
    ++mark_;
    if (draw_ != nullptr) draw_->pushClipRect(r);
}

void RecordingSink::restoreToMark(int mark) {
    scene_->ops.emplace_back(RestoreMarkOp{mark});
    mark_ = mark;
    if (draw_ != nullptr) draw_->restoreToMark(mark);
}

}  // namespace heditor
