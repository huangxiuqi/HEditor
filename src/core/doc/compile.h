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
#include "core/rev/compile_result.h"

namespace heditor {

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
