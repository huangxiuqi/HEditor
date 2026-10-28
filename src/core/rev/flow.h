/**
 * 修订口径的流索引：节点号 → 展开区间。
 *
 * 与模型层的 FlowIndex 同一条展开规则（容器占起止两位、文本按码点、原子与标记各一位），
 * 所以换掉数据源之后光标与命中的坐标含义不变。两份不能合并：模型层那一份按可变节点走，
 * 这一份按不可变节点走，而编辑完成之后只保留这一份。
 *
 * 下标按活节点重新编号，不按节点号。节点号是递增水位：修订树每改一次就分配新号，旧号一个
 * 也不回收，于是"按节点号开数组"等于让索引的占用随会话长度增长，而活节点数并不增长。
 * 重新编号之后，索引的占用只与文档规模有关；代价是一次节点号到下标的查表。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/rev/flow_types.h"
#include "core/rev/rev.h"

namespace heditor::rev {

class FlowIndex {
public:

    /**
     * 一棵修订的展开区间。vis 与投影同一口径：呈现态下隐藏子树整棵摘除，它没有区间，
     * 内部任何位置都不可寻址。
     * @param doc 修订
     * @param vis 可见性口径
     */
    FlowIndex(const Doc& doc, FlowVisibility vis);

    /**
     * 流位置的个数 = 最后一个合法位置 + 1。
     * @return 流位置个数
     */
    int size() const { return size_; }

    /**
     * 流位置 → 节点级位置。入参会被夹到 [0, size()]。
     * @param flow 流位置
     * @return 节点级位置
     */
    Pos posAt(FlowPos flow) const;

    /**
     * 节点级位置 → 流位置。容器或槽位不存在、或 offset 越界时返回 -1。
     * @param pos 节点级位置
     * @return 流位置；不在流里为 -1
     */
    FlowPos flowAt(const Pos& pos) const;

    /**
     * 这个节点当前在流里吗。不在流里与越界用同一种回执。
     * @param id 节点号
     * @return true = 在流里
     */
    bool inFlow(NodeId id) const { return indexOf(id) >= 0; }

    /**
     * 节点占用的流位置区间 [first, end)。不在流里时返回 false。
     * @param id 节点号
     * @param first 写回区间起点
     * @param end 写回区间终点（不含）
     * @return true = 在流里
     */
    bool intervalOf(NodeId id, int* first, int* end) const;

    /**
     * 索引自身占用的堆字节数。判据用它确认占用只随活节点数走，不随会话长度走。
     * @return 这些数组实际占了多少字节
     */
    std::size_t allocatedBytes() const;

    /**
     * 索引里有几个活节点。把占用折成"每节点字节数"时要用它：水位涨而节点数不涨，
     * 与两者一起涨，是两种不同的情形，只看总字节数分不开。
     * @return 活节点数
     */
    int nodeCount() const { return static_cast<int>(nodes_.size()); }

private:

    /**
     * 节点号到下标的查表槽。id 取 0（kInvalidNodeId）表示空槽，负载因此必须低于一半，
     * 否则线性探测会绕回自身。
     */
    struct Entry {
        /**
         * 节点号（0 = 空槽）。
         */
        NodeId id = kInvalidNodeId;

        /**
         * 节点在紧凑数组里的下标；-1 = 空槽。
         */
        int idx = -1;
    };

    /**
     * 递归算出展开起点与跨度，cursor 是"下一个可用流位置"。
     */
    void measure(const Ref& n, int& cursor, bool is_root);

    /**
     * 按已收集的节点建查表。放在 measure 之后：遍历只写不读，表可以一次建好。
     */
    void buildLookup();

    /**
     * 节点号 → 紧凑数组下标（开放寻址表查一次）。
     *
     * @param id 节点号
     * @return 下标；不在流里为 -1
     */
    int indexOf(NodeId id) const;

    /**
     * 按下标取节点。
     *
     * @param idx 紧凑数组下标
     * @return 节点
     */
    const Node* nodeAt(int idx) const { return nodes_[static_cast<std::size_t>(idx)]; }

    /**
     * 按下标取展开起点。
     *
     * @param idx 紧凑数组下标
     * @return 流位置
     */
    int firstAt(int idx) const { return first_[static_cast<std::size_t>(idx)]; }

    /**
     * 按下标取展开跨度。
     *
     * @param idx 紧凑数组下标
     * @return 流位置个数
     */
    int spanAt(int idx) const { return span_[static_cast<std::size_t>(idx)]; }

    /**
     * 按下标取展开之后的终点（不含）。
     *
     * @param idx 紧凑数组下标
     * @return 流位置
     */
    int endAt(int idx) const { return firstAt(idx) + spanAt(idx); }

    /**
     * 节点内容起点的流位置（开标记之后那一格）。
     *
     * @param id 节点号
     * @return 流位置；不在流里为 -1
     */
    int contentStartOf(NodeId id) const;

    /**
     * 流的根节点。
     */
    NodeId root_ = kInvalidNodeId;

    /**
     * 建这份索引时用的可见性口径。
     */
    FlowVisibility vis_ = FlowVisibility::Design;

    /**
     * 活节点的紧凑数组（下标与节点号脱钩）。
     */
    std::vector<const Node*> nodes_;

    /**
     * 每个节点的展开起点（与 nodes_ 同下标）。
     */
    std::vector<int> first_;

    /**
     * 每个节点占几个流位置（与 nodes_ 同下标）。
     */
    std::vector<int> span_;

    /**
     * 节点号 → 下标的开放寻址表。
     */
    std::vector<Entry> lookup_;

    /**
     * 查表用的掩码（表长减一，表长是 2 的幂）。
     */
    std::size_t mask_ = 0;

    /**
     * 流的总长度（最后一个合法位置 + 1）。
     */
    int size_ = 0;
};

}  // namespace heditor::rev
