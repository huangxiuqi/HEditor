/**
 * 编译：文档模型 → 引擎输入。
 *
 * 单向、可缓存（第十四节）：把树摊平成"段落序列"，每段一个 BlockSpec。它是模型与排版引擎
 * 之间唯一的接缝，只读树、不改树，产物是一份快照；也是模型层唯一 include 引擎类型
 * （layout.h 的 BlockSpec）的地方。长度换算（EMU 定点 → 磅值）集中在这里，是"整数在
 * 模型层、浮点在引擎层"这条边界的唯一一处实现。段落与字符格式都在这里按三层解析。
 *
 * 覆盖文本、段落、内容控件与表格：流里认 TextRun（贡献码点）、ParagraphMark（切段，并且是
 * 段落格式的宿主）与 ContentControl（两个边界落成段内的行内对象，内容照常并入当前段），
 * Document、Section 与 HeaderFooter 只作分组遍历 —— 它们的边界不进内容，也不切段。
 *
 * 表格走扁平化：格里的内容照常编成段落，每块带一条单元格路径说明它属于哪一格，产物另附
 * 表格结构（网格尺寸、行属性、列宽、每格的块区间）。单元格是段落边界，进格前先落当前段；
 * 嵌套表格就是路径更长一条。
 *
 * 其余元素一律写成诊断：静默放过比"还没做"更危险（调用方会以为拿到了完整结果）。容器仍
 * 照常下钻，内部文本按流顺序并入当前段；"未编译"只记在外层那一个元素上，免得一棵子树
 * 刷出几十条同因诊断。结尾没有结束标记的一段也照落，用默认格式。
 */
#pragma once

#include <string>
#include <vector>

#include "core/doc/node.h"
#include "core/doc/style.h"
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

/**
 * 把树编译成段落序列。vis 决定隐藏元素算不算内容：编辑态照常参与，呈现态整棵摘除
 * （与 FlowIndex / ParagraphIndex 同一口径，见 node.h 的 FlowVisibility）。
 *
 * sheet 是样式表：段落上挂的是"样式引用 + 内联覆盖"，这里按三层解析出完整格式再写进
 * 引擎输入（第六节）。没有样式表也要传一张空表 —— 空表的文档默认值就是各字段的默认值，
 * 于是"传空表"与"完全不解析样式"逐字段相同。
 * @param tree 文档树
 * @param vis 可见性口径
 * @param sheet 样式表
 * @return 块流、来源身份、表格与诊断
 */
CompileResult compileDocument(const NodeTree& tree, FlowVisibility vis, const StyleSheet& sheet);

/**
 * 把一条独立的内容片段编译成段落序列：页眉页脚（第十节）走这条路。
 *
 * 它与正文共用同一个编译过程（同一套块级节点、同一套三层解析），差别只在入口 —— 片段根
 * 不在正文流里、也不会遇到节，所以没有分节强制分页这一项。root 不是容器或不存在时返回
 * 空产物（blocks 为空、诊断也为空），调用方拿不到内容就是拿不到，不伪造一段空段落。
 * @param tree 文档树
 * @param root 片段根的节点号
 * @param vis 可见性口径
 * @param sheet 样式表
 * @return 这一棵子树的编译结果
 */
CompileResult compileFragment(const NodeTree& tree, NodeId root, FlowVisibility vis,
                              const StyleSheet& sheet);

/**
 * 只编译某个容器的一段子节点区间（[begin, end]，含两端），用来做增量编译。
 *
 * 编辑只动一段时没必要把整棵树重编一遍 —— 3000 段的文档里整树编译约 1.8ms，
 * 而那是每次按键都要付的一笔。区间由调用方从"上一枚段落标记之后"到"本段的结束标记"
 * 算出来，并且必须保证区间内不含节边界与嵌套的段落标记（含则退回 compileDocument）：
 * 那两样都会让"这一段的产物"依赖区间之外的状态（分节强制分页、段数）。
 *
 * 产出的 blocks 与 sources 仍按流顺序，且只覆盖这个区间里的段落。
 * @param tree 文档树
 * @param container 容器节点号
 * @param begin 起始槽位（含）
 * @param end 结束槽位（不含）
 * @param vis 可见性口径
 * @param sheet 样式表
 * @return 这一段孩子的编译结果
 */
CompileResult compileChildRange(const NodeTree& tree, NodeId container, int begin, int end,
                                FlowVisibility vis, const StyleSheet& sheet);

}  // namespace heditor
