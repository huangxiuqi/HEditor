#include "core/layout/block_shape.h"

#include <hb.h>

#include <algorithm>
#include <cmath>

#include "core/utils/hash.h"

namespace heditor {

namespace {

/**
 * 方括号在某个字体下的 advance（磅）。返回 0 表示该字体没有方括号字形 ——
 * 那就不留位，宁可没有空隙，也不要留一个宽度却画不出东西。
 */
float bracketAdvancePt(FontInstance* f) {
    if (!f) return 0.0f;
    hb_font_t* hbf = f->hb();
    if (!hbf) return 0.0f;
    hb_codepoint_t g = 0;
    if (!hb_font_get_nominal_glyph(hbf, static_cast<hb_codepoint_t>('['), &g) || !g) return 0.0f;
    const float pt = static_cast<float>(hb_font_get_glyph_h_advance(hbf, g)) / kFontScalePerPt;
    return pt > 0.0f ? pt : 0.0f;
}

/**
 * 一行/一列的切分结果。
 */
struct LineSlice {
    /**
     * 起始簇下标（含）。
     */
    size_t begin = 0;

    /**
     * 结束簇下标（不含）。
     */
    size_t end = 0;

    /**
     * 这一行的行内推进（磅）。
     */
    float advance_pt = 0.0f;
};

/**
 * 贪心断行：在 [start, clusters.size()) 里切出下一行，可用范围为 avail_pt。
 *
 * 边累积边推进，同时记住最后一个可断点；装不下时回退到该可断点。整行内一个可断点
 * 都没有（超长的不可断词）时在该处硬断，保证不会死循环。
 *
 * @param cluster_adv_pt 每个簇的推进
 * @param break_after 每个簇之后是否可断
 * @param start 起始簇下标
 * @param avail_pt 可用容量（磅）
 * @return 切出来的一行
 */
LineSlice takeLine(const std::vector<float>& cluster_adv_pt, const std::vector<bool>& break_after,
                   size_t start, float avail_pt) {
    LineSlice s;
    s.begin = start;

    float used = 0.0f;
    size_t j = start;
    size_t last_good = start;

    while (j < cluster_adv_pt.size()) {
        const float adv = cluster_adv_pt[j];
        // 至少放一个簇，避免超宽单簇导致空行。
        if (used + adv > avail_pt && j > start) break;
        used += adv;
        ++j;
        if (break_after[j - 1]) last_good = j;
    }

    size_t end;
    if (j >= cluster_adv_pt.size()) {
        end = cluster_adv_pt.size();
    } else {
        end = (last_good > start) ? last_good : j;
    }

    s.end = end;
    float sum = 0.0f;
    for (size_t i = s.begin; i < s.end; ++i) sum += cluster_adv_pt[i];
    s.advance_pt = sum;
    return s;
}

/**
 * 行距倍数的下限。小于它（含 0 与负数）按它算：块轴推进不能是 0，否则一页上几十行
 * 叠在同一处，分页、命中与滚动都没有尺子可用。
 */
constexpr float kMinLineFactor = 0.1f;

/**
 * 控件边界那一侧的括号该用哪个字体。
 *
 * 开口取边界所在的那个字（内容首字），闭口取前一个字（内容末字）：括号属于内容，
 * 换字体时它跟着变。字体没有方括号字形时退回块主字体。
 *
 * @param o 行内对象（控件边界）
 * @param fruns 格式 run 表
 * @param lib 字体库
 * @param n_cp 段长（码点）
 * @param primary 块主字体（退路）
 * @return 画这枚括号用的字体
 */
FontInstance* bracketFontFor(const InlineObject& o, const std::vector<FormatRun>& fruns,
                             FontLibrary& lib, uint32_t n_cp, FontInstance* primary) {
    if (fruns.empty() || n_cp == 0) return primary;
    uint32_t probe = (o.kind == InlineObjectKind::ControlEnd && o.cp > 0)
                         ? static_cast<uint32_t>(o.cp - 1)
                         : static_cast<uint32_t>(o.cp);
    if (probe >= n_cp) probe = n_cp - 1;
    const FormatRun* r = runCovering(fruns, probe);
    if (r == nullptr) return primary;
    FontInstance* f = lib.instance(r->font_index, r->size_pt, r->weight);
    if (f == nullptr || !(bracketAdvancePt(f) > 0.0f)) return primary;
    return f;
}

}  // namespace

float lineHeightFor(const LineSpacing& sp, float natural, float content_h) {
    if (sp.usesExact()) {
        // 固定值不给下限：exact 比内容矮时就是要压行、要裁字。
        if (sp.rule == LineSpacingRule::Exact) return sp.exact_pt;
        // 最小值：内容更高时跟着长，矮时用固定值。内容高取 natural 与墨迹的较大者 ——
        // 只按墨迹算的话，lineGap 非零的字体会在最小值档比单倍还紧。
        return std::max(std::max(natural, content_h), sp.exact_pt);
    }
    const float pitch = natural * std::max(sp.factor, kMinLineFactor);
    // 原子把行撑高时行距本身要留着：多出的部分加在行距之上，而不是取 max 把行距吃掉。
    return pitch + std::max(0.0f, content_h - natural);
}

float lineLeadTopFor(const LineSpacing& sp, float line_h, float ink_h) {
    if (sp.rule != LineSpacingRule::Auto) return 0.0f;
    return (line_h - ink_h) * 0.5f;
}

bool clipTextFor(const LineSpacing& sp, float ascent, float descent) {
    if (sp.rule != LineSpacingRule::Exact || !(sp.exact_pt > 0.0f)) return false;
    return sp.exact_pt + 0.01f < ascent + descent;
}

BrokenLine breakLine(const BlockShaping& bs, const std::vector<LineMetric>& cluster_metric,
                     size_t start, float avail, float block_size_pt, const LineSpacing& spacing) {
    const LineSlice slice = takeLine(bs.cluster_adv_pt, bs.break_after, start, avail);

    BrokenLine out;
    out.begin = slice.begin;
    out.end = slice.end;

    float ascent = 0.0f;
    float descent = 0.0f;
    float max_size = 0.0f;
    float max_box = 0.0f;
    // 文本自己的度量（不含原子）：行距是文本行之间的距离，由文本的 ascent 与 descent 定。
    float text_ascent = 0.0f;
    float text_descent = 0.0f;
    float text_gap = 0.0f;
    for (size_t i = slice.begin; i < slice.end; ++i) {
        ascent = std::max(ascent, cluster_metric[i].ascent_pt);
        descent = std::max(descent, cluster_metric[i].descent_pt);
        max_size = std::max(max_size, cluster_metric[i].size_pt);
        max_box = std::max(max_box, cluster_metric[i].box_h_pt);
        if (bs.clusters[i].atom == InlineAtom::None) {
            text_ascent = std::max(text_ascent, cluster_metric[i].ascent_pt);
            text_descent = std::max(text_descent, cluster_metric[i].descent_pt);
            text_gap = std::max(text_gap, cluster_metric[i].gap_pt);
        }
    }
    // 一个度量都取不到（取不到字体）就退回块级那套：几何必须存在。
    if (max_size <= 0.0f && max_box <= 0.0f) {
        ascent = bs.ascent_pt;
        descent = bs.descent_pt;
        max_size = block_size_pt;
    }

    // 单倍行高取字体自己的 ascent、descent 与 lineGap；取不到时退回块级度量或字号。
    // gap 只在行高里，不进 ascent —— 它落在 descent 之下，基线不动。
    float natural = text_ascent + text_descent + text_gap;
    if (!(natural > 0.0f)) natural = bs.ascent_pt + bs.descent_pt + bs.gap_pt;
    if (!(natural > 0.0f)) natural = (max_size > 0.0f) ? max_size : block_size_pt;
    out.ascent = ascent;
    out.descent = descent;
    out.size_pt = max_size;
    out.line_h = lineHeightFor(spacing, natural, ascent + descent);
    out.lead = lineLeadTopFor(spacing, out.line_h, ascent + descent);
    out.clip = clipTextFor(spacing, ascent, descent);
    return out;
}

void breakBlock(const BlockShaping& bs, const std::vector<LineMetric>& cluster_metric, float width,
                float block_size_pt, const LineSpacing& spacing, std::vector<BrokenLine>* out) {
    out->clear();
    if (bs.clusters.empty()) return;
    size_t start = 0;
    while (start < bs.clusters.size()) {
        const BrokenLine line = breakLine(bs, cluster_metric, start, width, block_size_pt, spacing);
        start = line.end;
        out->push_back(line);
    }
}

void fillLineParts(const BlockShaping& bs, const std::vector<LineMetric>& cluster_metric,
                   size_t start, const std::vector<LineSpan>& spans, float block_size_pt,
                   const LineSpacing& spacing, std::vector<LinePart>* parts, float* line_h_pt) {
    parts->clear();
    size_t cur = start;
    float h = 0.0f;
    for (const LineSpan& span : spans) {
        if (cur >= bs.clusters.size()) break;
        if (!(span.w > 1.0f)) continue;
        LinePart part;
        part.x = span.x;
        part.line = breakLine(bs, cluster_metric, cur, span.w, block_size_pt, spacing);
        if (part.line.end <= cur) continue;
        cur = part.line.end;
        h = std::max(h, part.line.line_h);
        parts->push_back(part);
    }
    if (line_h_pt != nullptr) *line_h_pt = h;
}

const BlockShaping* withInlineObjects(const BlockSpec& b, uint32_t n_cp, const BlockShaping* shaped,
                                      BlockShaping* scratch, const std::vector<FormatRun>& fruns,
                                      FontLibrary& lib, float max_hint_pt) {
    if (b.objects.empty()) return shaped;
    const BlockShaping& src = *shaped;
    // 槽宽 = 括号字形 advance 加间隙。间隙必须计入排版，否则字与括号会挤在一起。
    // 字体按边界那一侧的 run 现取：同一段里换了字体，两侧括号的宽度可以不同。
    auto slotOf = [&](const InlineObject& o) {
        FontInstance* f = bracketFontFor(o, fruns, lib, n_cp, src.primary);
        const float bw = bracketAdvancePt(f);
        return bw > 0.0f ? bw + kControlBracketSlotGapPt : 0.0f;
    };
    // 一个都插不进去就原样返回。原子不走这一条：它的宽度是自己给的。有提示文字的也留下：
    // 括号画不出时那句话仍要占位，否则空控件变成零宽，点击与光标都没地方落。
    bool has_atom = false;
    bool has_slot = false;
    for (const InlineObject& o : b.objects) {
        if (o.kind == InlineObjectKind::Atom) {
            has_atom = true;
        } else if (slotOf(o) > 0.0f || !o.hint.empty()) {
            has_slot = true;
        }
    }
    if (!has_slot && !has_atom) return shaped;

    // 这一段要插占位，簇序列会被改，需要一份可改的副本（shaped 可能指着只读的缓存条目）。
    if (shaped != scratch) *scratch = src;
    std::vector<BlockShaping::Cluster> cl2;
    std::vector<float> adv2;
    std::vector<bool> brk2;
    cl2.reserve(scratch->clusters.size() + b.objects.size());
    adv2.reserve(scratch->clusters.size() + b.objects.size());
    brk2.reserve(scratch->clusters.size() + b.objects.size());
    // 槽的 cp 取它后面那个真实簇的 cp（末尾用段长哨兵）。idx 是对象表里的下标：
    // 空控件的提示文字要往后看一格，才判得出这一对边界是紧挨着的。
    auto emitSlot = [&](const InlineObject& o, size_t idx, uint32_t cp) {
        BlockShaping::Cluster pc;
        pc.cp = cp;
        pc.g0 = pc.g1 = 0;
        pc.placeholder = true;
        pc.owner_control = o.owner;
        // 内容指纹两族都搬：原子的是画的是什么，控件边界的是外观摘要。少搬一族，
        // 那一类改动在几何上看不出来，页就会停在旧帧。
        pc.content_hash = o.content_hash;
        if (o.kind == InlineObjectKind::Atom) {
            pc.atom = o.atom;
            // 浮动图：流位置照旧（占一枚簇），版面上一寸不占，几何由 PlacedFloat 单独给出。
            pc.atom_h_pt = o.floating ? 0.0f : o.h_pt;
            cl2.push_back(pc);
            adv2.push_back(o.floating ? 0.0f : o.w_pt);
            // 盒子之后可以断：它与一个全角字同价，断在它后面不拆开任何东西。
            brk2.push_back(true);
            return;
        }
        pc.bracket_side =
            (o.kind == InlineObjectKind::ControlStart) ? BracketSide::Open : BracketSide::Close;
        pc.read_only = o.read_only;
        pc.bracket_font = bracketFontFor(o, fruns, lib, n_cp, src.primary);
        cl2.push_back(pc);
        adv2.push_back(slotOf(o));
        brk2.push_back(false);
        // 空控件的提示文字排在开括号之后、闭括号之前。两个条件缺一不可：产生端只在
        // 控件一个码点都没吃进来时给 hint，这里再确认紧跟的就是它自己的结束边界；
        // 中间夹了内容或嵌套控件时那句提示没有意义。
        if (o.hint.empty() || idx + 1 >= b.objects.size()) return;
        const InlineObject& next = b.objects[idx + 1];
        if (next.kind != InlineObjectKind::ControlEnd || next.owner != o.owner || next.cp != o.cp) {
            return;
        }
        float w = hintAdvancePt(pc.bracket_font, o.hint);
        if (w > max_hint_pt) w = max_hint_pt;
        if (!(w > 0.0f)) return;
        BlockShaping::Cluster hc;
        hc.cp = cp;
        hc.g0 = hc.g1 = 0;
        hc.placeholder = true;
        hc.control_hint = true;
        hc.owner_control = o.owner;
        hc.read_only = o.read_only;
        hc.bracket_font = pc.bracket_font;
        // 提示文字的摘要必须进簇：改一句话而字数不变时几何一个都不动，行指纹若只看
        // 几何就会判成这一页还是新鲜的，画面上仍是上一句。
        hc.content_hash = fnvMixCStr(kFnvBasis, o.hint.c_str());
        cl2.push_back(hc);
        adv2.push_back(w);
        brk2.push_back(false);
    };
    size_t oi = 0;
    for (size_t i = 0; i <= scratch->clusters.size(); ++i) {
        const uint32_t at = (i < scratch->clusters.size()) ? scratch->clusters[i].cp : n_cp;
        while (oi < b.objects.size() && b.objects[oi].cp <= at) {
            emitSlot(b.objects[oi], oi, at);
            ++oi;
        }
        if (i < scratch->clusters.size()) {
            cl2.push_back(scratch->clusters[i]);
            adv2.push_back(scratch->cluster_adv_pt[i]);
            brk2.push_back(scratch->break_after[i]);
        }
    }
    // 锚点越过段长的对象夹到段尾，而不是丢掉：丢一枚括号等于凭空少画一层，画面看着还有
    // 括号却层级不对；夹到段尾至少是可见的错误形态，也不会让后面的括号整串前移。
    for (; oi < b.objects.size(); ++oi) emitSlot(b.objects[oi], oi, n_cp);
    scratch->clusters.swap(cl2);
    scratch->cluster_adv_pt.swap(adv2);
    scratch->break_after.swap(brk2);
    return scratch;
}

Color runColorAt(const std::vector<FormatRun>& fruns, const BlockSpec& b, uint32_t cp) {
    const FormatRun* r = runCovering(fruns, cp);
    return r != nullptr ? r->color : b.color;
}

void stampRunMarks(const std::vector<FormatRun>& fruns, const BlockSpec& b, uint32_t cp,
                   PlacedGlyph* pg) {
    const FormatRun* r = runCovering(fruns, cp);
    if (r == nullptr) {
        pg->size_pt = b.size_pt;
        return;
    }
    pg->italic = r->italic;
    pg->underline = r->underline;
    pg->size_pt = r->size_pt;
    pg->weight = r->weight;
}

void computeClusterMetrics(FontLibrary& lib, const BlockSpec& b,
                           const std::vector<FormatRun>& fruns, const BlockShaping& bs,
                           std::vector<LineMetric>* cluster_metric, LineMetric* para_metric) {
    cluster_metric->assign(bs.clusters.size(), LineMetric{});
    *para_metric = LineMetric{};
    const FormatRun* cur = nullptr;
    LineMetric m;
    for (size_t i = 0; i < bs.clusters.size(); ++i) {
        const FormatRun* r = runCovering(fruns, bs.clusters[i].cp);
        if (r != cur) {
            cur = r;
            const int fi = (r != nullptr) ? r->font_index : b.font_index;
            m = LineMetric{};
            m.size_pt = (r != nullptr) ? r->size_pt : b.size_pt;
            if (FontInstance* f = lib.instance(fi, m.size_pt, r != nullptr ? r->weight : 400.0f)) {
                m.ascent_pt = f->ascenderPt();
                m.descent_pt = f->descenderPt();
                m.gap_pt = f->lineGapPt();
            }
        }
        (*cluster_metric)[i] = m;
        // 原子自己一套度量：盒高就是它的 ascent（盒底坐在基线上），字号留 0 ——
        // 盒高不是字号推出来的，混进去会让行高被行距再乘一遍。
        if (bs.clusters[i].atom != InlineAtom::None) {
            LineMetric am;
            am.ascent_pt = bs.clusters[i].atom_h_pt;
            am.descent_pt = 0.0f;
            am.box_h_pt = bs.clusters[i].atom_h_pt;
            (*cluster_metric)[i] = am;
        }
        para_metric->size_pt = std::max(para_metric->size_pt, (*cluster_metric)[i].size_pt);
        para_metric->ascent_pt = std::max(para_metric->ascent_pt, (*cluster_metric)[i].ascent_pt);
        para_metric->descent_pt =
            std::max(para_metric->descent_pt, (*cluster_metric)[i].descent_pt);
        para_metric->gap_pt = std::max(para_metric->gap_pt, (*cluster_metric)[i].gap_pt);
        para_metric->box_h_pt = std::max(para_metric->box_h_pt, (*cluster_metric)[i].box_h_pt);
    }
    // 一个度量都取不到（取不到字体）：退回块级那套，几何必须存在。
    if (para_metric->size_pt <= 0.0f) {
        *para_metric =
            LineMetric{b.size_pt, bs.ascent_pt, bs.descent_pt, para_metric->box_h_pt, bs.gap_pt};
    }
}

void placeHorizontalLine(const BlockSpec& b, const BlockShaping& bs,
                         const std::vector<FormatRun>& fruns, const std::vector<LinePart>& parts,
                         float top, int page, float page_y_offset, int para_index, int line_no,
                         const LineCellRange& cell, std::vector<LineBox>* lines,
                         std::vector<PlacedGlyph>* glyphs) {
    if (parts.empty()) return;
    const float usToPt = 1.0f / kFontScalePerPt;
    const uint32_t n_cp = static_cast<uint32_t>(b.cp_text.size());
    auto lastCpOf = [&](size_t slice_end) -> uint32_t {
        return (slice_end < bs.clusters.size()) ? bs.clusters[slice_end].cp : n_cp;
    };
    float ascent = 0.0f;
    float descent = 0.0f;
    float size_pt = 0.0f;
    float line_h = 0.0f;
    float lead = 0.0f;
    bool clip = false;
    for (const LinePart& p : parts) {
        ascent = std::max(ascent, p.line.ascent);
        descent = std::max(descent, p.line.descent);
        size_pt = std::max(size_pt, p.line.size_pt);
        line_h = std::max(line_h, p.line.line_h);
        lead = std::max(lead, p.line.lead);
        clip = clip || p.line.clip;
    }
    // 基线 = 行槽顶 + 上半截空白 + 本行的 ascent。同一视觉行只有一条基线。
    const float baseline = top + lead + ascent;
    LineBox lb;
    lb.paragraph = para_index;
    lb.line = line_no;
    lb.page = page;
    lb.cell_lo_pt = cell.cell_lo_pt;
    lb.cell_hi_pt = cell.cell_hi_pt;
    lb.cell_box_lo_pt = cell.box_lo_pt;
    lb.cell_box_hi_pt = cell.box_hi_pt;
    lb.vertical = false;
    lb.sideways = false;
    lb.size_pt = (size_pt > 0.0f) ? size_pt : b.size_pt;
    // 行顶就是 top：写成 (top + ascent) - ascent 会多一次舍入，而行顶是增量平移唯一
    // 能逐位复现的锚点。
    lb.lead_top_pt = lead;
    lb.block_lo_pt = top + lead + page_y_offset;
    lb.block_hi_pt = baseline + descent + page_y_offset;
    lb.ascent_pt = ascent;
    lb.descent_pt = descent;
    lb.advance_pt = line_h;
    lb.clip_text = clip;
    // 行内起点就是这一行的落笔位置（被图挖开时是第一段的左沿）。写成内容区左边的话，
    // 光标与选区会落在没让开图的地方，而字画在让开之后的位置。
    lb.inline_origin_pt = parts.front().x;
    // 括号要用块主字体画：槽宽就是按它算的，换个字体画，advance 就对不上。
    lb.bracket_font = bs.primary;
    lb.bracket_axis_pt = baseline + page_y_offset;
    lb.first_cp = bs.clusters[parts.front().line.begin].cp;
    lb.last_cp = lastCpOf(parts.back().line.end);
    {
        float ax = 0.0f;
        std::vector<LineBox::Span> spans;
        const bool multi = parts.size() > 1;
        if (multi) spans.reserve(parts.size());
        for (const LinePart& p : parts) {
            // 第二段起点落在图之后：ax 停在上一段末尾，这里一步跳过去。
            const float cur0 = std::max(p.x - lb.inline_origin_pt, ax);
            float cur = cur0;
            if (multi) spans.push_back(LineBox::Span{cur0, cur0});
            for (size_t i = p.line.begin; i < p.line.end; ++i) {
                LineCluster lc;
                lc.cp = bs.clusters[i].cp;
                lc.inline_pt = cur;
                lc.placeholder = bs.clusters[i].placeholder;
                lc.control_hint = bs.clusters[i].control_hint;
                lc.bracket_side = bs.clusters[i].bracket_side;
                lc.bracket_font = bs.clusters[i].bracket_font;
                lc.read_only = bs.clusters[i].read_only;
                lc.owner_control = bs.clusters[i].owner_control;
                lc.atom = bs.clusters[i].atom;
                lc.atom_h_pt = bs.clusters[i].atom_h_pt;
                lc.content_hash = bs.clusters[i].content_hash;
                lb.clusters.push_back(lc);
                cur += bs.cluster_adv_pt[i];
            }
            if (multi) spans.back().hi_pt = cur;
            ax = cur;
        }
        lb.inline_end_pt = ax;
        // 单段行留空：空 spans 就是一整行都可用，选区按默认一段处理。
        if (spans.size() > 1) lb.spans = std::move(spans);
    }
    lines->push_back(std::move(lb));

    for (const LinePart& p : parts) {
        float pen_x = p.x;
        float pen_y = baseline;
        // 行内位置由簇推进，不由字形连加推进：占位簇没有字形，字形连加在它身上原地不动，
        // 而行盒那一侧用的是簇推进。两者共用一个累加器，结构上不可能再不一致。
        float ax = 0.0f;
        for (size_t i = p.line.begin; i < p.line.end; ++i) {
            pen_x = p.x + ax;
            for (size_t g = bs.clusters[i].g0; g < bs.clusters[i].g1; ++g) {
                const ShapedGlyph& sg = bs.st.glyphs[g];
                PlacedGlyph pg;
                pg.glyph_id = sg.glyph_id;
                // 用该字形实际整形所用的字体（可能是回退链里的一环），不能用块的主字体。
                pg.font = bs.glyph_font[g];
                // y_offset 的符号：OpenType 的 yPlacement 向上为正，进屏幕坐标要取负。
                pg.x_pt = pen_x + sg.x_offset * usToPt;
                pg.y_pt = pen_y - sg.y_offset * usToPt + page_y_offset;
                pg.color = runColorAt(fruns, b, sg.cluster);
                stampRunMarks(fruns, b, sg.cluster, &pg);
                pg.page = page;
                pg.line = line_no;
                pg.vertical = false;
                pg.rotate_deg = 0.0f;
                pg.advance_pt = sg.x_advance * usToPt;
                pg.cluster = sg.cluster;
                pg.paragraph = para_index;
                glyphs->push_back(pg);

                pen_x += sg.x_advance * usToPt;
                pen_y += sg.y_advance * usToPt;
            }
            ax += bs.cluster_adv_pt[i];
        }
    }
}

void placeVerticalColumn(const BlockSpec& b, const BlockShaping& bs,
                         const std::vector<FormatRun>& fruns, const BrokenLine& col_line,
                         float col_center, float col_w, float pen_y, int page, bool continuous,
                         float slab_pt, int para_index, int line_no, const LineCellRange& cell,
                         std::vector<LineBox>* lines, std::vector<PlacedGlyph>* glyphs) {
    const float usToPt = 1.0f / kFontScalePerPt;
    const uint32_t n_cp = static_cast<uint32_t>(b.cp_text.size());
    auto lastCpOf = [&](size_t slice_end) -> uint32_t {
        return (slice_end < bs.clusters.size()) ? bs.clusters[slice_end].cp : n_cp;
    };
    // 连页下页号由位置现算；分页下就是块轴位置所在的页。
    auto pageOf = [&](float y) -> int {
        return continuous ? static_cast<int>(std::floor(y / slab_pt)) : page;
    };
    auto locY = [&](int pg, float y) -> float {
        return continuous ? y - static_cast<float>(pg) * slab_pt : y;
    };

    // 侧倒（蒙文）：横排成型 + 每个字形顺时针转 90°。
    //   局部坐标映射：横排下屏幕偏移 = (x_offset, -y_offset)；
    //   侧倒即把内容顺时针转 90°，(u, -v) ↦ (v, u)。于是
    //       屏幕偏移 = (y_offset, x_offset)
    //   笔的定位：v 轴（-descent … +ascender）现在是水平方向，
    //   要让字身在列里居中 → 基线 x = 列中线 - (ascender - descent)/2。
    const bool sideways = bs.st.sideways;
    const float col_asc = col_line.ascent;
    const float col_desc = col_line.descent;
    // 列内横轴的起始笔位：直立竖排 = 列心；侧倒 = 列心减去半个字身。
    // 名字里带 0：它只是"这一列的起点"，字形放置时那个 gx 会往前推进。
    const float gx0 = sideways ? (col_center - (col_asc - col_desc) * 0.5f) : col_center;

    LineBox lb;
    lb.paragraph = para_index;
    lb.line = line_no;
    lb.page = pageOf(pen_y);
    lb.cell_lo_pt = cell.cell_lo_pt;
    lb.cell_hi_pt = cell.cell_hi_pt;
    lb.cell_box_lo_pt = cell.box_lo_pt;
    lb.cell_box_hi_pt = cell.box_hi_pt;
    lb.vertical = true;
    lb.sideways = sideways;
    lb.size_pt = col_line.size_pt;
    lb.block_lo_pt = col_center - col_w * 0.5f;
    lb.block_hi_pt = col_center + col_w * 0.5f;
    lb.inline_origin_pt = locY(lb.page, pen_y);
    // 画括号用：字体 + 竖排下的列内横轴起始笔位。
    lb.bracket_font = bs.primary;
    lb.bracket_axis_pt = gx0;
    // 空列没有簇可指：那一列的行内只有可能存在的括号占位，first_cp 与 last_cp 都记 0。
    lb.first_cp = (col_line.begin < bs.clusters.size()) ? bs.clusters[col_line.begin].cp : 0;
    lb.last_cp = lastCpOf(col_line.end);
    {
        float ay = 0.0f;
        for (size_t i = col_line.begin; i < col_line.end; ++i) {
            LineCluster lc;
            lc.cp = bs.clusters[i].cp;
            lc.inline_pt = ay;
            lc.placeholder = bs.clusters[i].placeholder;
            lc.control_hint = bs.clusters[i].control_hint;
            lc.bracket_side = bs.clusters[i].bracket_side;
            lc.bracket_font = bs.clusters[i].bracket_font;
            lc.read_only = bs.clusters[i].read_only;
            lc.owner_control = bs.clusters[i].owner_control;
            lc.atom = bs.clusters[i].atom;
            lc.atom_h_pt = bs.clusters[i].atom_h_pt;
            lc.content_hash = bs.clusters[i].content_hash;
            lb.clusters.push_back(lc);
            ay += bs.cluster_adv_pt[i];
        }
        lb.inline_end_pt = ay;
    }
    lines->push_back(std::move(lb));

    float gx = gx0;
    float gy = pen_y;
    // 行内位置由簇推进，不由字形连加推进：占位簇没有字形，字形连加在它身上
    //   原地不动。竖排的行内轴是 y，所以这里重置的是 gy（gx 是列内横轴）。
    float ay = 0.0f;
    for (size_t i = col_line.begin; i < col_line.end; ++i) {
        gy = pen_y + ay;
        for (size_t g = bs.clusters[i].g0; g < bs.clusters[i].g1; ++g) {
            const ShapedGlyph& sg = bs.st.glyphs[g];
            PlacedGlyph pg;
            pg.glyph_id = sg.glyph_id;
            pg.font = bs.glyph_font[g];
            if (sideways) {
                pg.x_pt = gx + sg.y_offset * usToPt;
                pg.y_pt = gy + sg.x_offset * usToPt;
                pg.rotate_deg = 90.0f;
                pg.advance_pt = sg.y_advance * usToPt;
            } else {
                // 非侧倒走 HarfBuzz 的 TTB：它已自行减掉竖排原点
                // （hb-ot-shape.cc: subtract_glyph_v_origins）。
                // y_offset 是向上为正，进屏幕坐标要取负；
                //   验算：v_origin.y=14.08pt → y_offset=-14.08pt，
                //   取负后基线落在列顶下方 14.08pt，墨迹正好从列顶下 1.15pt 起。
                pg.x_pt = gx + sg.x_offset * usToPt;
                pg.y_pt = gy - sg.y_offset * usToPt;
                pg.rotate_deg = 0.0f;
            }
            const int gpage = pageOf(gy);
            pg.color = runColorAt(fruns, b, sg.cluster);
            stampRunMarks(fruns, b, sg.cluster, &pg);
            pg.page = gpage;
            pg.line = line_no;
            pg.vertical = true;
            pg.cluster = sg.cluster;
            pg.paragraph = para_index;
            // 竖排的行内轴是 y，所以页内坐标只改 y（x 是列内横轴，不过 locY）。
            pg.y_pt = locY(gpage, pg.y_pt);
            glyphs->push_back(pg);

            if (sideways) {
                // 行内推进量取自 hmtx 的 x_advance（横排度量），沿列向下
                gy += sg.x_advance * usToPt;
            } else {
                gx += sg.x_advance * usToPt;
                // 竖排向下推进：y_advance 为负，取反才是"向下"的步长
                gy += -sg.y_advance * usToPt;
            }
        }
        ay += bs.cluster_adv_pt[i];
    }
}

}  // namespace heditor
