/**
 * 浮动图排除带与块轴浮动流：横排正文里"这一行还有多宽、要不要下推"。
 *
 * 排除带记在块轴（flow）坐标里：分页下是"页号乘页高加页内 y"，连页下就是绝对 y。
 * 绕排只比较纵向区间与横向区间，跨页时同一套比较仍然成立。只有上下型（整行下推）与
 * 四周型（挖掉两侧）进这一层；衬于文字下方与浮于文字上方不占位。
 *
 * 整篇排版循环与块局部分页共用这里的实现：贴边间隙、下推轮次与"按哪一条带的次序算"
 * 都只有一份，两个消费者才可能给出逐位相同的几何。
 */
#pragma once

#include <cstddef>
#include <vector>

#include "core/layout/block_shape.h"
#include "core/layout/float_band.h"

namespace heditor {

/**
 * 生效中的排除带：按放上版面的先后排列，跨段保留，直到下沿越过当前行顶。
 */
struct FloatFlow {
    /**
     * 生效中的带。
     */
    std::vector<FloatBand> bands;

    /**
     * 从第 begin 条起整体下移 dy（磅）。
     *
     * 段首行被翻页推走时只平移本段落下的图：更早段落的图已经画在上一页上。
     * @param begin 起始下标
     * @param dy 位移（磅）
     */
    void shiftFrom(size_t begin, float dy);

    /**
     * 清掉下沿已经越过 at_flow_pt 的带。
     * @param at_flow_pt 当前行顶的块轴 y（磅）
     */
    void prune(float at_flow_pt);
};

/**
 * 块轴 y 与页内 y 的换算：分页下相差页号乘步距，连页下块轴 y 就是绝对 y。
 * @param page_stride_pt 页步距（分页 = 页高，连页 = 条带高，磅）
 * @param continuous 连页
 * @param page 页号（连页下是条带号）
 * @param local_y_pt 页内 y（磅）
 * @return 块轴 y（磅）
 */
float floatFlowY(float page_stride_pt, bool continuous, int page, float local_y_pt);

/**
 * 块轴 y 落在第几页。
 * @param page_stride_pt 页步距（磅）
 * @param flow_y_pt 块轴 y（磅）
 * @param fallback_page 步距不为正时返回的页号
 * @return 页号（不小于 0）
 */
int floatFlowPage(float page_stride_pt, float flow_y_pt, int fallback_page);

/**
 * 块轴 y 折回页内：减页号乘步距。
 * @param page_stride_pt 页步距（磅）
 * @param page 页号
 * @param flow_y_pt 块轴 y（磅）
 * @return 页内 y（磅）
 */
float floatFlowLocal(float page_stride_pt, int page, float flow_y_pt);

/**
 * 这一行是否与排除带在纵向相交。
 *
 * 行顶那半个点的余量是旧循环的口径：与带下沿齐平的行不算压在图里。
 * @param band 排除带
 * @param flow_y_pt 行顶的块轴 y（磅）
 * @param line_h_pt 用来判定的行高（旧循环取段行高估计，磅）
 * @return true = 相交
 */
bool floatBandOverlapsLine(const FloatBand& band, float flow_y_pt, float line_h_pt);

/**
 * 上下型下推：行顶落在带里就整行移到带下沿，最多四轮。
 *
 * 带之间可以上下相接，推一次之后可能又落进下一条，所以轮次是收敛条件的一部分。
 * @param flow 生效中的排除带
 * @param line_h_pt 用来判定的行高（磅）
 * @param flow_y_pt 出参：行顶的块轴 y（磅）
 */
void pushLineBelowFloats(const FloatFlow& flow, float line_h_pt, float* flow_y_pt);

/**
 * 四周型把这一行挖到连一段都放不下时，下一个可用的行顶：相交带里最低的下沿。
 * @param flow 生效中的排除带
 * @param flow_y_pt 行顶的块轴 y（磅）
 * @param line_h_pt 用来判定的行高（磅）
 * @return 下推后的行顶（磅）；没有可推的带时等于入参
 */
float nextSquareFloatBottom(const FloatFlow& flow, float flow_y_pt, float line_h_pt);

/**
 * 从区间表里减掉 [lo, hi)：与它相交的每一段被切成左右两截。
 * @param spans 区间表（就地修改）
 * @param lo_pt 被占掉的左沿
 * @param hi_pt 被占掉的右沿
 */
void cutSpanRange(std::vector<LineSpan>* spans, float lo_pt, float hi_pt);

/**
 * 四周型让宽：把相交的带各挖掉一段，左右各留贴边间隙。
 * @param flow 生效中的排除带
 * @param flow_y_pt 行顶的块轴 y（磅）
 * @param line_h_pt 用来判定的行高（磅）
 * @param gap_pt 图与文字的贴边间隙（磅）
 * @param spans 本行的可用区间（就地修改）
 */
void cutSpansAroundFloats(const FloatFlow& flow, float flow_y_pt, float line_h_pt, float gap_pt,
                          std::vector<LineSpan>* spans);

/**
 * 图与文字的贴边间隙（磅）。太小会贴着图形边缘，与字处理软件的默认留白同档。
 */
inline constexpr float kFloatWrapGapPt = 6.0f;

}  // namespace heditor
