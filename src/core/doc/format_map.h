/**
 * 三层解析的结果 → 引擎输入字段的那一段映射。
 *
 * 编译（NodeTree 到 BlockSpec）与投影（修订到 BlockSpec）都要做同一件事：把解析出来的
 * ParaFormat 与 CharFormat 写进 BlockSpec 与 FormatRun。这段映射看着机械，但每一条都带一个
 * 决定（未设置即继承、非绝对长度要拒、粗斜下划线还没有引擎字段要报），所以只能有一份 ——
 * 两份实现的漂移不会报错，只会让同一份文档在两条路上得到不同的字号。
 *
 * 诊断走回调：调用方决定去重与落点（编译按 detail 去重，投影原样收进结果）。
 */
#pragma once

#include <functional>

#include "core/doc/compile.h"
#include "core/doc/style.h"

namespace heditor {

/**
 * 一条诊断的去处。参数是已经填好种类与节点的完整记录。
 */
using FormatDiag = std::function<void(const CompileDiagnostic&)>;

/**
 * 段落格式 → 块级字段。
 *
 * 长度在这里做唯一一次换算：模型存 EMU 整数，引擎吃磅值。非绝对长度（百分比 / em / 字符数）
 * 要等排版给出参照才知道多长，编译期拒绝并记诊断 —— 静默取零会让一整段文字凭空没了字号。
 */
void applyParaFormatToBlock(const StyleSheet& sheet, const std::string& style_ref,
                            const ParaFormatPatch& patch, NodeId mark, BlockSpec* block,
                            const FormatDiag& diag);

/**
 * 字符格式 → run 字段：没设置的字段保留段落默认值（未设置即继承）。
 *
 * 粗体、斜体、下划线在引擎输入里还没有对应字段。设了却不报出来等于静默丢格式 ——
 * 排版看着没错、只是样式没了，比"不支持"难查得多。所以显式记诊断，不伪造成功路径。
 */
void applyCharFormatToRun(const CharFormat& cf, NodeId node, FormatRun* run,
                          const FormatDiag& diag);

/**
 * 模型长度 → 引擎宽度：Auto 保持自动，绝对长度换磅值，百分比换比例。
 *
 * em 与字符位在这一层算不出参照（要等字号与列宽），所以结果里带一个 unresolved 标记，
 * 由调用方按自动处理并记一条诊断 —— 静默取零会让"作者给了 3 个字宽"变成零宽列，
 * 而画面看着又是合理的。
 */
struct WidthMapping {
    WidthSpec spec;
    bool unresolved = false;
};

WidthMapping mapWidth(const Length& len);

/**
 * 行高规则与单元格垂直对齐到引擎枚举的映射。两边值域一一对应，但引擎不认模型枚举，
 * 所以这是唯一映射点。
 */
RowHeightRule mapRowHeightRule(HeightRule rule);
BoxVAlign mapCellVAlign(CellVAlign align);

}  // namespace heditor
