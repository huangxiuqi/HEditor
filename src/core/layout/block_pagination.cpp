#include "core/layout/block_pagination.h"

#include <algorithm>
#include <cmath>

#include "core/layout/float_flow.h"

namespace heditor {

BlockPaginationResult paginateBlockLines(const std::vector<BrokenLine>& fragments,
                                         const BlockPaginationInput& in) {
    BlockPaginationResult out;
    out.lines.reserve(fragments.size());
    out.page_index = in.page_index;
    float cursor_y = in.cursor_y_pt;

    // 段前分页先于段前距：翻页之后正好落在版心顶，段前距于是不再加。
    if (!in.continuous && in.page_break_before && cursor_y > in.content_top_pt + 0.01f) {
        ++out.page_index;
        cursor_y = in.content_top_pt;
    }
    // 页顶那一段不加段前距：页首的空白属于页边距，不属于段落。
    const bool at_page_top =
        !in.continuous && out.page_index > 0 && cursor_y <= in.content_top_pt + 0.01f;
    if (in.space_before_pt > 0.0f && !at_page_top) cursor_y += in.space_before_pt;

    float top = cursor_y;
    float prev_line_h = 0.0f;
    bool anchor = true;
    for (const BrokenLine& fragment : fragments) {
        const float line_h = fragment.line_h;
        float line_top = anchor ? cursor_y : (top + prev_line_h);
        // 用本行自己的行高判页底：拿段行高估值会漏判下一行更高的情形。
        // 最多重试一次；比整页还高的一行两次判据都不通过，页号连进两格。
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (in.continuous || line_top + line_h <= in.content_bottom_pt) break;
            ++out.page_index;
            cursor_y = in.content_top_pt;
            line_top = in.content_top_pt;
            anchor = true;
        }
        top = line_top;
        anchor = false;
        BlockPaginationLine placed;
        // 连页下页号只由行槽顶现算；分页下就是当前页。
        placed.page =
            in.continuous ? static_cast<int>(std::floor(line_top / in.slab_pt)) : out.page_index;
        placed.top_pt = line_top;
        out.lines.push_back(placed);
        prev_line_h = line_h;
    }
    out.cursor_y_pt = top + prev_line_h + in.space_after_pt;
    return out;
}

BlockFloatPaginationResult paginateBlockLinesWithFloats(
    const BlockSpec& block, const BlockShaping& shaped,
    const std::vector<LineMetric>& cluster_metric, FloatFlow* flow,
    const BlockFloatPaginationInput& in) {
    BlockFloatPaginationResult out;
    int page_index = in.page_index;
    float cursor_y = in.cursor_y_pt;
    float top = cursor_y;
    float prev_line_h = in.base_line_h_pt;
    bool anchor = true;
    bool first_line = true;
    bool floats_shifted = false;
    float anchor_flow = in.anchor_flow_pt;
    size_t start_cl = 0;
    std::vector<LineSpan> spans;
    std::vector<LinePart> parts;

    // 逐行落位：翻页预判、上下型下推、四周型让宽与断行必须按这个次序交替，
    //   改变其中任何两步的先后都会换掉某一行断在哪里。
    while (start_cl < shaped.clusters.size()) {
        float line_top = anchor ? cursor_y : (top + prev_line_h);
        float line_flow = floatFlowY(in.page_stride_pt, in.continuous, page_index, line_top);
        float line_h = 0.0f;
        // 最多两趟：第一趟按段行高估计定位，断完发现本行放不下就翻页再来。
        for (int attempt = 0; attempt < 2; ++attempt) {
            // 本页放不下就翻页。预判用段行高估计：旧循环在有浮动图时就是这么算的，
            //   换成真实行高会少翻一页。格内不翻页（no_page_break），页底判据整体关闭。
            if (!in.continuous && !in.no_page_break) {
                const int pg = floatFlowPage(in.page_stride_pt, line_flow, page_index);
                const float local = floatFlowLocal(in.page_stride_pt, pg, line_flow);
                if (pg > page_index || local + in.base_line_h_pt > in.content_bottom_pt) {
                    page_index = std::max(page_index + 1, pg);
                    cursor_y = in.content_top_pt;
                    line_flow = floatFlowY(in.page_stride_pt, in.continuous, page_index, cursor_y);
                    anchor = true;
                }
            }
            // 段首行被翻页推走：本段落下的图跟着平移一次，更早段落的图留在原页。
            if (first_line && !floats_shifted) {
                const float shift = line_flow - anchor_flow;
                if (std::fabs(shift) > 0.01f) {
                    if (flow != nullptr) flow->shiftFrom(in.live_base, shift);
                    anchor_flow = line_flow;
                    floats_shifted = true;
                    out.first_line_shift_pt = shift;
                }
            }
            // 上下型：行顶落在框的纵向区间里就整行下推。
            if (flow != nullptr) pushLineBelowFloats(*flow, in.base_line_h_pt, &line_flow);
            if (!in.continuous && !in.no_page_break) {
                const int pg = floatFlowPage(in.page_stride_pt, line_flow, page_index);
                const float local = floatFlowLocal(in.page_stride_pt, pg, line_flow);
                if (pg > page_index || local + in.base_line_h_pt > in.content_bottom_pt) {
                    page_index = std::max(page_index + 1, pg);
                    cursor_y = in.content_top_pt;
                    line_flow = floatFlowY(in.page_stride_pt, in.continuous, page_index, cursor_y);
                    anchor = true;
                }
            }
            // 四周型：把相交的带从本行可用区间里减掉。
            spans.assign(1, LineSpan{in.content_x_pt, in.content_width_pt});
            if (flow != nullptr) {
                cutSpansAroundFloats(*flow, line_flow, in.base_line_h_pt, kFloatWrapGapPt, &spans);
            }
            line_top = in.continuous ? line_flow
                                     : floatFlowLocal(in.page_stride_pt, page_index, line_flow);
            fillLineParts(shaped, cluster_metric, start_cl, spans, block.size_pt,
                          block.line_spacing, &parts, &line_h);
            // 一段都放不下：行顶移到图形底边再来。图高过整页时不再推，退回整宽。
            for (int guard = 0; guard < 4 && parts.empty(); ++guard) {
                const float next = flow != nullptr
                                       ? nextSquareFloatBottom(*flow, line_flow, in.base_line_h_pt)
                                       : line_flow;
                if (!(next > line_flow + 0.5f)) break;
                line_flow = next;
                line_top = in.continuous ? line_flow
                                         : floatFlowLocal(in.page_stride_pt, page_index, line_flow);
                spans.assign(1, LineSpan{in.content_x_pt, in.content_width_pt});
                if (flow != nullptr) {
                    cutSpansAroundFloats(*flow, line_flow, in.base_line_h_pt, kFloatWrapGapPt,
                                         &spans);
                }
                fillLineParts(shaped, cluster_metric, start_cl, spans, block.size_pt,
                              block.line_spacing, &parts, &line_h);
            }
            if (parts.empty()) {
                spans.assign(1, LineSpan{in.content_x_pt, in.content_width_pt});
                fillLineParts(shaped, cluster_metric, start_cl, spans, block.size_pt,
                              block.line_spacing, &parts, &line_h);
            }
            if (in.continuous || in.no_page_break || line_top + line_h <= in.content_bottom_pt)
                break;
            ++page_index;
            cursor_y = in.content_top_pt;
            line_top = in.content_top_pt;
            line_flow = floatFlowY(in.page_stride_pt, in.continuous, page_index, in.content_top_pt);
            anchor = true;
        }
        top = line_top;
        anchor = false;
        const bool have_parts = !parts.empty();
        const size_t next_start = have_parts ? parts.back().line.end : start_cl;
        if (have_parts) {
            BlockFloatPaginationLine placed;
            placed.parts = std::move(parts);
            placed.top_pt = top;
            placed.page =
                in.continuous ? static_cast<int>(std::floor(top / in.slab_pt)) : page_index;
            out.lines.push_back(std::move(placed));
        }
        prev_line_h = line_h;
        first_line = false;
        if (!have_parts) break;
        start_cl = next_start;
    }
    out.page_index = page_index;
    out.cursor_y_pt = top + prev_line_h + in.space_after_pt;
    return out;
}

BlockColumnPaginationResult paginateBlockColumns(const std::vector<BlockColumn>& columns,
                                                 const BlockColumnPaginationInput& in) {
    BlockColumnPaginationResult out;
    out.page_index = in.page_index;
    out.line_total = in.line_total;
    // 竖排块在分页下吃掉本页剩余高度：后续块从版心下沿之后起排。
    out.cursor_y_pt = in.content_bottom_pt + in.space_after_pt;
    if (columns.empty()) return out;

    // 块首另起的带就是旧循环在这里翻的那一页。连页没有页底，页号只由列顶现算。
    const int base_page =
        in.continuous ? in.page_index : in.page_index + (in.started_new_band ? 1 : 0);
    out.columns.reserve(columns.size());
    int last_band = 0;
    int last_col = 0;
    for (const BlockColumn& column : columns) {
        BlockColumnPaginationColumn placed;
        placed.page = in.continuous ? static_cast<int>(std::floor(column.top_pt / in.slab_pt))
                                    : base_page + column.band;
        placed.line = column.column;
        placed.top_pt = column.top_pt;
        placed.center_pt = column.center_pt;
        placed.width_pt = column.width_pt;
        placed.fragment = column.line;
        out.columns.push_back(placed);
        last_band = column.band;
        last_col = column.column;
    }
    out.page_index = in.continuous ? in.page_index : base_page + last_band;
    // 旧循环的账：行数只按最后一条带的列数往下推，前几条带的列号不累加。
    out.line_total = in.line_total + last_col + 1;
    // 连页的块尾游标是"最后一条带的带顶 + 带高"：带顶逐带累加，带高由调用方给。
    if (in.continuous) {
        out.cursor_y_pt = columns.back().top_pt + in.band_height_pt + in.space_after_pt;
    }
    return out;
}

}  // namespace heditor
