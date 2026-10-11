/**
 * 编译：文档模型 → 引擎输入。
 *
 * 单向、可缓存（第十四节）：把树摊平成"段落序列"，每段一个 BlockSpec。它是模型与排版引擎
 * 之间唯一的接缝，所以只读树、不改树，产物是一份快照；也是模型层唯一 include 引擎类型
 * （layout.h 的 BlockSpec）的地方。模型层的长度是 EMU 定点整数，引擎吃磅值 —— 换算集中在
 * 这里，是"整数在模型层、浮点在引擎层"这条边界的唯一一处实现。
 *
 * 只覆盖文本与段落：流里只认 TextRun（贡献码点）与 ParagraphMark（切段，并且是段落格式的
 * 宿主），Document 与 Section 只作为分组遍历 —— 它们的边界不进内容，也不切段。其余元素
 * （图片、公式、控件、表格、Tab、域……）一律写成诊断：静默放过比"还没做"更危险（调用方会
 * 以为拿到了完整结果）。容器仍照常下钻，内部文本按流顺序并入当前段；"未编译"只记在外层那
 * 一个元素上，免得一棵表格子树刷出几十条同因诊断。文本的字符格式解析得出来（1.6），但引擎
 * 输入还没有 run 级字段，所以设了它就记一条诊断，等阶段 2 加行内区间时换成写进产物。
 *
 * 段落序列的切法就是流里的顺序：深度优先摊平，遇到 ParagraphMark 切一段。标记属于它结束的
 * 那一段，与 ParagraphIndex 同一口径；容器边界不是切分点，这正是"控件跨度跨过段落标记 =
 * 内部两个段落"的结构基础（第三节）。结尾还没有结束标记的一段也照落一段，用默认段落格式。
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
 * 编译结果。blocks 按流顺序排列；diagnostics 非空表示有元素没被编译 —— 这时 blocks 只是
 * "能编的那部分"，调用方应据此拒绝使用或降级，不能当成完整结果。
 */
struct CompileResult {
    std::vector<BlockSpec> blocks;
    std::vector<CompileDiagnostic> diagnostics;

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

}  // namespace heditor
