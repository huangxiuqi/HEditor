/**
 * 规范化：把树变成"最小形状"。
 *
 * 规范化只做两件事（第二节第 55 行那条不变量）：相邻且标记集合相同的文本节点合并成一个、
 * 清理空的文本节点。空 run 不承载任何字符、也没有身份，所以无意义；相邻的同标记 run
 * 只是同一段文字被切成了两截 —— 边界是算出来的，不是用户划出来的（第二节）。
 *
 * 两条刻意不做：跨容器不合并，容器边界是结构，分属不同容器的两个文本节点即使标记相同
 * 也不相邻；空容器不清除，与空 run 相反，"删空的控件"是合法状态，它有身份、有属性，
 * 作者删掉内容不等于删掉控件。
 *
 * 与编辑步骤解耦（见 1.2）：步骤只做最小改动，规范化在事务提交前统一跑一次。所以本函数的
 * 输入允许是中间态，输出必然是规范形状；而规范形状是唯一的，跑第二遍就不会再有任何改动 ——
 * 幂等判据直接读返回的读数。
 */
#pragma once

#include "core/doc/node.h"

namespace heditor {
/**
 * 规范化的改动读数。第二次跑必须全是 0。
 */
struct NormalizeResult {
    /**
     * 合并掉的文本节点个数（合并一次减一个节点）。
     */
    int merged = 0;

    /**
     * 清理掉的空文本节点个数。
     */
    int removed = 0;

    /**
     * 这一轮有没有改动。
     * @return true = 合并或清理过
     */
    bool changed() const { return merged > 0 || removed > 0; }
};

/**
 * 就地规范化整棵树，返回改动读数。
 *
 * 会改树结构，用到了 NodeTree 的两条改动路径之一的 eraseChild。派生索引
 * （FlowIndex / ParagraphIndex）在这之后一律失效，调用方要重建 —— 与任何编辑动作同一口径。
 * @param tree 文档树
 * @return 合并与清理的计数
 */
NormalizeResult normalizeDocument(NodeTree& tree);

}  // namespace heditor
