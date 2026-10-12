/**
 * 编译：文档模型 → 引擎输入。
 *
 * 单向、可缓存（第十四节）：把树摊平成"段落序列"，每段一个 BlockSpec。它是模型与排版引擎
 * 之间唯一的接缝，所以只读树、不改树，产物是一份快照；也是模型层唯一 include 引擎类型
 * （layout.h 的 BlockSpec）的地方。模型层的长度是 EMU 定点整数，引擎吃磅值 —— 换算集中在
 * 这里，是"整数在模型层、浮点在引擎层"这条边界的唯一一处实现。
 *
 * 覆盖文本、段落与内容控件：流里认 TextRun（贡献码点）、ParagraphMark（切段，并且是段落格式
 * 的宿主）与 ContentControl（两个边界落成段内的行内对象，内容照常并入当前段），Document、
 * Section 与 HeaderFooter 只作分组遍历 —— 它们的边界不进内容，也不切段。其余元素一律写成
 * 诊断：静默放过比"还没做"更危险（调用方会以为拿到了完整结果）。容器仍照常下钻，内部文本
 * 按流顺序并入当前段；"未编译"只记在外层那一个元素上，免得一棵表格子树刷出几十条同因诊断。
 *
 * 段落格式与字符格式都在这里按三层解析（第六节）出完整形式写进产物：段落格式进块级字段，
 * 字符格式进 run。引擎输入还没有的字符属性（粗体、斜体、下划线）记诊断而不静默丢。
 *
 * 切法就是流里的顺序：深度优先摊平，遇到 ParagraphMark 切一段。标记属于它结束的那一段，
 * 与 ParagraphIndex 同一口径；容器边界不是切分点。结尾没有结束标记的一段也照落，用默认格式。
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
    NodeKind kind = NodeKind::TextRun;  // 不支持的元素种类
    NodeId node = kInvalidNodeId;       // 该元素在树里的 id，便于定位
    std::string detail;                 // 人可读原因
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
    NodeId first = kInvalidNodeId;
    NodeId mark = kInvalidNodeId;
};

/**
 * 编译结果。blocks 按流顺序排列；diagnostics 非空表示有元素没被编译 —— 这时 blocks 只是
 * "能编的那部分"，调用方应据此拒绝使用或降级，不能当成完整结果。
 *
 * sources 与 blocks 一一对应（同长同序）；它只给引擎做坐标换算用，不影响"编了什么"。
 */
struct CompileResult {
    std::vector<BlockSpec> blocks;
    std::vector<CompileDiagnostic> diagnostics;
    std::vector<BlockSource> sources;

    bool ok() const { return diagnostics.empty(); }
};

/**
 * 把树编译成段落序列。vis 决定隐藏元素算不算内容：编辑态照常参与，呈现态整棵摘除
 * （与 FlowIndex / ParagraphIndex 同一口径，见 node.h 的 FlowVisibility）。
 *
 * sheet 是样式表：段落上挂的是"样式引用 + 内联覆盖"，这里按三层解析出完整格式再写进
 * 引擎输入（第六节）。没有样式表也要传一张空表 —— 空表的文档默认值就是各字段的默认值，
 * 于是解析结果与"完全没有样式"一致，0.4 那条"两条路逐字段一致"的判据因此仍然成立。
 */
CompileResult compileDocument(const NodeTree& tree, FlowVisibility vis, const StyleSheet& sheet);

/**
 * 把一条独立的内容片段编译成段落序列：页眉页脚（第十节）走这条路。
 *
 * 它与正文共用同一个编译过程（同一套块级节点、同一套三层解析），差别只在入口 —— 片段根
 * 不在正文流里、也不会遇到节，所以没有分节强制分页这一项。root 不是容器或不存在时返回
 * 空产物（blocks 为空、诊断也为空），调用方拿不到内容就是拿不到，不伪造一段空段落。
 */
CompileResult compileFragment(const NodeTree& tree, NodeId root, FlowVisibility vis,
                              const StyleSheet& sheet);

}  // namespace heditor
