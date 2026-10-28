/**
 * 简单块的分页：把块局部断行片段落到页上。
 *
 * 横排只处理非表格、块自身没有浮动原子的块：断行时可用宽度恒定，一行恰好一个片段，
 * 所以分页只依赖每行的高度与页度量，不依赖字形；绕排的横排块见
 * paginateBlockLinesWithFloats。竖排见 paginateBlockColumns：列内容由块局部排版按竖带
 * 断好，分页只回答每条带落在哪一页。表格格内不走这一层，它们的页内位置还与格高有关。
 * 浮动图的排除带只作用于横排：竖排里的浮动图由编译层退回行内，竖排列不读排除带。
 *
 * 本层只决定每行落在哪一页、行槽顶在哪。行盒与字形仍由 layout.cpp 的
 * placeHorizontalLine 写：页内坐标的加法次序不能在这里重排，否则逐位摘要会变。
 */
#pragma once

#include <vector>

#include "core/layout/block_layout.h"
#include "core/layout/block_shape.h"
#include "core/layout/float_flow.h"

namespace heditor {

/**
 * 简单块分页用的页状态与段间间距。
 */
struct BlockPaginationInput {
    /**
     * 起始页号。
     */
    int page_index = 0;

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
     * 连页：不在页底断开，页号退化成条带号。
     */
    bool continuous = false;

    /**
     * 连页条带高（磅），必须为正；continuous 为 false 时忽略。
     */
    float slab_pt = 0.0f;

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
};

/**
 * 一行在页上的落点。
 */
struct BlockPaginationLine {
    /**
     * 页号；连页下是条带号，由行槽顶现算。
     */
    int page = 0;

    /**
     * 行槽顶（块轴 y，磅）。分页下就是页内 y；连页下是绝对 y，绘制前按页号减条带偏移。
     */
    float top_pt = 0.0f;
};

/**
 * 简单块分页的结果。
 */
struct BlockPaginationResult {
    /**
     * 每行一个落点，与输入的断行片段一一对应。
     */
    std::vector<BlockPaginationLine> lines;

    /**
     * 结束页号，可直接作为下一块的起始页号。连页下原样回传入参：条带号由结束游标现算，
     * 不作为分页状态。
     */
    int page_index = 0;

    /**
     * 结束块轴 y（磅）：最后一行行槽顶 + 行高 + 段后距。
     */
    float cursor_y_pt = 0.0f;
};

/**
 * 把简单横排块的断行片段排到页上。
 *
 * 段前分页先于段前距；页顶那一段不加段前距；行槽底越过版心下沿才翻页。一行最多重试
 * 一次：比整页还高的一行两次判据都不通过，页号会连进两格，与整篇循环一致。
 *
 * @param fragments 块局部断行片段，按行序；分页只读 line_h
 * @param in 页状态与段间间距
 * @return 每行的页号与行槽顶，以及结束状态
 */
BlockPaginationResult paginateBlockLines(const std::vector<BrokenLine>& fragments,
                                         const BlockPaginationInput& in);

/**
 * 带浮动图排除带的块分页输入。
 *
 * 段前分页与段前距已经由调用方算进 page_index 与 cursor_y_pt：浮动图按段首行顶落位，
 * 落位之前那两项就必须生效。
 */
struct BlockFloatPaginationInput {
    /**
     * 起始页号。
     */
    int page_index = 0;

    /**
     * 起始行顶（磅）：分页下是页内 y，连页下是块轴绝对 y。
     */
    float cursor_y_pt = 0.0f;

    /**
     * 版心上沿（磅，页内坐标），翻页后行顶的落点。
     */
    float content_top_pt = 0.0f;

    /**
     * 版心下沿（磅，页内坐标）。
     */
    float content_bottom_pt = 0.0f;

    /**
     * 内容区左沿（页内 x，磅）。
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
     * 不按版心底翻页：格内块用。格内不翻页，越出版心底的行由表格行切片处理；此时
     * content_bottom_pt 不参与判定，页号恒为 page_index。
     */
    bool no_page_break = false;

    /**
     * 连页条带高（磅）；continuous 为 true 时必须为正。
     */
    float slab_pt = 0.0f;

    /**
     * 页步距（磅）：分页下是页高，连页下是条带高。块轴 y = 页号乘它加页内 y。
     */
    float page_stride_pt = 0.0f;

    /**
     * 段行高估计（磅）：旧循环用它预判翻页与上下型下推，不用本行真实行高。
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
     * 段后距（磅）：只进结束游标。
     */
    float space_after_pt = 0.0f;
};

/**
 * 带浮动图的一行：本行的片段、行顶与页号。
 */
struct BlockFloatPaginationLine {
    /**
     * 本行的片段，按从左到右；每段带它在页内的起点。被四周型图挖开时有两段。
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
};

/**
 * 带浮动图排除带的块分页结果。
 */
struct BlockFloatPaginationResult {
    /**
     * 每行一个落点，按行序；放不出一段的行不在这里（与旧循环一样直接结束本块）。
     */
    std::vector<BlockFloatPaginationLine> lines;

    /**
     * 结束页号，可直接作为下一块的起始页号。
     */
    int page_index = 0;

    /**
     * 结束块轴 y（磅）：最后一行行槽顶 + 行高 + 段后距。
     */
    float cursor_y_pt = 0.0f;

    /**
     * 段首行被翻页推走时浮动图要跟着平移的量（磅）；没平移时为 0。
     */
    float first_line_shift_pt = 0.0f;
};

/**
 * 排一个带浮动图排除带的简单横排块。
 *
 * 可用宽度取决于行落点，行落点又取决于前面翻没翻页，所以由分页层驱动断行：每行按落点处
 * 的排除带取区间，再交给 fillLineParts。排除带、上下型下推与四周型让宽复用 FloatFlow。
 * 格内块也走这一条，格内不翻页时页号恒为当前页。
 *
 * @param block 要排的块；只读它的字号与行距
 * @param shaped 插好占位簇的整形结果
 * @param cluster_metric 与 shaped.clusters 等长的行度量
 * @param flow 生效中的排除带（就地修改）
 * @param in 页状态、内容区几何与段首锚点
 * @return 每行的片段与落点，以及结束页号、游标与段首平移量
 */
BlockFloatPaginationResult paginateBlockLinesWithFloats(
    const BlockSpec& block, const BlockShaping& shaped,
    const std::vector<LineMetric>& cluster_metric, FloatFlow* flow,
    const BlockFloatPaginationInput& in);

/**
 * 竖排分页的输入：块布局之后的页状态。
 *
 * 段前分页与段前距不在这里：竖排断列依赖块首列顶，必须先由整篇循环把页首定下来，
 * 再用那个列顶排块，所以那两项在块布局之前就落到游标上了。
 */
struct BlockColumnPaginationInput {
    /**
     * 块开始时的页号（段前分页已经生效）。连页下原样回传。
     */
    int page_index = 0;

    /**
     * 块开始时的全局行号；列盒的 line 从这里续起。
     */
    int line_total = 0;

    /**
     * 版心下沿（磅，页内坐标）。块尾游标由它加段后距给出。
     */
    float content_bottom_pt = 0.0f;

    /**
     * 段后距（磅）。只进结束游标。
     */
    float space_after_pt = 0.0f;

    /**
     * 块布局在块首另起了一条竖带（旧循环在这里翻一页）：首带落在下一页。
     */
    bool started_new_band = false;

    /**
     * 连页：页号由列顶与 slab_pt 现算，结束页号原样回传入参。
     */
    bool continuous = false;

    /**
     * 连页条带高（磅），continuous 为 true 时必须为正。
     */
    float slab_pt = 0.0f;

    /**
     * 带高（磅）：连页下块尾游标 = 最后一列的列顶 + 它 + 段后距。分页下不用这一项
     * （块尾游标恒是版心下沿加段后距）。
     */
    float band_height_pt = 0.0f;
};

/**
 * 一列的落点。
 */
struct BlockColumnPaginationColumn {
    /**
     * 这一列落在第几页。连页下是列顶所在的条带号。
     */
    int page = 0;

    /**
     * 这一列在所在竖带里的下标；块起始行号加它就是列盒的 line。
     */
    int line = 0;

    /**
     * 列顶（磅）：分页下是页内 y，连页下是块轴绝对 y。
     */
    float top_pt = 0.0f;

    /**
     * 列心（页内 x，磅）。
     */
    float center_pt = 0.0f;

    /**
     * 列宽（磅）。
     */
    float width_pt = 0.0f;

    /**
     * 列内断行片段，落位时原样交给 placeVerticalColumn。
     */
    BrokenLine fragment;
};

/**
 * 简单竖排块的分页结果。
 */
struct BlockColumnPaginationResult {
    /**
     * 每列一个落点，与输入的列一一对应。
     */
    std::vector<BlockColumnPaginationColumn> columns;

    /**
     * 结束页号，可直接作为下一块的起始页号。连页下原样回传入参。
     */
    int page_index = 0;

    /**
     * 结束块轴 y（磅）：分页下是版心下沿加段后距；连页下是最后一列的列顶加带高再加段后距。
     */
    float cursor_y_pt = 0.0f;

    /**
     * 结束时的全局行号：最后一条竖带的列数接着块起始行号往下数。
     */
    int line_total = 0;
};

/**
 * 把简单竖排块的列分到页上。
 *
 * 列断在哪里已经由块局部排版按竖带定下；这里只回答每条带落在哪一页、列盒的行号从哪
 * 续起，以及块尾的页号与游标。分页下页号随竖带递增，同一带里的列共用一个页号；块首
 * 那条带若是块布局另起的，它落在下一页。连页下页号由列顶与条带高现算，结束页号原样
 * 回传。排除带不是这一层的输入：竖排列不受浮动图约束，编译层也不给竖排浮动图。
 *
 * @param columns 块局部排版给出的列，按列序；非空块至少有一列
 * @param in 页状态与段后距
 * @return 每列的页号/行号/落点，以及结束状态
 */
BlockColumnPaginationResult paginateBlockColumns(const std::vector<BlockColumn>& columns,
                                                 const BlockColumnPaginationInput& in);

}  // namespace heditor
