/**
 * 段落索引：每个容器一条"标记下标数组"，以及由它支撑的段落定位。
 *
 * 段落不是节点（第二节）：它是同一个容器内被标记切出来的范围，属性挂在结束标记上。
 * 于是"这一段从哪到哪、属性挂谁身上、某个槽位属于第几段"全部由一条数组回答 ——
 * 那个容器里哪些孩子是标记，按升序记下它们的下标。本类的数据就这一张表。
 * 分段的定义（下面的序号都由它推出来）：容器的孩子 c0..c(n-1) 中标记的下标是
 * m1 < ... < mk，则第 j 段是孩子在 [m(j-1)+1, mj] 上的范围（m0 记作 -1），
 * 也就是标记属于它结束的那一段。最后一段可能还没有结束标记（内容末尾还没插入标记），
 * 它照样算一段，只是没有属性宿主。
 *
 * 与 FlowIndex 分开是因为两者的变化面不同：改一个字符只动流位置、不动标记数组；
 * 插入或删除一个元素才动数组。合成一个类会让"改文字"这条最高频的路径也背上重建
 * 标记数组的成本，也让"哪个量什么时候失效"说不清。可见性口径与它一致：呈现态下
 * 隐藏的子树整棵不在流里，其中的标记也不在，所以段落数随模式变（第七节）。
 *
 * 全流的段落序号（第四节那条"之前的标记个数加一"）不在这里：它需要流位置参与，
 * 而流位置属于 FlowIndex。两者的组合留给真正用它的那一步（1.2 的脏段标记）一起做，
 * 避免这一步先造一张没人调的全局表。
 */
#pragma once

#include <unordered_map>
#include <vector>

#include "core/doc/node.h"

namespace heditor {

/**
 * 段落的容器内序号：1 起。0 留给"没有"（沿用 kInvalidNodeId 那套"0 是无效"的约定）。
 */
using ParaNo = int;

/**
 * 段落索引。构造时一次遍历建表，之后可以整体重建，也可以按增删增量维护。
 *
 * 增量维护的时机：先在树上做完增删，再带着被增删节点的 id 通知这里。要求传 id 而不是
 * 只传下标，是因为本类要判一件从下标看不出来的事 —— 那个节点在呈现态下是否被隐藏
 * （隐藏的节点不在流里，数组不受影响）。插入时还要靠它判种类（是不是标记）；
 * 删除时不用：那个下标是不是标记由数组本身回答，调用方报错种类也删不错。
 */
class ParaIndex {
public:
    explicit ParaIndex(const NodeTree& tree, FlowVisibility vis);

    /**
     * 该容器的标记下标数组（升序）。容器不在流里（被隐藏的子树，或根本不是容器）时
     * 返回 nullptr —— 与"容器在流里但没有标记"（返回一个空数组）是两件事，
     * 前者表示这个容器现在没有段落结构可言。
     */
    const std::vector<int>* marksOf(NodeId container) const;

    /**
     * 标记个数。不在流里返回 -1。
     */
    int markCount(NodeId container) const;

    /**
     * 段落数：含至少一个元素的段的个数。内容末尾还有未被标记结束的内容时，它也算一段
     * （最后一段）。空容器是 0 段。不在流里返回 -1。
     */
    int paragraphCount(NodeId container) const;

    /**
     * 第 at 个槽位所属段落的序号（1 起）。at 取 [0, 子节点数]，取子节点数时得到的是
     * "内容末尾那个位置"所属的段落 —— 内容末尾若已经被标记结束，它比段落数大 1，
     * 因为那个位置属于"还没开始写的下一段"。越界或不在流里返回 0。
     */
    ParaNo paragraphNoIn(NodeId container, int at) const;

    /**
     * 第 no 段占用的子下标范围 [begin, end)。no 越界、容器不在流里、或该段其实是空的
     * （内容末尾已被标记结束，那个"下一段"还没有内容）时返回 false。
     */
    bool paragraphRange(NodeId container, ParaNo no, int* begin, int* end) const;

    /**
     * 第 no 段的结束标记。该段没有结束标记（最后一段还没写标记）时返回 kInvalidNodeId；
     * no 越界或容器不在流里时返回 kInvalidNodeId 并让 ok 为 false。
     */
    NodeId markOfParagraph(NodeId container, ParaNo no, bool* ok = nullptr) const;

    /**
     * 增删之后的增量维护。返回 false 表示该通知对不上本类的数据（容器不在流里、
     * 或传进来的节点 id 查不到），调用方应视为"要么该重建、要么参数传错了"，
     * 而不是当成无事发生。呈现态下被隐藏的节点不在流里，数组不受影响，这时返回 true。
     */
    bool onInsert(NodeId container, int at, NodeId inserted);
    bool onErase(NodeId container, int at, NodeId erased);

    /**
     * 本类当前记账的容器（升序）。供"整体逐项比对"与不变量检查使用 ——
     * 没有它就只能抽查单个容器，而增量维护要证明的是整张表与重建的结果一致。
     */
    std::vector<NodeId> containers() const;

    /**
     * 重新扫描建表（与构造同一套逻辑）。整体重建的调用方用它，不必新建对象。
     */
    void rebuild(const NodeTree& tree, FlowVisibility vis);

private:
    void scan(NodeId id);
    /**
     * 把 id 这棵子树的记账项全部删掉（被摘下的子树仍可遍历，链还在）。
     */
    void drop(NodeId id);

    const NodeTree* tree_ = nullptr;
    FlowVisibility vis_ = FlowVisibility::Design;
    std::unordered_map<NodeId, std::vector<int>> marks_;
};

}  // namespace heditor
