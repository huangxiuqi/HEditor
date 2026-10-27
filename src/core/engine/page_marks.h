/**
 * 页位图上的"非正文"图层：原子框、选择记号、水印、段落标记、控件括号。
 *
 * 这一族共同点：都不进流位置、不参与断行，只决定像素。它们原先挂在 EditorEngine 上，
 * 于是"画一页"必须有一个引擎实例；改成吃 RenderInput 之后，光栅本体可以只依赖这份输入。
 */
#pragma once

#include "core/engine/render_input.h"
#include "core/render/bitmap.h"

namespace heditor {

/**
 * 行内原子的显式占位框，或图片的真像素。
 *
 * 为什么画框而不是伪渲染：把表达式按普通文字画出来会得到错误的字距与上下标，比一个
 * 明确的框更危险。框里写上它是什么，读者一眼看得出这里是一枚原子；资源表里有这张图时
 * 直接贴像素。
 *
 * @param target 目标位图
 * @param in 绘制输入
 * @param lb 原子所在的行盒（基线、朝向、字体）
 * @param c 原子占的那一枚簇（宽高、锚点、种类）
 * @param a1_pt 原子盒的行内末端（磅）
 * @param off_y 纵向平移（设备像素，连页接缝补画用）
 */
void drawAtomBox(Bitmap& target, const RenderInput& in, const LineBox& lb, const LineCluster& c,
                 float a1_pt, float off_y);

/**
 * 这个字形是不是选择类控件的记号（方框 / 圆圈那一个码点）。
 *
 * 判的是"它正好落在某个选项的记号位置上"，不是"这个字长得像圆圈"：正文里打的 ○
 * 照旧由字体画，只有控件里那一枚才由引擎按记号色重画。
 *
 * @param in 绘制输入
 * @param pg 字形
 * @param checked 写回选中态
 * @param square 写回"方框还是圆圈"
 * @param color 写回记号色（alpha 0 = 跟随文字颜色）
 * @returns true = 是记号
 */
bool choiceMarkerOf(const RenderInput& in, const PlacedGlyph& pg, bool* checked, bool* square,
                    Color* color);

/**
 * 画一枚选择类记号：方框（选中时带对勾）或圆圈（选中时圆心一个实心点）。
 *
 * 两族都是自己画的，不走字体字形：字体里没有 ☑，而"方框里一个对勾"是这个控件唯一
 * 说得清的选中态；圆圈若借字体字形，它的直径取决于字体，与方框并排放着大小不一。
 *
 * @param target 页位图
 * @param in 绘制输入
 * @param pg 字形（位置、字号、推进）
 * @param checked 选中态
 * @param square true = 方框（复选），false = 圆圈（单选）
 * @param color 记号色（alpha 0 = 用字形自己的颜色）
 * @param dy 页内纵向偏移（设备像素）
 */
void paintChoiceMarker(Bitmap& target, const RenderInput& in, const PlacedGlyph& pg, bool checked,
                       bool square, Color color, float dy);

/**
 * 画水印：衬在正文下方的一行半透明文字，按节记录。
 *
 * 它不参与断行、不进流位置，所以只画进页位图；分带重画时按字形纵向位置筛。
 *
 * @param target 光栅目标（页位图）
 * @param in 绘制输入
 * @param page_index 页号（水印每页都有，页号只用于诊断）
 * @param band_lo_pt 带下界（磅），可为空
 * @param band_hi_pt 带上界（磅），可为空
 */
void drawWatermark(Bitmap& target, const RenderInput& in, int page_index, const float* band_lo_pt,
                   const float* band_hi_pt);

/**
 * 画不可见字符标记（段落标记、空格里的中点、换行符标记）。
 *
 * 只画进页位图、不占宽度：标记不是内容，改档位只让像素变，断行、光标落点、命中与
 * 流位置一个都不动 —— 这也是它可以随手开关的前提。几何从行盒派生：段落标记坐在
 * "这一段的最后一行"的行末，字体与基线取行盒的 bracket_font / bracket_axis_pt。
 *
 * @param target 页位图
 * @param in 绘制输入
 * @param page_index 页号
 * @param band_lo_pt 带下界（磅），可为空
 * @param band_hi_pt 带上界（磅），可为空
 * @param dy_px 整笔纵向平移（连页接缝补画那半截时是负的带高）
 * @param spill_only 只画越过带底的那几条行盒
 */
void drawEditingMarks(Bitmap& target, const RenderInput& in, int page_index,
                      const float* band_lo_pt, const float* band_hi_pt, float dy_px,
                      bool spill_only);

/**
 * 画表单标记（方括号、空字段提示、小点）到一页的位图上。
 *
 * 几何全部从排版的行盒派生（占位簇的位置 + 该簇的括号字体/行盒基线）：排版说占位在哪、
 * 用哪个字体，就画在哪 —— 空字段（一个字形都没有）也画得出来，它的几何是提示槽。
 *
 * band 非空时只画与这条带相交的行与控件 —— 与字形、原子框同一套裁剪口径。
 * dy_px 把整笔上移（连页接缝补画那半截时传一个负的带高）；spill_only 为真时只画越过
 * 带底的那几条行盒与控件 —— 与 drawPage 的接缝补画同一口径。
 *
 * @param target 页位图
 * @param in 绘制输入
 * @param page_index 页号
 * @param band_lo_pt 带下界（磅），可为空
 * @param band_hi_pt 带上界（磅），可为空
 * @param dy_px 整笔纵向平移（设备像素）
 * @param spill_only 只画越过带底的那几条
 */
void drawControlMarks(Bitmap& target, const RenderInput& in, int page_index,
                      const float* band_lo_pt, const float* band_hi_pt, float dy_px,
                      bool spill_only);

}  // namespace heditor
