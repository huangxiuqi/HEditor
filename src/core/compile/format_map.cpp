#include "core/compile/format_map.h"

#include <string>
#include <vector>

namespace heditor {

namespace {

/**
 * 绝对长度 → 磅。非绝对长度记诊断并保持引擎的默认值 —— 诊断已经让 ok() 变假，
 * 调用方不会把这份产物当完整结果。
 */
void absToPt(const Length& len, const char* field, NodeId mark, float* out,
             const FormatDiag& diag) {
    if (len.kind != LengthKind::Abs) {
        diag(CompileDiagnostic{NodeKind::ParagraphMark, mark,
                               std::string("段落格式的") + field + "必须是绝对长度"});
        return;
    }
    *out = static_cast<float>(emuToPt(len.abs_emu));
}

}  // namespace

void applyParaFormatToBlock(const StyleSheet& sheet, const std::string& style_ref,
                            const ParaFormatPatch& patch, NodeId mark, BlockSpec* block,
                            const FormatDiag& diag) {
    std::vector<std::string> why;
    const ParaFormat f = resolveParagraphFormat(sheet, style_ref, patch, &why);
    for (const std::string& w : why) {
        diag(CompileDiagnostic{NodeKind::ParagraphMark, mark, w});
    }
    block->font_index = f.font_index;
    absToPt(f.size, "字号", mark, &block->size_pt, diag);
    block->wm = f.wm;
    block->line_spacing.rule = f.line_rule;
    block->line_spacing.factor = f.line_factor;
    absToPt(f.line_exact, "行距固定值", mark, &block->line_spacing.exact_pt, diag);
    absToPt(f.space_before, "段前距", mark, &block->space_before_pt, diag);
    absToPt(f.space_after, "段后距", mark, &block->space_after_pt, diag);
    block->page_break_before = f.page_break_before;
    block->language = f.language;
    block->color = f.color;
    block->fallback_font_indices = f.fallback_font_indices;
}

void applyCharFormatToRun(const CharFormat& cf, NodeId node, FormatRun* run,
                          const FormatDiag& diag) {
    if (cf.font_index.has_value()) run->font_index = *cf.font_index;
    if (cf.color.has_value()) run->color = *cf.color;
    if (cf.size.has_value()) {
        if (cf.size->kind != LengthKind::Abs) {
            diag(CompileDiagnostic{NodeKind::TextRun, node, "字符格式的字号必须是绝对长度"});
        } else {
            run->size_pt = static_cast<float>(emuToPt(cf.size->abs_emu));
        }
    }
    // 三态照抄：没设置就沿用基底（段落默认），设置成 false 也要写下去 ——
    //   否则"取消加粗"会被解析成"继承"，而继承的值可能是粗的。
    if (cf.bold.has_value()) run->weight = *cf.bold ? kWeightBold : kWeightRegular;
    if (cf.italic.has_value()) run->italic = *cf.italic;
    if (cf.underline.has_value()) run->underline = *cf.underline;
}

WidthMapping mapWidth(const Length& len) {
    WidthMapping out;
    switch (len.kind) {
        case LengthKind::Auto:
            return out;
        case LengthKind::Abs:
            out.spec = WidthSpec::makeAbs(static_cast<float>(emuToPt(len.abs_emu)));
            return out;
        case LengthKind::Percent:
            out.spec =
                WidthSpec::makePercent(static_cast<float>(len.num) / static_cast<float>(len.den));
            return out;
        case LengthKind::Em:
        case LengthKind::Chars:
            break;
    }
    out.unresolved = true;
    return out;
}

RowHeightRule mapRowHeightRule(HeightRule rule) {
    switch (rule) {
        case HeightRule::Auto:
            return RowHeightRule::Auto;
        case HeightRule::AtLeast:
            return RowHeightRule::AtLeast;
        case HeightRule::Exact:
            return RowHeightRule::Exact;
    }
    return RowHeightRule::Auto;
}

BoxVAlign mapCellVAlign(CellVAlign align) {
    switch (align) {
        case CellVAlign::Top:
            return BoxVAlign::Top;
        case CellVAlign::Center:
            return BoxVAlign::Center;
        case CellVAlign::Bottom:
            return BoxVAlign::Bottom;
    }
    return BoxVAlign::Top;
}

}  // namespace heditor
