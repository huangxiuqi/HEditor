/**
 * 一页的光栅化：把排版结果画成一页位图。
 *
 * 这个单元是"呈现"的全部：底色、页边框、裁剪标记、内容（字形 / 表格 / 原子 / 浮动图）、
 * 水印、编辑标记、控件标记，按层次叠在一张页位图上。它不认帧、不认窗口 —— 页位图不知道
 * 自己会摆到画布的哪一页上，这正是"窗口平移不产生像素工作"的来源。
 *
 * 之所以独立成文件：这段原先长在 EditorEngine 上，读十来个私有成员，于是"画一页"必须有
 * 一个引擎实例。改成吃 RenderInput 之后，它可以在不建引擎的情况下单独验（一张位图、一份
 * 输入、一个页号）。引擎那一侧只剩"填输入、给位图、记账"。
 */
#pragma once

#include "core/engine/render_input.h"
#include "core/render/bitmap.h"

namespace heditor {

/**
 * 一次页光栅的分项耗时（毫秒）。只记账，不进像素。
 */
struct PageRasterCost {
    /**
     * 铺面积（底色、边框、裁剪标记、表格格线）花掉的毫秒。
     */
    double fill_ms = 0.0;

    /**
     * 画字形、原子、图片与各处标记花掉的毫秒。
     */
    double glyph_ms = 0.0;
};

/**
 * 把一页画进 @p target（坐标全部页内化，原点 = 页左上角）。
 *
 * band 非空时只画一条横带，带外像素保持原样 —— 前提是调用方保证带外已经有一份同一页、
 * 同一光栅配置的完整像素。分带重画是"只改了一行、却要重画整页"的解法：行带之内重画，
 * 行带之外不动。
 *
 * 连页模式的接缝那一趟（spill）由本函数自己补：一条带的下边界可能切开一行，那半行属于
 * 下一页位图的顶端，所以要多画一遍、整体上移一个带高。
 *
 * @param target 目标位图（尺寸必须已是页像素尺寸）
 * @param in 绘制输入
 * @param page_index 页号
 * @param band_lo_pt 带下界（磅），可为空
 * @param band_hi_pt 带上界（磅），可为空
 * @param cost 分项耗时累加到这里（可为空）
 */
void drawPage(Bitmap& target, const RenderInput& in, int page_index, const float* band_lo_pt,
              const float* band_hi_pt, PageRasterCost* cost);

}  // namespace heditor
