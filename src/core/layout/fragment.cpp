/**
 * 片段索引与后缀重新分页。
 *
 * 页/块/行三张反查表都由这里维护。整篇排版收尾时 rebuild 一次；增量排版只重排脏区时，
 * 后缀的重新分页也从这里的块行切片出发，不再按标量状态自己扫一遍行盒。
 */

#include "core/layout/fragment.h"

#include <algorithm>
#include <cmath>

#include "core/layout/block_pagination.h"

namespace heditor {

namespace {

/**
 * 一行在后缀重新分页之后的落点。
 */
struct LinePlacement {
    /**
     * lines 里的下标。
     */
    int line_index = 0;

    /**
     * 落到的页号。
     */
    int page = 0;

    /**
     * 行槽顶（分页下是页内 y，连页下是绝对 y）。
     */
    float top_pt = 0.0f;

    /**
     * 新的全局行号。
     */
    int line_no = 0;

    /**
     * 改写之前的全局行号，字形按它归属到行。
     */
    int old_line = 0;
};

}  // namespace

void FragmentIndex::rebuild(const std::vector<LineBox>& lines,
                            const std::vector<PlacedGlyph>& glyphs, size_t para_count,
                            int page_count) {
    const size_t n = static_cast<size_t>(std::max(1, page_count));
    page_lines.assign(n, {});
    block_lines.assign(para_count, {});
    page_glyphs.assign(n, {});
    fragments.clear();
    page_fragments.assign(n, {});
    // 先数一遍、按页预留，再填第二遍：每页一个 vector，边推边扩容会反复搬元素，
    //   而这两张表每敲一个字都要重建一次（4000 行 / 4.5 万字形那个量级）。
    std::vector<int> glyph_count(n, 0);
    for (const PlacedGlyph& g : glyphs) {
        if (g.page < 0 || static_cast<size_t>(g.page) >= n) continue;
        ++glyph_count[static_cast<size_t>(g.page)];
    }
    for (size_t p = 0; p < n; ++p) {
        if (glyph_count[p] > 0) page_glyphs[p].reserve(static_cast<size_t>(glyph_count[p]));
    }
    for (size_t i = 0; i < glyphs.size(); ++i) {
        const int p = glyphs[i].page;
        if (p < 0 || static_cast<size_t>(p) >= n) continue;
        page_glyphs[static_cast<size_t>(p)].push_back(static_cast<int>(i));
    }
    // 行数比字形少一个量级，直接推。
    for (size_t i = 0; i < lines.size(); ++i) {
        const int p = lines[i].page;
        if (p >= 0 && static_cast<size_t>(p) < n) {
            page_lines[static_cast<size_t>(p)].push_back(static_cast<int>(i));
        }
        const int para = lines[i].paragraph;
        if (para >= 0 && static_cast<size_t>(para) < block_lines.size()) {
            block_lines[static_cast<size_t>(para)].push_back(static_cast<int>(i));
        }
        if (p < 0 || static_cast<size_t>(p) >= n || para < 0) continue;
        if (fragments.empty() || fragments.back().block != para || fragments.back().page != p) {
            const int index = static_cast<int>(fragments.size());
            fragments.push_back(Fragment{para, p, static_cast<int>(i), static_cast<int>(i)});
            page_fragments[static_cast<size_t>(p)].push_back(index);
        } else {
            fragments.back().last_line = static_cast<int>(i);
        }
    }
}

void FragmentIndex::rebuildBlockLinesFrom(const std::vector<LineBox>& lines, size_t para_count,
                                          int first_block, int first_line) {
    if (block_lines.size() < para_count) {
        block_lines.resize(para_count);
    } else if (block_lines.size() > para_count) {
        block_lines.resize(para_count);
    }
    if (first_block < 0) first_block = 0;
    for (size_t b = static_cast<size_t>(first_block); b < block_lines.size(); ++b) {
        block_lines[b].clear();
    }
    const size_t begin = static_cast<size_t>(std::max(0, first_line));
    for (size_t i = begin; i < lines.size(); ++i) {
        const int para = lines[i].paragraph;
        if (para < first_block || para >= static_cast<int>(block_lines.size())) continue;
        block_lines[static_cast<size_t>(para)].push_back(static_cast<int>(i));
    }
}

const std::vector<int>& FragmentIndex::linesOnPage(int page) const {
    static const std::vector<int> kNone;
    if (page < 0 || static_cast<size_t>(page) >= page_lines.size()) return kNone;
    return page_lines[static_cast<size_t>(page)];
}

const std::vector<int>& FragmentIndex::fragmentsOnPage(int page) const {
    static const std::vector<int> kNone;
    if (page < 0 || static_cast<size_t>(page) >= page_fragments.size()) return kNone;
    return page_fragments[static_cast<size_t>(page)];
}

const std::vector<int>& FragmentIndex::linesOfBlock(int para) const {
    static const std::vector<int> kNone;
    if (para < 0 || static_cast<size_t>(para) >= block_lines.size()) return kNone;
    return block_lines[static_cast<size_t>(para)];
}

const std::vector<int>& FragmentIndex::glyphsOnPage(int page) const {
    static const std::vector<int> kNone;
    if (page < 0 || static_cast<size_t>(page) >= page_glyphs.size()) return kNone;
    return page_glyphs[static_cast<size_t>(page)];
}

bool FragmentIndex::valid(const std::vector<LineBox>& lines, const std::vector<PlacedGlyph>& glyphs,
                          size_t para_count, int page_count) const {
    const size_t pages = static_cast<size_t>(std::max(1, page_count));
    if (page_lines.size() != pages || page_glyphs.size() != pages || page_fragments.size() != pages)
        return false;
    if (block_lines.size() != para_count) return false;
    size_t line_seen = 0;
    for (size_t p = 0; p < page_lines.size(); ++p) {
        for (int index : page_lines[p]) {
            if (index < 0 || static_cast<size_t>(index) >= lines.size()) return false;
            if (lines[static_cast<size_t>(index)].page != static_cast<int>(p)) return false;
            ++line_seen;
        }
    }
    size_t block_seen = 0;
    for (size_t b = 0; b < block_lines.size(); ++b) {
        for (int index : block_lines[b]) {
            if (index < 0 || static_cast<size_t>(index) >= lines.size()) return false;
            if (lines[static_cast<size_t>(index)].paragraph != static_cast<int>(b)) return false;
            ++block_seen;
        }
    }
    size_t glyph_seen = 0;
    for (size_t p = 0; p < page_glyphs.size(); ++p) {
        for (int index : page_glyphs[p]) {
            if (index < 0 || static_cast<size_t>(index) >= glyphs.size()) return false;
            if (glyphs[static_cast<size_t>(index)].page != static_cast<int>(p)) return false;
            ++glyph_seen;
        }
    }
    size_t fragment_seen = 0;
    size_t fragment_total = 0;
    for (size_t p = 0; p < page_fragments.size(); ++p) {
        for (int index : page_fragments[p]) {
            if (index < 0 || static_cast<size_t>(index) >= fragments.size()) return false;
            const Fragment& f = fragments[static_cast<size_t>(index)];
            if (f.page != static_cast<int>(p) || f.first_line < 0 || f.last_line < f.first_line ||
                static_cast<size_t>(f.last_line) >= lines.size())
                return false;
            for (int line = f.first_line; line <= f.last_line; ++line) {
                if (lines[static_cast<size_t>(line)].page != f.page ||
                    lines[static_cast<size_t>(line)].paragraph != f.block)
                    return false;
            }
            fragment_seen += static_cast<size_t>(f.last_line - f.first_line + 1);
            ++fragment_total;
        }
    }
    if (fragment_total != fragments.size() || fragment_seen != lines.size()) return false;
    return line_seen == lines.size() && block_seen == lines.size() && glyph_seen == glyphs.size();
}

bool repaginateSuffix(LayoutResult* layout, const std::vector<BlockSpec>& blocks,
                      const PageMetrics& page, int from, int to, const ReflowState& anchor,
                      ReflowState* end_anchor) {
    if (layout == nullptr || end_anchor == nullptr) return false;
    const int n = static_cast<int>(layout->para_index.size());
    if (from < 0) from = 0;
    if (to > n) to = n;
    if (from >= to) {
        *end_anchor = anchor;
        return true;
    }
    if (to > static_cast<int>(blocks.size())) return false;
    const bool cont = page.continuous;
    const float slab = page.slabHeightPt();
    if (cont && !(slab > 0.0f)) return false;
    // 竖排的行槽推进不是"字号乘行距"的折线，这条路径不接；调用方退回续排。
    for (int p = from; p < to; ++p) {
        if (blocks[static_cast<size_t>(p)].wm.isVertical()) return false;
    }

    const float cy = page.margins.top;
    const float ch = page.contentHeightPt();
    const int first_line = layout->para_index[static_cast<size_t>(from)].line_begin;
    // 块行切片是这一层的分页输入：拼接之后后缀块号与行号都变了，先按行盒刷新一遍。
    layout->fragments.rebuildBlockLinesFrom(layout->lines, layout->para_index.size(), from,
                                            first_line);

    int page_no = anchor.page_index;
    float cursor = anchor.cursor_y;
    int line_no = anchor.line_total;
    std::vector<ReflowState> starts(static_cast<size_t>(to - from));
    std::vector<LinePlacement> placed;
    placed.reserve(layout->lines.size() > static_cast<size_t>(first_line)
                       ? layout->lines.size() - static_cast<size_t>(first_line)
                       : 0);

    for (int p = from; p < to; ++p) {
        const int start_page = cont ? static_cast<int>(std::floor(cursor / slab)) : page_no;
        starts[static_cast<size_t>(p - from)] = ReflowState{start_page, cursor, line_no};
        const std::vector<int>& ids = layout->fragments.linesOfBlock(p);
        const BlockSpec& block = blocks[static_cast<size_t>(p)];
        if (ids.empty()) {
            // 与整篇循环一致：没有行盒的块只可能翻页，不让段前距与段后距。
            if (!cont && block.page_break_before && cursor > cy + 0.01f) {
                ++page_no;
                cursor = cy;
            }
            continue;
        }
        std::vector<BrokenLine> fragments;
        fragments.reserve(ids.size());
        for (int index : ids) {
            if (index < 0 || static_cast<size_t>(index) >= layout->lines.size()) return false;
            BrokenLine line;
            line.line_h = layout->lines[static_cast<size_t>(index)].advance_pt;
            fragments.push_back(line);
        }
        BlockPaginationInput in;
        in.page_index = page_no;
        in.cursor_y_pt = cursor;
        in.content_top_pt = cy;
        in.content_bottom_pt = cy + ch;
        in.continuous = cont;
        in.slab_pt = slab;
        in.page_break_before = block.page_break_before;
        in.space_before_pt = block.space_before_pt;
        in.space_after_pt = block.space_after_pt;
        const BlockPaginationResult plan = paginateBlockLines(fragments, in);
        if (plan.lines.size() != ids.size()) return false;
        for (size_t i = 0; i < ids.size(); ++i) {
            LinePlacement item;
            item.line_index = ids[i];
            item.page = plan.lines[i].page;
            item.top_pt = plan.lines[i].top_pt;
            item.line_no = line_no + static_cast<int>(i);
            item.old_line = layout->lines[static_cast<size_t>(ids[i])].line;
            placed.push_back(item);
        }
        page_no = plan.page_index;
        cursor = plan.cursor_y_pt;
        line_no += static_cast<int>(ids.size());
    }

    // 字形按旧行号归属到行。先校验两趟：中途发现对不上时 LayoutResult 还没改过。
    for (int p = from; p < to; ++p) {
        const ParaIndex& pi = layout->para_index[static_cast<size_t>(p)];
        if (pi.glyph_begin < 0 || pi.glyph_end < pi.glyph_begin ||
            static_cast<size_t>(pi.glyph_end) > layout->glyphs.size()) {
            return false;
        }
        int g = pi.glyph_begin;
        for (int index : layout->fragments.linesOfBlock(p)) {
            const int old_line = layout->lines[static_cast<size_t>(index)].line;
            while (g < pi.glyph_end && layout->glyphs[static_cast<size_t>(g)].line == old_line) ++g;
        }
        if (g != pi.glyph_end) return false;
    }

    size_t g = static_cast<size_t>(layout->para_index[static_cast<size_t>(from)].glyph_begin);
    const size_t glyph_end =
        (to < n) ? static_cast<size_t>(layout->para_index[static_cast<size_t>(to)].glyph_begin)
                 : layout->glyphs.size();
    for (const LinePlacement& item : placed) {
        LineBox& lb = layout->lines[static_cast<size_t>(item.line_index)];
        // 连页下行槽顶是绝对的，行盒与字形存页内坐标；分页下两者相同。
        const float local =
            cont ? (item.top_pt - static_cast<float>(item.page) * slab) : item.top_pt;
        const float ink_lo = local + lb.lead_top_pt;
        const float axis = ink_lo + lb.ascent_pt;
        const float old_axis = lb.bracket_axis_pt;
        while (g < glyph_end && layout->glyphs[g].line == item.old_line) {
            PlacedGlyph& glyph = layout->glyphs[g];
            // 字形相对基线的距离不变，按新基线重算，不做增量加法（多一次结合会差 1ulp）。
            glyph.y_pt = axis - (old_axis - glyph.y_pt);
            glyph.page = item.page;
            glyph.line = item.line_no;
            ++g;
        }
        lb.block_lo_pt = ink_lo;
        lb.block_hi_pt = axis + lb.descent_pt;
        lb.bracket_axis_pt = axis;
        lb.page = item.page;
        lb.line = item.line_no;
    }
    for (int p = from; p < to; ++p) {
        layout->para_index[static_cast<size_t>(p)].state_at_start =
            starts[static_cast<size_t>(p - from)];
    }

    const int end_page = cont ? static_cast<int>(std::floor(cursor / slab)) : page_no;
    *end_anchor = ReflowState{end_page, cursor, line_no};
    if (to >= n) layout->stats.page_count = end_page + 1;
    summarizeLayoutStats(layout);
    return true;
}

}  // namespace heditor
