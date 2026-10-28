/**
 * 两个 BlockSpec 是不是同一份排版输入 —— 跨模型比对的唯一口径。
 *
 * 两份测试（doc_test 的投影 vs 编译、edit_test 的引擎编辑差分）都要它。各写一份的话，
 * 加字段时只改一处，另一处就开始漏比，而漏比的表现是"断言变绿"。
 */
#pragma once

#include <string>
#include <vector>

#include "core/layout/layout.h"

namespace testcmp {

using heditor::BlockSpec;
using heditor::CellSlot;
using heditor::InlineObject;
using heditor::LayoutCell;
using heditor::LayoutRow;
using heditor::LayoutTable;
using heditor::WidthSpec;

/**
 * 格路径里 table 是表的节点号（身份）。跨模型比较时两边的节点号不同源，抹掉它只留
 * row / cell —— 那两项才是"在哪一格"的内容。
 */
inline std::vector<CellSlot> cellPathShape(const std::vector<CellSlot>& path) {
    std::vector<CellSlot> out = path;
    for (CellSlot& s : out) s.table = 0;
    return out;
}

/**
 * 行内对象里 owner 是节点号（身份）。号码可比的场合由调用方另判，这里只比内容。
 */
inline bool sameObjects(const std::vector<InlineObject>& a, const std::vector<InlineObject>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].cp != b[i].cp || a[i].kind != b[i].kind || a[i].read_only != b[i].read_only) {
            return false;
        }
        if (a[i].w_pt != b[i].w_pt || a[i].h_pt != b[i].h_pt) return false;
        if (a[i].atom != b[i].atom || a[i].content_hash != b[i].content_hash) return false;
    }
    return true;
}

/**
 * 两块是不是同一份排版输入。除行内对象按上面那条口径外，其余字段交给 BlockSpec 的相等 ——
 * 它是逐字段写的，加字段漏比会编不过。
 */
inline bool sameBlock(const BlockSpec& a, const BlockSpec& b) {
    BlockSpec x = a;
    BlockSpec y = b;
    x.objects.clear();
    y.objects.clear();
    x.cell_path = cellPathShape(a.cell_path);
    y.cell_path = cellPathShape(b.cell_path);
    return x == y && sameObjects(a.objects, b.objects);
}

/**
 * 第一处不同的字段名，给差分判据的失败信息用。只说"不同"的断言没法查。
 */
inline std::string blockDiff(const BlockSpec& a, const BlockSpec& b) {
    if (a.cp_text != b.cp_text) return "cp_text";
    if (a.font_index != b.font_index) return "font_index";
    if (a.size_pt != b.size_pt) return "size_pt";
    if (!(a.wm == b.wm)) return "wm";
    if (!(a.line_spacing == b.line_spacing)) return "line_spacing";
    if (a.space_before_pt != b.space_before_pt) return "space_before_pt";
    if (a.space_after_pt != b.space_after_pt) return "space_after_pt";
    if (a.page_break_before != b.page_break_before) return "page_break_before";
    if (!BlockSpec::sameLanguage(a.language, b.language)) return "language";
    if (!(a.color == b.color)) return "color";
    if (a.fallback_font_indices != b.fallback_font_indices) return "fallback_font_indices";
    if (cellPathShape(a.cell_path) != cellPathShape(b.cell_path)) return "cell_path";
    if (a.runs.size() != b.runs.size()) return "runs.size";
    for (size_t i = 0; i < a.runs.size(); ++i) {
        if (!(a.runs[i] == b.runs[i])) return "runs[" + std::to_string(i) + "]";
    }
    if (a.objects.size() != b.objects.size()) return "objects.size";
    for (size_t i = 0; i < a.objects.size(); ++i) {
        if (a.objects[i].cp != b.objects[i].cp) return "objects.cp";
        if (a.objects[i].kind != b.objects[i].kind) return "objects.kind";
        if (a.objects[i].read_only != b.objects[i].read_only) return "objects.read_only";
        if (a.objects[i].w_pt != b.objects[i].w_pt) return "objects.w_pt";
        if (a.objects[i].h_pt != b.objects[i].h_pt) return "objects.h_pt";
        if (a.objects[i].atom != b.objects[i].atom) return "objects.atom";
        if (a.objects[i].content_hash != b.objects[i].content_hash) return "objects.content_hash";
    }
    return "（只有 owner 不同）";
}

inline bool sameWidth(const WidthSpec& a, const WidthSpec& b) {
    return a.kind == b.kind && a.pt == b.pt && a.ratio == b.ratio;
}

/**
 * 两张已投影的表格结构是不是同一份（宽度、对齐、列宽、行规则、每格的跨度与块区间）。
 *
 * 句柄（表格的节点号）不在这里比：两套模型各自建表时号码不可比。要连带比身份用 sameTable。
 */
inline bool sameTableShape(const LayoutTable& a, const LayoutTable& b) {
    if (!sameWidth(a.width, b.width) || a.align != b.align || a.rows != b.rows ||
        a.cols != b.cols) {
        return false;
    }
    if (a.col_widths.size() != b.col_widths.size()) return false;
    for (size_t i = 0; i < a.col_widths.size(); ++i) {
        if (!sameWidth(a.col_widths[i], b.col_widths[i])) return false;
    }
    if (a.row_specs.size() != b.row_specs.size()) return false;
    for (size_t i = 0; i < a.row_specs.size(); ++i) {
        const LayoutRow& x = a.row_specs[i];
        const LayoutRow& y = b.row_specs[i];
        if (x.can_split_by_page != y.can_split_by_page || x.is_header != y.is_header ||
            x.page_break_before != y.page_break_before || x.height_rule != y.height_rule ||
            x.height_pt != y.height_pt) {
            return false;
        }
    }
    if (a.cells.size() != b.cells.size()) return false;
    for (size_t i = 0; i < a.cells.size(); ++i) {
        const LayoutCell& x = a.cells[i];
        const LayoutCell& y = b.cells[i];
        if (x.row != y.row || x.cell != y.cell || x.row_span != y.row_span ||
            x.col_span != y.col_span || x.v_align != y.v_align || x.first_block != y.first_block ||
            x.block_count != y.block_count) {
            return false;
        }
    }
    return true;
}

/**
 * 结构相同且句柄相同 —— 修订从模型导出时节点号原样带过来，所以这条在那种比法里成立。
 */
inline bool sameTable(const LayoutTable& a, const LayoutTable& b) {
    return a.handle == b.handle && sameTableShape(a, b);
}

}  // namespace testcmp
