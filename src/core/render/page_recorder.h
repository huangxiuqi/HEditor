/**
 * 把页绘制调用记成 PageScene::ops，同时（可选）转发给位图。
 *
 * 记录与绘制共用同一份 drawPage：一次调用既画出像素，也留下可重放的指令。
 * 记录器从不解释几何，只保存调用参数。
 */
#pragma once

#include "core/engine/page_scene.h"
#include "core/render/page_sink.h"

namespace heditor {

/**
 * 记录型绘制接收端。
 */
class RecordingSink final : public PageSink {
public:

    /**
     * 绑定场景与可选位图。
     * @param scene 记录目标（不持有）
     * @param w 目标宽度（设备像素）
     * @param h 目标高度（设备像素）
     * @param draw 同时绘制的位图；nullptr 表示只记录
     */
    RecordingSink(PageScene* scene, int w, int h, Bitmap* draw = nullptr);

    /**
     * 记录宽度。
     * @returns 宽度
     */
    int width() const override { return w_; }

    /**
     * 记录高度。
     * @returns 高度
     */
    int height() const override { return h_; }

    /**
     * 记录实心矩形。
     * @param r 矩形
     * @param c 颜色
     */
    void fillRect(const Rect& r, Color c) override;

    /**
     * 记录矩形描边。
     * @param r 矩形
     * @param c 颜色
     * @param thickness 线宽
     */
    void strokeRect(const Rect& r, Color c, float thickness) override;

    /**
     * 记录实心圆。
     * @param cx 圆心 x
     * @param cy 圆心 y
     * @param radius 半径
     * @param c 颜色
     */
    void fillCircle(float cx, float cy, float radius, Color c) override;

    /**
     * 记录圆环。
     * @param cx 圆心 x
     * @param cy 圆心 y
     * @param radius 半径
     * @param c 颜色
     * @param thickness 线宽
     */
    void strokeCircle(float cx, float cy, float radius, Color c, float thickness) override;

    /**
     * 记录三角形。
     * @param x0 顶点 0 x
     * @param y0 顶点 0 y
     * @param x1 顶点 1 x
     * @param y1 顶点 1 y
     * @param x2 顶点 2 x
     * @param y2 顶点 2 y
     * @param c 颜色
     */
    void fillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) override;

    /**
     * 记录折线。
     * @param pts 顶点
     * @param count 个数
     * @param c 颜色
     * @param width 线宽
     */
    void strokePolyline(const float* pts, int count, Color c, float width) override;

    /**
     * 记录单字形。
     * @param font 字体
     * @param glyph_id 字形下标
     * @param pen_x 原点 x
     * @param pen_y 原点 y
     * @param units_to_px 单位换算
     * @param color 颜色
     * @param rotate_deg 旋转角度
     */
    void drawGlyph(hb_font_t* font, uint32_t glyph_id, float pen_x, float pen_y, float units_to_px,
                   Color color, float rotate_deg) override;

    /**
     * 记录一批字形。
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
                    bool italic) override;

    /**
     * 记录图片。
     * @param key 资源身份
     * @param version 资源版本
     * @param rgba 像素
     * @param src_w 源宽
     * @param src_h 源高
     * @param dst 目标矩形
     */
    void drawImage(uint64_t key, uint32_t version, const uint8_t* rgba, int src_w, int src_h,
                   const Rect& dst) override;

    /**
     * 记录裁剪矩形。
     * @param r 裁剪矩形
     */
    void pushClipRect(const Rect& r) override;

    /**
     * 记录裁剪标记。
     * @returns 标记
     */
    int clipMark() const override { return mark_; }

    /**
     * 记录裁剪恢复。
     * @param mark 标记
     */
    void restoreToMark(int mark) override;

private:

    /**
     * 记录目标（不持有）。
     */
    PageScene* scene_ = nullptr;

    /**
     * 同时绘制的位图；nullptr = 只记录。
     */
    Bitmap* draw_ = nullptr;

    /**
     * 目标宽度。
     */
    int w_ = 0;

    /**
     * 目标高度。
     */
    int h_ = 0;

    /**
     * 记录的裁剪栈深度。
     */
    int mark_ = 1;
};

}  // namespace heditor
