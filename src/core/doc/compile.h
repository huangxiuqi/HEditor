/**
 * 编译：文档模型 → 引擎输入。
 *
 * 单向、可缓存（第十四节）：把树摊平成"段落序列"，每段一个 BlockSpec。这是模型与排版引擎
 * 之间唯一的接缝，所以它只读树、不改树，产物是一份快照。模型层的长度是 EMU 定点整数，
 * 引擎吃磅值 —— 换算集中在这里，是"整数在模型层、浮点在引擎层"这条边界的唯一一处实现。
 *
 * 0.4 只覆盖文本与段落：流里只认 TextRun（贡献码点）与 ParagraphMark（切段，并且是
 * 段落格式的宿主）。Document 与 Section 是唯一被认的容器，只作为分组遍历 —— 它们的边界
 * 不进内容，也不切段。其余元素（图片、公式、控件、表格、Tab、域……）一律写成诊断：
 * 静默放过比"还没做"更危险（调用方会以为拿到了完整结果）。容器仍照常下钻，内部文本按
 * 流顺序并入当前段；"未编译"只记在外层那一个元素上，免得一棵表格子树刷出几十条同因诊断。
 * 文本节点上的字符样式同样进不了这一层（要等 1.6 的样式表），设了它就记一条诊断。
 *
 * 段落序列的切法就是流里的顺序：深度优先摊平，遇到 ParagraphMark 切一段。标记属于它结束的
 * 那一段，与 ParagraphIndex 同一口径；容器边界不是切分点，这正是"控件跨度跨过段落标记 =
 * 内部两个段落"的结构基础（第三节）。结尾还没有结束标记的一段也照落一段，用默认段落格式。
 *
 * 依赖方向：本文件属于模型层（步号 0.4 在模型文档里），却要 include layout.h 拿 BlockSpec。
 * 这是编译这一层作为"接缝"的固有代价，也仅此一处 —— 模型层其余文件都不认识引擎类型。
 */
#pragma once

#include <string>
#include <vector>

#include "core/doc/node.h"
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
 */
CompileResult compileDocument(const NodeTree& tree, FlowVisibility vis);

}  // namespace heditor
