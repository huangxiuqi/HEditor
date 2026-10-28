/**
 * 块分页：把块局部的断行/断列结果落到页上。
 *
 * 三条规则各自独立：横排按行高逐行翻页（planBlockLines）、横排带浮动图时按排除带驱动
 * 断行（planBlockLinesWithFloats）、竖排把每条竖带映射成页（planBlockColumns）。
 * 它们共用同一个结果类型 BlockPlan —— 消费方只认"每行的片段、行顶、页号、行号"，
 * 于是整篇循环里落位只有一处。
 *
 * 本层只决定每行落在哪一页、行槽顶在哪。行盒与字形仍由 block_shape 的 place* 写：
 * 行顶由本层递推，行盒从行顶派生 —— 行顶只能有一处累加，两处各加一次会在末位分叉。
 */
#pragma once

#include <vector>

#include "core/layout/block_layout.h"
#include "core/layout/block_shape.h"
#include "core/layout/float_flow.h"

namespace heditor {

/**
 * 分页输入：三条规则共用。
 *
 * 规则用不到的字段留默认值即可。格内块把 no_page_break 置真、段前分页与段前距留在 0/
 * false —— 那两项由整篇循环在块首处理（表格行的位置由表决定，不由段前分页决定）。
 */
struct BlockPlanInput {
    /**
     * 起始页号。
     */
    int page_index = 0;

    /**
     * 块开始时的全局行号；行号按它续。
     */
    int line_total = 0;

    /**
     * 起始块轴 y（磅）：分页下是页内 y，连页下是绝对 y。
     */
    float cursor_y_pt = 0.0f;

    /**
     * 版心上沿（磅），也是翻页后行槽顶的落点。
     */
    float content_top_pt = 0.0f;

    /**
     * 版心下沿（磅）：行槽底越过它就翻页。
     */
    float content_bottom_pt = 0.0f;

    /**
     * 内容区左沿（页内 x，磅）：不绕排时每一行的起点。
     */
    float content_x_pt = 0.0f;

    /**
     * 内容区宽度（磅）。
     */
    float content_width_pt = 0.0f;

    /**
     * 连页：不在页底断开，页号退化成条带号。
     */
    bool continuous = false;

    /**
     * 连页条带高（磅），必须为正；continuous 为 false 时忽略。
     */
    float slab_pt = 0.0f;

    /**
     * 页步距（磅）：分页下是页高，连页下是条带高。块轴 y = 页号乘它加页内 y。
     */
    float page_stride_pt = 0.0f;

    /**
     * 不按版心底翻页：格内块用。格内不翻页，越出版心底的行由表格行切片处理。
     */
    bool no_page_break = false;

    /**
     * 段前分页：块首不在版心顶时先翻一页。
     */
    bool page_break_before = false;

    /**
     * 段前距（磅）。页顶那一段不加。
     */
    float space_before_pt = 0.0f;

    /**
     * 段后距（磅）。只进结束游标，不参与分页。
     */
    float space_after_pt = 0.0f;

    /**
     * 段行高估计（磅）：浮动图规则用它预判翻页与上下型下推，不用本行真实行高。
     */
    float base_line_h_pt = 0.0f;

    /**
     * 段首行未被推走时的块轴 y（磅）；浮动图按它落位。
     */
    float anchor_flow_pt = 0.0f;

    /**
     * 本段落下的排除带在 FloatFlow 里的起始下标；段首行被翻页推走时只平移这一段。
     */
    size_t live_base = 0;

    /**
     * 块布局在块首另起了一条竖带（旧循环在这里翻一页）：首带落在下一页。
     */
    bool started_new_band = false;

    /**
     * 带高（磅）：连页下竖排块尾游标 = 最后一列的列顶 + 它 + 段后距。
     */
    float band_height_pt = 0.0f;
};

/**
 * 一行在页上的落点。竖排的一列也走这个结构：列心进 parts[0].x，列宽就是片段的 line_h。
 */
struct BlockPlanLine {
    /**
     * 本行的片段，按从左到右；被四周型图挖开时有两段，竖排恒为一段。
     */
    std::vector<LinePart> parts;

    /**
     * 行槽顶（磅）：分页下是页内 y，连页下是块轴绝对 y。
     */
    float top_pt = 0.0f;

    /**
     * 页号；连页下是条带号。
     */
    int page = 0;

    /**
     * 全局行号（竖排下是列号），落位时写进行盒。
     */
    int line_no = 0;
};

/**
 * 分页结果。
 */
struct BlockPlan {
    /**
     * 每行一个落点，按行序。放不出一段的行不在这里（与旧循环一致：直接结束本块）。
     */
    std::vector<BlockPlanLine> lines;

    /**
     * 结束页号，可直接作为下一块的起始页号。
     */
    int page_index = 0;

    /**
     * 结束时的全局行号，可直接作为下一块的起始行号。
     */
    int line_total = 0;

    /**
     * 结束块轴 y（磅）。
     */
    float cursor_y_pt = 0.0f;

    /**
     * 段首行被翻页推走时浮动图要跟着平移的量（磅）；没平移为 0。
     */
    float first_line_shift_pt = 0.0f;
};

/**
 * 把块局部的断行片段排到页上（不绕排）。
 *
 * 段前分页先于段前距；页顶那一段不加段前距；行槽底越过版心下沿才翻页。一行最多重试
 * 一次：比整页还高的一行两次判据都不通过，页号会连进两格，与旧循环一致。
 *
 * @param fragments 块局部断行片段，按行序；只用它们的 line_h
 * @param in 页状态与段间间距；content_x_pt 决定每行片段的起点
 * @return 每行的片段/行顶/页号，以及结束状态
 */
BlockPlan planBlockLines(const std::vector<BrokenLine>& fragments, const BlockPlanInput& in);

/**
 * 排一个带浮动图排除带的横排块。
 *
 * 可用宽度取决于行落点，行落点又取决于前面翻没翻页，所以由本层驱动断行：每行按落点处
 * 的排除带取区间，再交给 fillLineParts。排除带、上下型下推与四周型让宽复用 FloatFlow。
 * 格内块也走这一条：no_page_break 置真时页号恒为起始页。
 *
 * @param block 要排的块；只读它的字号与行距
 * @param shaped 插好占位簇的整形结果
 * @param cluster_metric 与 shaped.clusters 等长的行度量
 * @param flow 生效中的排除带（就地修改）；为空表示不绕排
 * @param in 页状态、内容区几何与段首锚点
 * @return 每行的片段与落点，以及结束页号、游标与段首平移量
 */
BlockPlan planBlockLinesWithFloats(const BlockSpec& block, const BlockShaping& shaped,
                                   const std::vector<LineMetric>& cluster_metric, FloatFlow* flow,
                                   const BlockPlanInput& in);

/**
 * 把竖排块的列分到页上。
 *
 * 列断在哪里已经由块局部排版按竖带定下；这里只回答每条带落在哪一页、列号从哪续起，
 * 以及块尾的页号与游标。分页下页号随竖带递增，同一带里的列共用一个页号；块首那条带
 * 若是块布局另起的，它落在下一页。连页下页号由列顶与条带高现算，结束页号原样回传。
 * 排除带不是这一层的输入：竖排列不受浮动图约束，编译层也不给竖排浮动图。
 *
 * @param columns 块局部排版给出的列，按列序；非空块至少有一列
 * @param in 页状态与段后距
 * @return 每列的落点，以及结束状态
 */
BlockPlan planBlockColumns(const std::vector<BlockColumn>& columns, const BlockPlanInput& in);

}  // namespace heditor
