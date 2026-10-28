/**
 * 编译/投影产物的值类型。
 */
#pragma once

#include <string>
#include <vector>

#include "core/doc/node_id.h"
#include "core/doc/node_kind.h"
#include "core/layout/layout.h"

namespace heditor {
/**
 * 一条编译诊断：这里有一段内容没被编译，以及为什么。
 */
struct CompileDiagnostic {
    /**
     * 不支持的元素种类。
     */
    NodeKind kind = NodeKind::TextRun;

    /**
     * 该元素在树里的 id，便于定位。
     */
    NodeId node = kInvalidNodeId;

    /**
     * 人可读的失败原因。
     */
    std::string detail;
};

/**
 * 一块产物的来源身份。
 *
 * first 是本块吃到的第一个在流里占位的元素：文本节点、控件节点，或"空段"时的那枚结束标记。
 * 记节点 id 而不是流位置，是因为编译这一层不认识 FlowIndex（两者口径不同、都会随树变化），
 * 让这边算一个位置、那边又算一个，分叉是迟早的事。引擎拿它经 FlowIndex 换出本块内容起点的
 * 流位置 —— 单向换算，只有 FlowIndex 一个权威。控件节点落在它自己的起始边界上，与
 * "块内容从控件的开括号起"一致；空段落在标记上，就是"标记之前"。
 *
 * mark 是本块的结束标记；kInvalidNodeId 表示末尾那一段还没被标记结束（它用默认段落格式）。
 * 跨段控件会让中间那些块的 mark 落在控件内部，这不影响身份的正确性：每个块的 first 与 mark
 * 都是它自己那一段真实吃到的那两个元素。
 */
struct BlockSource {
    /**
     * 本块吃到的第一个在流里占位的元素。
     */
    NodeId first = kInvalidNodeId;

    /**
     * 本块的结束标记；无效值表示末尾那段还没被标记结束。
     */
    NodeId mark = kInvalidNodeId;
};

/**
 * 编译结果。blocks 按流顺序排列；diagnostics 非空表示有元素没被编译 —— 这时 blocks 只是
 * "能编的那部分"，调用方应据此拒绝使用或降级，不能当成完整结果。
 *
 * sources 与 blocks 一一对应（同长同序）；它只给引擎做坐标换算用，不影响"编了什么"。
 *
 * tables 是表格结构（出现顺序：外层在前，嵌套的内层在后）。它与 blocks 的关系是单向的：
 * 格用块区间索引内容，块用 cell_path 说明自己在哪一格。两层都记是因为用途不同 ——
 * 排版按格取内容，光标按块找格。
 */
struct CompileResult {
    /**
     * 块流，按流顺序。
     */
    std::vector<BlockSpec> blocks;

    /**
     * 没被编译的元素与原因。
     */
    std::vector<CompileDiagnostic> diagnostics;

    /**
     * 每块的来源身份，与 blocks 同长同序。
     */
    std::vector<BlockSource> sources;

    /**
     * 表格结构（外层在前，嵌套的内层在后）。
     */
    std::vector<LayoutTable> tables;

    /**
     * 浮动图的排版输入，按 (para, cp) 升序。
     *
     * 竖排段落与表格格里不产出：那两处没有可用的环绕几何（列宽随内容、格宽是版心的一部分），
     * 那里的浮动图退回行内排 —— 明确的边界，不做静默的错误布局。
     */
    std::vector<LayoutFloat> floats;

    /**
     * 产物完整吗（没有诊断）。
     * @return true = 没有诊断
     */
    bool ok() const { return diagnostics.empty(); }
};
}  // namespace heditor