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

/**
 * 表单模式下哪些控件显示方括号。
 *
 * 括号既提示"这里可以输入"，又占宽度（进文档流，见 layout.h），所以显示范围决定
 * 排版成本：All 常显，占位集合与光标无关，光标移动不重排；Focused 只显示光标所在
 * 字段，代价是跨字段移动光标会重排整行。默认 All。
 */
enum class FormBracketScope : uint8_t {
    All = 0,      // 默认：所有控件一直显示括号
    Focused = 1,  // 只有光标所在字段显示括号，其余画小点
};

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
     * 表单控件标记。
     *
     * 不画底色也不画边框：控件内容就是普通正文，加底纹会让它看起来像另一种东西。
     * 括号常驻（form_brackets = All），光标只决定是否额外高亮；只读字段用灰色。
     * 标记画在帧上而非底图，因为要跟随光标状态，见 presentInternal。
     */
    FormBracketScope form_brackets = FormBracketScope::All;
    Color control_mark = Color::rgb(56, 139, 253);      // 可编辑：亮蓝
    Color control_mark_ro = Color::rgb(168, 168, 168);  // 只读：灰（否则看着能改）
    /**
     * 方括号用的是字体里的字形（'[' / ']'），所以没有"线宽""臂长"这类参数 ——
     * 粗细与形状跟着正文字体走，换字体/换字号都不用改配置。
     * 也没有"间距"参数：括号的间隙由排版给（控件边界会占一个括号宽度，
     * 见 layout.cpp 的占位簇），不再由绘制层补。
     */
    float control_dot_pt = 3.0f;  // 小点边长（磅）
};

}  // namespace heditor
