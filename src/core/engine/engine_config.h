/**
 * 引擎配置：宿主能改的那些开关。
 *
 * 与 engine_stats.h 分开的理由是方向相反 —— 这一个是从宿主流向引擎的输入，
 * 那一个是从引擎流向宿主的度量。两者的读者与变更理由都不一样。
 */
#pragma once

#include <cstdint>

#include "core/layout/layout.h"
#include "core/utils/types.h"

namespace heditor {

struct EngineConfig {
    PageMetrics page = PageMetrics::A4();
    /**
     * Tab 的缩进宽度（空格数）。
     *
     * Tab 在编辑器里表示缩进，不表示跳字段（跳字段是表单/网页的约定）；字段之间用
     * 鼠标点。用空格而非 U+0009：行盒只认簇的 advance，制表符会变成零宽度字符。
     *
     * 模型里预留了制表符原子（NodeKind::Tab，导出文本取 '\t'），但排版层没有制表位
     * —— 行盒推进、命中测试、光标落点都不认它，所以文档模式的 Tab 也走这里插空格。
     * 要做"Tab 插一个真的制表符"，得先给排版层加制表位。
     */
    int tab_spaces = 4;

    /**
     * 撤销栈的深度上限（条数）。0 表示不限制（内存自己扛）。
     *
     * 默认 100：与 Office 系（Word / Excel / PowerPoint 默认保存最近 100 个可撤销操作）和
     * 网页富文本里最常见的 Quill（history.maxStack 默认 100）同值。超出时丢最旧的 ——
     * 与 Quill 同一条口径。
     *
     * 粒度比它们细：一笔编辑一步、不做时间合并（Quill 另有 1 秒的合并窗口），所以同样 100 条
     * 在这里覆盖的输入更短，嫌少就把它调大。
     */
    int undo_limit = 100;

    float zoom = 2.0f;  // 设备像素 / 磅。2.0 ≈ 144dpi
    float page_gap_pt = 14.0f;
    /**
     * 连页模式：排版不分页，内容一列连到底。
     *
     * 它改的是排版的形状（页底断行、显式分页符、竖排吃页尾），所以必须重排；
     *   引擎把它与页度量一起装配成 PageMetrics 交给排版层（见 setConfig）。
     * 宿主配合的两件事：页间隙设成 0（条带首尾相接）、滚动高度取内容总高而不是页数乘步距。
     */
    bool continuous = false;
    Color background = Color::rgb(238, 240, 243);
    Color page_fill = Color::rgb(255, 255, 255);
    Color page_border = Color::rgb(206, 212, 218);
    Color caret_color = Color::rgb(31, 111, 235);
    /**
     * 选区底色。alpha 故意不是 255：它盖在文字上，实心填充会把字一起抹掉。
     * 混过之后白底大约落在浅蓝那一带、字仍是深色 —— 这才是"选中"该有的样子。
     */
    Color selection_color = Color{178, 214, 255, 90};
    float caret_width_pt = 1.4f;
    bool draw_page_border = true;
    /**
     * 表格格线，纯黑。宽度固定为一个设备像素（见绘制处）—— 线宽不做成配置项：
     * 它一旦跟着缩放变粗，跨页拼接处的两条半像素线就会显出来。
     */
    Color table_line = Color::rgb(0, 0, 0);

    /**
     * 表单控件标记。不画底色也不画边框：控件内容就是普通正文，加底纹会让它看起来像
     * 另一种东西。括号与光标无关：要么所有控件都显示（默认），要么一个都不显示 ——
     * 不做"只显示光标所在字段"那一档，它会让跨字段移动光标变成整行重排。只读字段用灰色。
     *
     * 括号用字体里的字形（'[' / ']'），没有"线宽""臂长"这类参数：粗细与形状跟着正文
     * 字体走；也没有"间距"参数 —— 间隙由排版给（控件边界占一个括号宽度，见 layout.cpp
     * 的占位簇），不由绘制层补。
     *
     * 标记画在帧上而非底图（它是呈现态，跟编辑模式走）；5D 之后搬进页内容位图。
     */
    bool show_brackets = true;
    Color control_mark = Color::rgb(56, 139, 253);      // 可编辑：亮蓝
    Color control_mark_ro = Color::rgb(168, 168, 168);  // 只读：灰（否则看着能改）

    /**
     * 小点：控件右下角的一个小方块。
     *
     * 独立开关，与括号显不显示无关。开启时所有控件都画，不区分焦点 —— 它只表示
     * "这里有一个字段"，不表示"光标不在这里"。尺寸是磅值，跟着缩放走。
     */
    bool show_control_dots = false;
    float control_dot_pt = 3.0f;  // 小点边长（磅）

    /**
     * 行内原子的占位框。
     *
     * 图片与公式本期都画框、不画内容（见 image_spec.h / formula_spec.h）：图片的字节还没
     * 进来，公式的排版后端还没做，把表达式按普通文字画出来会得到错误的字距与上下标。
     * 框里写上它是什么（资源标识 / 表达式，放不下就截断），读者一眼看得出这里是一枚原子。
     *
     * 底色调得很浅：它是页面内容的一部分（画在底图里），重了会盖过正文的灰度。
     */
    Color atom_fill = Color::rgb(246, 248, 250);
    Color atom_border = Color::rgb(174, 186, 199);
    Color atom_text = Color::rgb(110, 122, 135);
    /**
     * 框里文字左右各留的边距（磅）。
     */
    float atom_pad_pt = 2.0f;
};

}  // namespace heditor
