#include "core/layout/block_layout.h"

#include <algorithm>

#include "core/layout/block_shape.h"

namespace heditor {

namespace {

/**
 * 块局部排版的约束检查：不支持的情形返回明确状态，而不是猜一份几何。
 *
 * @param block 要排的块
 * @param constraints 宽度、书写模式与竖带
 * @return 可以排时为 Ok
 */
BlockLayoutStatus blockConstraintsStatus(const BlockSpec& block,
                                         const BlockConstraints& constraints) {
    if (!(constraints.writing_mode == block.wm)) return BlockLayoutStatus::InvalidConstraints;
    if (!(constraints.width_pt > 0.0f)) return BlockLayoutStatus::InvalidConstraints;
    // 竖排的列高来自竖带：带必须有下沿，块首列顶不能高于带顶。
    if (block.wm.isVertical()) {
        const ColumnBand& c = constraints.column;
        if (c.continuous) {
            // 连页的带顶是绝对坐标，逐带下移；容量是"带下沿减绝对带顶"，走到后续带时
            // 可以是负的（旧循环如此），所以只要求推进量与条带高为正。
            if (!(c.band_step_pt > 0.0f)) return BlockLayoutStatus::InvalidConstraints;
            if (!(c.slab_pt > 0.0f)) return BlockLayoutStatus::InvalidConstraints;
        } else {
            if (!(c.bottom_pt > c.band_top_pt)) return BlockLayoutStatus::InvalidConstraints;
            if (c.first_top_pt < c.band_top_pt) return BlockLayoutStatus::InvalidConstraints;
        }
    }
    // 横排格内块的断行只依赖格宽：格内不翻页，行高由表格在块排完之后结算，两项都不进
    // 断行输入。竖排格内块同样只依赖约束里的竖带 —— 它的列长与列位取版心而不是格框，
    // 但那是调用方传进来的页形状，块局部不需要知道表格。
    // 浮动图不占流宽，几何由 PlacedFloat 单独给出，绕排还要按行的纵向位置逐行取宽。
    for (const InlineObject& o : block.objects) {
        if (o.floating) return BlockLayoutStatus::UnsupportedFloat;
    }
    return BlockLayoutStatus::Ok;
}

/**
 * 整段的度量上界：与 computeClusterMetrics 的 para_metric 同一套取法。
 *
 * 竖排的列宽上界与"带里至少放得下一行"的判据都用它。块局部只有逐簇度量，没有那份
 * 汇总值，所以在这里按字段取最大值；一个字号都取不到时退回整形的字体度量。
 *
 * @param block 要排的块
 * @param shaped 整形结果
 * @param cluster_metric 簇度量
 * @return 整段度量上界
 */
LineMetric paragraphMetric(const BlockSpec& block, const BlockShaping& shaped,
                           const std::vector<LineMetric>& cluster_metric) {
    LineMetric pm;
    for (const LineMetric& m : cluster_metric) {
        pm.size_pt = std::max(pm.size_pt, m.size_pt);
        pm.ascent_pt = std::max(pm.ascent_pt, m.ascent_pt);
        pm.descent_pt = std::max(pm.descent_pt, m.descent_pt);
        pm.gap_pt = std::max(pm.gap_pt, m.gap_pt);
        pm.box_h_pt = std::max(pm.box_h_pt, m.box_h_pt);
    }
    if (pm.size_pt <= 0.0f) {
        pm.size_pt = block.size_pt;
        pm.ascent_pt = shaped.ascent_pt;
        pm.descent_pt = shaped.descent_pt;
        pm.gap_pt = shaped.gap_pt;
    }
    return pm;
}

/**
 * 竖排的断列：一条竖带一条竖带地切，列宽逐列算。
 *
 * 与整篇循环同一套次序：先看带里还剩多少高度，不够一行就换带；再断一列；列宽由断出来的
 * 行高给，放不进当前带宽度就换带（这一列仍用旧带的容量断，与旧循环一致）；换带后列位
 * 从带的起排边重新累加。块首那条带可能比后续带短，first_top_pt 就是它的列顶。连页的
 * 后续带顶是绝对坐标，逐带累加 band_step_pt，带下沿不动。
 *
 * @param block 要排的块
 * @param constraints 竖带与带宽度
 * @param shaped 整形结果
 * @param cluster_metric 簇度量
 * @param out 结果写到这里
 */
void breakBlockColumns(const BlockSpec& block, const BlockConstraints& constraints,
                       const BlockShaping& shaped, const std::vector<LineMetric>& cluster_metric,
                       BlockLayout* out) {
    const ColumnBand& band = constraints.column;
    const float width = constraints.width_pt;
    const LineMetric pm = paragraphMetric(block, shaped, cluster_metric);
    const float para_natural = (pm.ascent_pt + pm.descent_pt + pm.gap_pt > 0.0f)
                                   ? (pm.ascent_pt + pm.descent_pt + pm.gap_pt)
                                   : pm.size_pt;
    const float col_w_max = lineHeightFor(block.line_spacing, para_natural, para_natural);
    const bool rtl = block.wm.isBlockReversed();

    float region_top = band.first_top_pt;
    // 块首那条带放不下两条列位：整块另起一条带（整篇循环据此翻一页）。连页没有页底，
    // 这一条不成立。
    if (!band.continuous && region_top + col_w_max * 2.0f > band.bottom_pt) {
        out->started_new_band = true;
        region_top = band.band_top_pt;
    }
    float pen_y = region_top;
    float next_edge = rtl ? (band.left_pt + width) : band.left_pt;
    int band_index = 0;
    int col = 0;
    // 换一条带：列号归零、列位回到起排边。连页的带顶逐带累加而不是乘，浮点加法次序
    // 必须与旧循环逐位相同。
    auto nextBand = [&]() {
        region_top = band.continuous ? (region_top + band.band_step_pt) : band.band_top_pt;
        pen_y = region_top;
        next_edge = rtl ? (band.left_pt + width) : band.left_pt;
        col = 0;
        ++band_index;
    };
    size_t start = 0;
    while (start < shaped.clusters.size()) {
        float avail = band.bottom_pt - pen_y;
        // 带里连一行都放不下：换带再断。只换一次，与旧循环一致。
        if (avail < pm.size_pt) {
            nextBand();
            avail = band.bottom_pt - pen_y;
        }
        const BrokenLine col_line =
            breakLine(shaped, cluster_metric, start, avail, block.size_pt, block.line_spacing);
        const float col_w = col_line.line_h;
        // 这一列放不进剩余宽度：换带。列内容已按旧带的容量断好，与旧循环一致。
        const float edge_after = rtl ? (next_edge - col_w) : (next_edge + col_w);
        const bool overflow =
            rtl ? (edge_after < band.left_pt - 0.01f) : (edge_after > band.left_pt + width + 0.01f);
        if (col > 0 && overflow) nextBand();
        BlockColumn column;
        column.line = col_line;
        column.band = band_index;
        column.column = col;
        column.top_pt = pen_y;
        column.center_pt = rtl ? (next_edge - col_w * 0.5f) : (next_edge + col_w * 0.5f);
        column.width_pt = col_w;
        out->columns.push_back(column);
        start = col_line.end;
        if (start < shaped.clusters.size()) {
            next_edge = rtl ? (next_edge - col_w) : (next_edge + col_w);
            ++col;
            pen_y = region_top;
        }
    }
}

}  // namespace

BlockLayout layoutBlockFragments(const BlockSpec& block, const BlockConstraints& constraints,
                                 const BlockShaping& shaped,
                                 const std::vector<LineMetric>& cluster_metric) {
    BlockLayout out;
    out.status = blockConstraintsStatus(block, constraints);
    if (out.status != BlockLayoutStatus::Ok) return out;
    if (shaped.clusters.empty()) {
        // 没有可断的簇：空段的行盒与空列要字体度量，由 layoutBlock 给出；这一层只报空结果。
        return out;
    }
    if (block.wm.isVertical()) {
        breakBlockColumns(block, constraints, shaped, cluster_metric, &out);
        return out;
    }
    breakBlock(shaped, cluster_metric, constraints.width_pt, block.size_pt, block.line_spacing,
               &out.fragments);
    // 与 layoutBlock 的落位累加同一个次序：块高就是逐行相加的 top。
    float top = 0.0f;
    for (const BrokenLine& line : out.fragments) top += line.line_h;
    out.height_pt = top;
    return out;
}

BlockLayout layoutBlock(FontLibrary& lib, const BlockSpec& block,
                        const BlockConstraints& constraints, ShapingCache* cache) {
    BlockLayout out;
    out.status = blockConstraintsStatus(block, constraints);
    if (out.status != BlockLayoutStatus::Ok) return out;

    const float width = constraints.width_pt;
    if (block.cp_text.empty()) {
        FontInstance* font = lib.instance(block.font_index, block.size_pt);
        // 取不到字体就用字号折算的近似度量：空行的几何必须存在，否则光标没地方画。
        const float ascent = font ? font->ascenderPt() : block.size_pt * 0.8f;
        const float descent = font ? font->descenderPt() : block.size_pt * 0.2f;
        const float gap = font ? font->lineGapPt() : 0.0f;
        // 空段落一样要插括号占位：字段所在段被删空时，这一段里就只剩字段边界了。
        // 一枚图片的段落也走这一条，所以行高与基线要把原子的盒高算进去。
        BlockShaping empty_bs;
        empty_bs.primary = font;
        empty_bs.ascent_pt = ascent;
        empty_bs.descent_pt = descent;
        empty_bs.st.vertical = block.wm.isVertical();
        empty_bs.st.sideways = block.wm.isSideways();
        BlockShaping slot_bs;
        const BlockShaping* slotted =
            withInlineObjects(block, 0, &empty_bs, &slot_bs, std::vector<FormatRun>{}, lib, width);

        float box_h = 0.0f;
        for (const BlockShaping::Cluster& c : slotted->clusters) {
            if (c.atom != InlineAtom::None) box_h = std::max(box_h, c.atom_h_pt);
        }
        // 单倍行高与有内容的段同一个基准（ascent 加 descent），否则同一个格式下空行
        // 会比有字的行矮一截。
        float natural = ascent + descent + gap;
        if (!(natural > 0.0f)) natural = block.size_pt;
        const float ascent_eff = std::max(ascent, box_h);
        const float line_h = lineHeightFor(block.line_spacing, natural, ascent_eff + descent);
        const float lead = lineLeadTopFor(block.line_spacing, line_h, ascent_eff + descent);

        if (block.wm.isVertical()) {
            const ColumnBand& band = constraints.column;
            float top = band.first_top_pt;
            // 空段只有一列：块首那条带放不下两条列位就另起一条（整篇循环据此翻一页）。
            // 连页没有页底，这一条不成立。
            if (!band.continuous && top + line_h * 2.0f > band.bottom_pt) {
                out.started_new_band = true;
                top = band.band_top_pt;
            }
            BlockColumn column;
            // 空列没有字形，行内只有可能存在的括号占位：begin/end 覆盖全部占位簇，
            // 落位于是与有内容的列共用 placeVerticalColumn。
            column.line.begin = 0;
            column.line.end = slotted->clusters.size();
            column.line.line_h = line_h;
            column.line.size_pt = block.size_pt;
            // line 的 ascent 与 descent 留默认 0：侧倒的笔位要退回列心，整篇循环的空列
            // 正是这么记的。
            column.band = 0;
            column.column = 0;
            column.top_pt = top;
            column.width_pt = line_h;
            column.center_pt = block.wm.isBlockReversed() ? (band.left_pt + width - line_h * 0.5f)
                                                          : (band.left_pt + line_h * 0.5f);
            out.columns.push_back(column);
            placeVerticalColumn(block, *slotted, std::vector<FormatRun>{}, column.line,
                                column.center_pt, column.width_pt, column.top_pt, column.band,
                                band.continuous, band.slab_pt, /*para_index=*/0, /*line_no=*/0,
                                LineCellRange{}, &out.lines, &out.glyphs);
            return out;
        }

        LineBox lb;
        lb.paragraph = 0;
        lb.line = 0;
        lb.page = 0;
        lb.vertical = false;
        lb.sideways = false;
        lb.size_pt = block.size_pt;
        lb.bracket_font = font;
        lb.first_cp = 0;
        lb.last_cp = 0;
        // 行盒的簇 = 那些占位簇。空段落没有真实字形，行内推进只能由它们给出。
        float ax = 0.0f;
        for (size_t i = 0; i < slotted->clusters.size(); ++i) {
            lb.clusters.push_back(lineClusterOf(slotted->clusters[i], ax));
            ax += slotted->cluster_adv_pt[i];
        }
        lb.inline_end_pt = ax;
        lb.lead_top_pt = lead;
        lb.block_lo_pt = lead;
        lb.block_hi_pt = lead + (ascent_eff + descent);
        lb.ascent_pt = ascent_eff;
        lb.descent_pt = descent;
        lb.advance_pt = line_h;
        lb.clip_text = clipTextFor(block.line_spacing, ascent_eff, descent);
        lb.inline_origin_pt = 0.0f;
        lb.bracket_axis_pt = lead + ascent_eff;
        out.lines.push_back(std::move(lb));
        out.height_pt = line_h;
        return out;
    }

    const std::vector<FormatRun> fruns = block.effectiveRuns();
    BlockShaping scratch;
    BlockShapeStats stats;
    const BlockShaping* shaped = shapeBlockShaping(lib, block, fruns, cache, /*cache_slot=*/0,
                                                   /*cache_by_content=*/true, &scratch, &stats);
    // 与整篇排版同一条路径：取不到字体或整形为空时这个块没有行。
    if (!shaped) return out;

    const uint32_t n_cp = static_cast<uint32_t>(block.cp_text.size());
    shaped = withInlineObjects(block, n_cp, shaped, &scratch, fruns, lib, width);
    const BlockShaping& bs = *shaped;

    std::vector<LineMetric> cluster_metric;
    LineMetric para_metric;
    computeClusterMetrics(lib, block, fruns, bs, &cluster_metric, &para_metric);

    BlockLayout broken = layoutBlockFragments(block, constraints, bs, cluster_metric);
    out.status = broken.status;
    if (out.status != BlockLayoutStatus::Ok) return out;
    if (block.wm.isVertical()) {
        out.columns = std::move(broken.columns);
        out.started_new_band = broken.started_new_band;
        out.lines.reserve(out.columns.size());
        // 块局部没有页：列盒的 page 记带号，page 内坐标就是约束里竖带的位置。
        for (size_t i = 0; i < out.columns.size(); ++i) {
            const BlockColumn& column = out.columns[i];
            placeVerticalColumn(block, bs, fruns, column.line, column.center_pt, column.width_pt,
                                column.top_pt, column.band, constraints.column.continuous,
                                constraints.column.slab_pt, /*para_index=*/0, static_cast<int>(i),
                                LineCellRange{}, &out.lines, &out.glyphs);
        }
        return out;
    }
    out.fragments = std::move(broken.fragments);
    out.height_pt = broken.height_pt;

    out.lines.reserve(out.fragments.size());
    float top = 0.0f;
    for (size_t i = 0; i < out.fragments.size(); ++i) {
        LinePart part;
        part.line = out.fragments[i];
        part.x = 0.0f;
        // 块局部排版不认识浮动图，所以一行恒为一个片段。
        const std::vector<LinePart> parts{part};
        placeHorizontalLine(block, bs, fruns, parts, top, /*page=*/0, /*page_y_offset=*/0.0f,
                            /*para_index=*/0, static_cast<int>(i), LineCellRange{}, &out.lines,
                            &out.glyphs);
        top += out.fragments[i].line_h;
    }
    return out;
}

}  // namespace heditor
