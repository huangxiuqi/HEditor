/**
 * 三层解析的结果 → 引擎输入字段的那一段映射。
 *
 * 编译与投影都要做同一件事：把解析出来的
 * ParaFormat 与 CharFormat 写进 BlockSpec 与 FormatRun。这段映射看着机械，但每一条都带一个
 * 决定（未设置即继承、非绝对长度要拒、粗斜下划线还没有引擎字段要报），所以只能有一份 ——
 * 两份实现的漂移不会报错，只会让同一份文档在两条路上得到不同的字号。
 *
 * 诊断走回调：调用方决定去重与落点（编译按 detail 去重，投影原样收进结果）。
 */
#pragma once

#include <functional>

#include "core/doc/compile_result.h"
#include "core/doc/style.h"
#include "core/doc/table_spec.h"

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
 * @param sheet 样式表
 * @param style_ref 段落样式引用
 * @param patch 内联覆盖
 * @param mark 段落标记（记诊断用）
 * @param block 写回的排版输入
 * @param diag 记一条诊断
 */
void applyParaFormatToBlock(const StyleSheet& sheet, const std::string& style_ref,
                            const ParaFormatPatch& patch, NodeId mark, BlockSpec* block,
                            const FormatDiag& diag);

/**
 * 字符格式 → run 字段：没设置的字段保留段落默认值（未设置即继承）。
 *
 * 粗体、斜体、下划线在引擎输入里还没有对应字段。设了却不报出来等于静默丢格式 ——
 * 排版看着没错、只是样式没了，比"不支持"难查得多。所以显式记诊断，不伪造成功路径。
 * @param cf 字符格式
 * @param node 文本节点（记诊断用）
 * @param run 写回的 run
 * @param diag 记一条诊断
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
    /**
     * 映射出来的引擎宽度。
     */
    WidthSpec spec;

    /**
     * 这一段算不出参照值，调用方按自动处理并记诊断。
     */
    bool unresolved = false;
};

/**
 * 模型长度 → 引擎宽度（Auto 保持自动，绝对换磅值）。
 * @param len 模型长度
 * @return 引擎宽度；算不出参照时带 unresolved 标记
 */
WidthMapping mapWidth(const Length& len);

/**
 * 行高规则与单元格垂直对齐到引擎枚举的映射。两边值域一一对应，但引擎不认模型枚举，
 * 所以这是唯一映射点。
 * @param rule 模型侧行高规则
 * @return 引擎侧行高规则
 */
RowHeightRule mapRowHeightRule(HeightRule rule);

/**
 * 单元格垂直对齐 → 引擎侧枚举。
 * @param align 模型侧对齐
 * @return 引擎侧对齐
 */
BoxVAlign mapCellVAlign(CellVAlign align);

}  // namespace heditor
