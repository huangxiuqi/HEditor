/**
 * 绘制接收端：页绘制代码只认这一组操作，不直接认 Bitmap。
 *
 * 两个实现：BitmapSink 原样转发给位图；RecordingSink 把调用记成 PageScene::ops，
 * 需要时再重放。这样同一份 drawPage 既能直接画，也能生成显示列表。
 */
#pragma once

#include <cstdint>

#include "core/render/bitmap.h"
#include "core/utils/types.h"

struct hb_font_t;

namespace heditor {

/**
 * 页绘制用的最小操作集：位图与记录器都实现它。
 */
class PageSink {
public:

    virtual ~PageSink() = default;

    /**
     * 目标宽度（设备像素）。
     * @returns 宽度
     */
    virtual int width() const = 0;

    /**
     * 目标高度（设备像素）。
     * @returns 高度
     */
    virtual int height() const = 0;

    /**
     * 实心矩形。
     * @param r 目标矩形
     * @param c 颜色
     */
    virtual void fillRect(const Rect& r, Color c) = 0;

    /**
     * 矩形描边。
     * @param r 目标矩形
     * @param c 颜色
     * @param thickness 线宽
     */
    virtual void strokeRect(const Rect& r, Color c, float thickness) = 0;

    /**
     * 实心圆。
     * @param cx 圆心 x
     * @param cy 圆心 y
     * @param radius 半径
     * @param c 颜色
     */
    virtual void fillCircle(float cx, float cy, float radius, Color c) = 0;

    /**
     * 圆环。
     * @param cx 圆心 x
     * @param cy 圆心 y
     * @param radius 半径
     * @param c 颜色
     * @param thickness 线宽
     */
    virtual void strokeCircle(float cx, float cy, float radius, Color c, float thickness) = 0;

    /**
     * 实心三角形。
     * @param x0 顶点 0 的 x
     * @param y0 顶点 0 的 y
     * @param x1 顶点 1 的 x
     * @param y1 顶点 1 的 y
     * @param x2 顶点 2 的 x
     * @param y2 顶点 2 的 y
     * @param c 颜色
     */
    virtual void fillTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
                              Color c) = 0;

    /**
     * 折线。
     * @param pts 顶点数组（x0,y0,x1,y1,...）
     * @param count 顶点个数
     * @param c 颜色
     * @param width 线宽
     */
    virtual void strokePolyline(const float* pts, int count, Color c, float width) = 0;

    /**
     * 单个字形。
     * @param font 字体
     * @param glyph_id 字形下标
     * @param pen_x 原点 x
     * @param pen_y 原点 y
     * @param units_to_px 单位换算
     * @param color 颜色
     * @param rotate_deg 旋转角度
     */
    virtual void drawGlyph(hb_font_t* font, uint32_t glyph_id, float pen_x, float pen_y,
                           float units_to_px, Color color, float rotate_deg = 0.0f) = 0;

    /**
     * 一批同字体/同色/同朝向的字形。
     * @param font 字体
     * @param ids 字形下标
     * @param xs 原点 x
     * @param ys 原点 y
     * @param count 个数
     * @param units_to_px 单位换算
     * @param color 颜色
     * @param rotate_deg 旋转角度
     * @param italic 是否合成倾斜
     */
    virtual void drawGlyphs(hb_font_t* font, const uint32_t* ids, const float* xs, const float* ys,
                            int count, float units_to_px, Color color, float rotate_deg = 0.0f,
                            bool italic = false) = 0;

    /**
     * 一张图片。
     * @param key 资源身份
     * @param version 资源版本
     * @param rgba 预乘像素
     * @param src_w 源宽
     * @param src_h 源高
     * @param dst 目标矩形
     */
    virtual void drawImage(uint64_t key, uint32_t version, const uint8_t* rgba, int src_w,
                           int src_h, const Rect& dst) = 0;

    /**
     * 压入一个裁剪矩形。
     * @param r 裁剪矩形
     */
    virtual void pushClipRect(const Rect& r) = 0;

    /**
     * 当前裁剪栈标记。
     * @returns 标记
     */
    virtual int clipMark() const = 0;

    /**
     * 退回裁剪栈标记。
     * @param mark 标记
     */
    virtual void restoreToMark(int mark) = 0;
};

/**
 * 直接画到位图的接收端。
 */
class BitmapSink final : public PageSink {
public:

    /**
     * 绑定目标位图。
     * @param b 位图
     */
    explicit BitmapSink(Bitmap& b) : b_(b) {}

    /**
     * 转发宽度。
     * @returns 宽度
     */
    int width() const override { return b_.width(); }

    /**
     * 转发高度。
     * @returns 高度
     */
    int height() const override { return b_.height(); }

    /**
     * 转发实心矩形。
     * @param r 矩形
     * @param c 颜色
     */
    void fillRect(const Rect& r, Color c) override { b_.fillRect(r, c); }

    /**
     * 转发矩形描边。
     * @param r 矩形
     * @param c 颜色
     * @param thickness 线宽
     */
    void strokeRect(const Rect& r, Color c, float thickness) override {
        b_.strokeRect(r, c, thickness);
    }

    /**
     * 转发实心圆。
     * @param cx 圆心 x
     * @param cy 圆心 y
     * @param radius 半径
     * @param c 颜色
     */
    void fillCircle(float cx, float cy, float radius, Color c) override {
        b_.fillCircle(cx, cy, radius, c);
    }

    /**
     * 转发圆环。
     * @param cx 圆心 x
     * @param cy 圆心 y
     * @param radius 半径
     * @param c 颜色
     * @param thickness 线宽
     */
    void strokeCircle(float cx, float cy, float radius, Color c, float thickness) override {
        b_.strokeCircle(cx, cy, radius, c, thickness);
    }

    /**
     * 转发三角形。
     * @param x0 顶点 0 x
     * @param y0 顶点 0 y
     * @param x1 顶点 1 x
     * @param y1 顶点 1 y
     * @param x2 顶点 2 x
     * @param y2 顶点 2 y
     * @param c 颜色
     */
    void fillTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
                      Color c) override {
        b_.fillTriangle(x0, y0, x1, y1, x2, y2, c);
    }

    /**
     * 转发折线。
     * @param pts 顶点
     * @param count 个数
     * @param c 颜色
     * @param width 线宽
     */
    void strokePolyline(const float* pts, int count, Color c, float width) override {
        b_.strokePolyline(pts, count, c, width);
    }

    /**
     * 转发单字形。
     * @param font 字体
     * @param glyph_id 字形下标
     * @param pen_x 原点 x
     * @param pen_y 原点 y
     * @param units_to_px 单位换算
     * @param color 颜色
     * @param rotate_deg 旋转角度
     */
    void drawGlyph(hb_font_t* font, uint32_t glyph_id, float pen_x, float pen_y, float units_to_px,
                   Color color, float rotate_deg) override {
        b_.drawGlyph(font, glyph_id, pen_x, pen_y, units_to_px, color, rotate_deg);
    }

    /**
     * 转发一批字形。
     * @param font 字体
     * @param ids 字形下标
     * @param xs 原点 x
     * @param ys 原点 y
     * @param count 个数
     * @param units_to_px 单位换算
     * @param color 颜色
     * @param rotate_deg 旋转角度
     * @param italic 是否倾斜
     */
    void drawGlyphs(hb_font_t* font, const uint32_t* ids, const float* xs, const float* ys,
                    int count, float units_to_px, Color color, float rotate_deg,
                    bool italic) override {
        b_.drawGlyphs(font, ids, xs, ys, count, units_to_px, color, rotate_deg, italic);
    }

    /**
     * 转发图片。
     * @param key 资源身份
     * @param version 资源版本
     * @param rgba 像素
     * @param src_w 源宽
     * @param src_h 源高
     * @param dst 目标矩形
     */
    void drawImage(uint64_t key, uint32_t version, const uint8_t* rgba, int src_w, int src_h,
                   const Rect& dst) override {
        b_.drawImage(key, version, rgba, src_w, src_h, dst);
    }

    /**
     * 转发裁剪矩形。
     * @param r 裁剪矩形
     */
    void pushClipRect(const Rect& r) override { b_.pushClipRect(r); }

    /**
     * 转发裁剪标记。
     * @returns 标记
     */
    int clipMark() const override { return b_.clipMark(); }

    /**
     * 转发裁剪恢复。
     * @param mark 标记
     */
    void restoreToMark(int mark) override { b_.restoreToMark(mark); }

private:

    /**
     * 目标位图（不持有）。
     */
    Bitmap& b_;
};

}  // namespace heditor
