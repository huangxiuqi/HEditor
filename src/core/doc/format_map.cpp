#include "core/doc/format_map.h"

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
    block->line_factor = f.line_factor;
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
    if (cf.bold.value_or(false) || cf.italic.value_or(false) || cf.underline.value_or(false)) {
        diag(CompileDiagnostic{
            NodeKind::TextRun, node,
            "字符格式的粗体、斜体、下划线尚未进入编译（引擎输入还没有这些字段）"});
    }
}

}  // namespace heditor
