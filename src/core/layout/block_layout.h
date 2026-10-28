/**
 * 块局部排版：与页无关的纯函数。
 *
 * 输入是一个块与它的排版约束，输出是块内坐标下的行盒与字形。分页、表格行高与浮动图都
 * 不在这一层：它们由整篇排版循环（layout.cpp）或后续的分页片段负责。横排格内块只有格宽
 * 从约束进来 —— 格内不翻页，行高由表格在块排完之后结算；竖排格内块的列长与列位取的是
 * 版心而不是格框，所以页的竖带由调用方显式传进约束。
 *
 * 横排的约束只有宽度，断行与页无关。竖排不同：一列排多长由页给，断列依赖竖带高，
 * 所以竖排必须把竖带（ColumnBand）显式传进来；产出的列坐标沿用竖带那把尺子。这是
 * 本层唯一的页形状输入，也是竖排拖到这一层才接的原因。
 *
 * 整篇排版循环与它共用同一份断行结果（layoutBlockFragments）：分页消费不含坐标的行片段，
 * 页内坐标仍由 layout.cpp 按原有加法次序重算 —— 块内坐标平移到页原点上会换掉浮点加法的
 * 结合顺序，逐位相等的 LayoutDigest 会变。
 */
#pragma once

#include <cstdint>
#include <vector>

#include "core/layout/block_shape.h"
#include "core/layout/layout.h"

namespace heditor {

/**
 * 竖排的竖带几何（页内坐标）。
 *
 * 竖排的一列能排多长由页高决定，块局部排版拿不到页，只能由调用方把带传进来。横排不用
 * 这一组。列宽逐列算，所以这里只给带的边界，不给列位。
 */
struct ColumnBand {
    /**
     * 竖带左沿（页内 x，磅）。
     */
    float left_pt = 0.0f;

    /**
     * 带下沿（磅）：列顶到它的距离就是一列的容量。连页下它是"版心上沿 + 版心高"这个
     * 绝对坐标，带顶一路下移而它不动，所以后续带的容量可以是负的（旧循环如此）。
     */
    float bottom_pt = 0.0f;

    /**
     * 块首那一列的列顶（页内 y，磅）。块从页中起排时它比换带后的列顶低。
     */
    float first_top_pt = 0.0f;

    /**
     * 换带之后各列的列顶（页内 y，磅）；分页下就是版心上沿。连页不用这一项：带顶从
     * first_top_pt 起逐带累加 band_step_pt。
     */
    float band_top_pt = 0.0f;

    /**
     * 后续每条带相对前一条带列顶的下移量（磅）。0 = 分页：后续带都从 band_top_pt 起排；
     * 正数 = 连页：带顶是块轴绝对坐标，逐带下移这么多。
     */
    float band_step_pt = 0.0f;

    /**
     * 连页：页号由列顶与 slab_pt 现算，列盒的 y 折回页内。分页为 false。
     */
    bool continuous = false;

    /**
     * 连页条带高（磅）：页号 = floor(列顶 / 它)。continuous 为 true 时必须为正。
     */
    float slab_pt = 0.0f;
};

/**
 * 块局部排版的约束：只描述这一块能排多宽、沿哪条书写轴，竖排另给竖带。
 *
 * 段前距与段后距不在这里 —— 它们属于块与块之间的分页关系，不是块自身的几何。
 */
struct BlockConstraints {
    /**
     * 块内容区宽度（磅）：横排是断行的可用容量，竖排是竖带的宽度。横排格内块传格内容区
     * 宽度；竖排格内块传版心宽 —— 旧循环的列位取的是版心，不是格框。必须为正。
     */
    float width_pt = 0.0f;

    /**
     * 调用方要求的书写模式。必须与 BlockSpec::wm 相同。
     */
    WritingMode writing_mode = WritingMode::HorizontalTB();

    /**
     * 竖排的竖带；横排忽略。
     */
    ColumnBand column;
};

/**
 * layoutBlock 的结果状态。
 *
 * 非 Ok 时 lines、columns 与 glyphs 为空、height_pt 为 0。调用方必须按状态分派，不能把
 * 这种空结果当成"这个块没有内容"：内容为空的块仍有一个行盒。
 */
enum class BlockLayoutStatus : uint8_t {
    Ok = 0,
    UnsupportedFloat = 1,
    InvalidConstraints = 2,
};

/**
 * 竖排的一列：列内断行片段与它在竖带里的落点。
 */
struct BlockColumn {
    /**
     * 本列的断行片段（竖排下 line_h 是列宽）。
     */
    BrokenLine line;

    /**
     * 第几条竖带，0 是块首那一条。分页下带号与页号的差是一个常量（块首另起带时差 1）；
     * 连页下带号只用来数换了几次带。
     */
    int band = 0;

    /**
     * 在这条带里是第几列，0 起。分页与行号都按它续。
     */
    int column = 0;

    /**
     * 列顶（磅）：分页下是页内 y；连页下是块轴绝对 y，绘制前按页号减条带偏移。
     */
    float top_pt = 0.0f;

    /**
     * 列心（页内 x，磅），与约束里竖带的左沿同一把尺子。
     */
    float center_pt = 0.0f;

    /**
     * 列宽（磅），等于 line.line_h。
     */
    float width_pt = 0.0f;
};

/**
 * 一个块的排版结果。
 *
 * 横排的坐标是块内坐标：原点取块内容区左上，x 为 0 是第一枚簇的落笔处，y 为 0 是
 * 第一行行槽顶。竖排的坐标沿用约束里竖带那把尺子（页内坐标）—— 竖排的列顶与列心本来
 * 就由页给出，换成块内原点会改变坐标的加法次序。
 */
struct BlockLayout {
    /**
     * 结果状态；见 BlockLayoutStatus。
     */
    BlockLayoutStatus status = BlockLayoutStatus::Ok;

    /**
     * 横排的断行片段：簇区间与本行度量，不含坐标。分页消费这一份。竖排为空，列见
     * columns。空段与取不到字体的块没有片段；空段仍有一个行盒（竖排空段有一条空列），
     * 只是那一行不是断行产物。
     */
    std::vector<BrokenLine> fragments;

    /**
     * 竖排的列，按列序；横排为空。
     */
    std::vector<BlockColumn> columns;

    /**
     * 竖排：块首那一条带是块局部排版另起的（整篇循环在这里翻一页）。横排恒为 false。
     */
    bool started_new_band = false;

    /**
     * 行盒，按行序（竖排按列序）。paragraph 恒为 0（块局部不认识文流里的块下标）；
     * 横排的 page 恒为 0，竖排的 page 是带号，便于与整篇对拍。line 是块内行号。要接进
     * 整篇时由调用方重新打这几个字段。
     */
    std::vector<LineBox> lines;

    /**
     * 字形，按 (行, 簇) 顺序。坐标与行盒同一套。
     */
    std::vector<PlacedGlyph> glyphs;

    /**
     * 横排：所有行的行槽高度之和（不含段前距与段后距 —— 那两项属于分页）。竖排为 0：
     * 列高由竖带决定，块自身不另记高度。
     */
    float height_pt = 0.0f;
};

/**
 * 块局部排版的断行层：约束检查与 layoutBlock 相同，只用已经整好的形断行。
 *
 * 整篇循环为了统计与簇起点已经整过一次形，拿这里的结果驱动分页，不再自己断行。
 * 横排结果里只有 fragments 与 height_pt，lines 与 glyphs 为空；竖排结果里只有 columns，
 * 每列带它的竖带号与列位。分页要的是不含坐标的断行片段，块内落位坐标在那里用不上。
 * 浮动图的排除带不在约束里：横排的绕排由 paginateBlockLinesWithFloats 逐行取宽，
 * 竖排列不受排除带约束（竖排里的浮动图在编译层已退回行内）。
 * 空块没有可断的簇（空段的行盒与空列要字体度量，由 layoutBlock 给出），结果是 Ok 且
 * fragments 与 columns 为空。
 *
 * @param block 要排的块
 * @param constraints 宽度、书写模式与竖带；书写模式必须与 block.wm 相同
 * @param shaped 插好占位簇的整形结果
 * @param cluster_metric 与 shaped.clusters 等长的行度量
 * @return 块局部结果；非 Ok 时 fragments 与 columns 为空，调用方必须退回旧路径
 */
BlockLayout layoutBlockFragments(const BlockSpec& block, const BlockConstraints& constraints,
                                 const BlockShaping& shaped,
                                 const std::vector<LineMetric>& cluster_metric);

/**
 * 排一个块：整形、按约束断行、把行/列放到块内坐标。
 *
 * 整形与断行复用 layoutBlockFragments，括号占位也走同一条路径。横排格内块传格内容区
 * 宽度；竖排格内块传版心宽与页的竖带，因为旧循环的列长与列位取的是版心而不是格框。
 * 含浮动原子的块明确返回 UnsupportedFloat，由 paginateBlockLinesWithFloats 处理。
 * page_break_before 与段前距在这里不生效。
 *
 * @param lib 字体库
 * @param block 要排的块
 * @param constraints 宽度、书写模式与竖带；书写模式必须与 block.wm 相同
 * @param cache 整形缓存；nullptr 表示不用缓存
 * @return 块内排版结果；不支持的输入见 status
 */
BlockLayout layoutBlock(FontLibrary& lib, const BlockSpec& block,
                        const BlockConstraints& constraints, ShapingCache* cache);

}  // namespace heditor
