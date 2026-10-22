/**
 * 区间：一对流位置，左闭右开，带方向。
 *
 * 选区不落在文档里（第四节）—— 进模型的只是这个值。端点用流位置而不是 Pos{node, offset}：
 * 后者里的节点 ID 在节点被删掉之后悬空，而流位置是一个能被映射、能被复位的整数。
 * 端点带方向（anchor 是不动的那一端）也是模型的一部分：它决定 Shift+方向 下一次从哪一端
 * 继续扩展；参照项目的选区只有起点加长度、没有方向，表达不了这件事。
 */
#pragma once

#include "core/doc/flow.h"
#include "core/doc/node.h"

namespace heditor {

struct Range {
    /**
     * 不动的那一端。
     */
    FlowPos anchor = 0;

    /**
     * 跟着光标走的那一端。
     */
    FlowPos focus = 0;

    /**
     * 左右两端。方向不进这两个函数 —— 它们只回答"覆盖了哪一段"。
     * @return 靠前的那一端
     */
    FlowPos begin() const { return anchor < focus ? anchor : focus; }

    /**
     * 靠后的那一端。
     * @return 较大的流位置
     */
    FlowPos end() const { return anchor < focus ? focus : anchor; }

    /**
     * 选区是空的（两端重合）。
     * @return true = 两端相等
     */
    bool empty() const { return anchor == focus; }

    /**
     * 两个选区相同（两端都对得上，含方向）。
     */
    bool operator==(const Range& o) const { return anchor == o.anchor && focus == o.focus; }
};

/**
 * 端点规范化：把落在不可半分单位内部的端点向外扩到那个单位的边界。
 *
 * 不可半分单位是控件、表格与它的行列、链接 —— 它们的边界成对出现在同一个容器里，从中间切开
 * 会让起始与结束边界不成对（第三节）。参照项目遇到同类情况是拆元素再取区间；控件拆不了
 * （拆了边界就不成对），只能外扩。于是取出来的片段永远良构，这由结构保证、不靠调用方自觉。
 *
 * 空区间（光标）不扩：它取不出任何东西，也就没有"边界成对"的问题；扩了反而把"点在控件里"
 * 变成"选中整个控件"。非空区间是逐个端点按它在区间里的角色扩的 —— 起点收到单位起点、终点
 * 放到单位终点。
 *
 * 坐标口径是编辑态（Design）：编辑动作都发生在编辑态。
 * @param tree 文档树
 * @param range 区间
 * @return 端点外扩到不可半分单位的边界之后的区间
 */
Range normalizeRange(const NodeTree& tree, Range range);

}  // namespace heditor
