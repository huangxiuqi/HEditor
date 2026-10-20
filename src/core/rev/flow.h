/**
 * 修订口径的流索引：节点号 → 展开区间。
 *
 * 与模型层的 FlowIndex 同一条展开规则（容器占起止两位、文本按码点、原子与标记各一位），
 * 所以换掉数据源之后光标与命中的坐标含义不变。两份不能合并：模型层那一份按可变节点走，
 * 这一份按不可变节点走，而编辑完成之后只保留这一份。
 *
 * 位置表按节点号直接下标。修订的节点号是递增水位，构造时按 next_id 开一次数组、
 * 顺序写一遍即可；不用哈希表是因为每次编辑之后都要重建一次，而重建就在按键路径上。
 */
#pragma once

#include <vector>

#include "core/doc/flow.h"
#include "core/doc/node.h"
#include "core/rev/rev.h"

namespace heditor::rev {

class FlowIndex {
public:
    /**
     * 一棵修订的展开区间。vis 与投影同一口径：呈现态下隐藏子树整棵摘除，它没有区间，
     * 内部任何位置都不可寻址。
     */
    FlowIndex(const Doc& doc, FlowVisibility vis);

    /**
     * 流位置的个数 = 最后一个合法位置 + 1。
     */
    int size() const { return size_; }

    /**
     * 流位置 → 节点级位置。入参会被夹到 [0, size()]。
     */
    Pos posAt(FlowPos flow) const;

    /**
     * 节点级位置 → 流位置。容器或槽位不存在、或 offset 越界时返回 -1。
     */
    FlowPos flowAt(const Pos& pos) const;

    /**
     * 这个节点当前在流里吗。不在流里与越界用同一种回执。
     */
    bool inFlow(NodeId id) const {
        const size_t i = static_cast<size_t>(id);
        return i < first_.size() && first_[i] >= 0;
    }

    /**
     * 节点占用的流位置区间 [first, end)。不在流里时返回 false。
     */
    bool intervalOf(NodeId id, int* first, int* end) const;

private:
    /**
     * 递归算出展开起点与跨度，cursor 是"下一个可用流位置"。
     */
    void measure(const Ref& n, int& cursor, bool is_root);

    const Node* find(NodeId id) const {
        const size_t i = static_cast<size_t>(id);
        return i < by_id_.size() ? by_id_[i] : nullptr;
    }
    int firstOf(NodeId id) const { return first_[static_cast<size_t>(id)]; }
    int spanOf(NodeId id) const { return span_[static_cast<size_t>(id)]; }
    int endOf(NodeId id) const { return firstOf(id) + spanOf(id); }
    int contentStartOf(NodeId id) const;

    NodeId root_ = kInvalidNodeId;
    FlowVisibility vis_ = FlowVisibility::Design;
    std::vector<const Node*> by_id_;
    std::vector<int> first_;
    std::vector<int> span_;
    int size_ = 0;
};

}  // namespace heditor::rev
