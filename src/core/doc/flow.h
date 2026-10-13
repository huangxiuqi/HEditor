/**
 * 位置：节点级坐标与流位置的互换。
 *
 * 两套坐标并存是方案一的直接结果（见 docs/文档模型设计.md 第四节）：节点级用
 * Pos{容器, 槽位}，段级与引擎级用流位置（一个整数）。光标与命中测试这类"贴在内容上"
 * 的接口用前者；段落序号、脏段标记、行内盒子下标用后者。两者必须双向互逆，
 * 判据在 tests/model_test.cpp。
 *
 * 流位置怎么展开，决定了这两者能不能互逆：按深度优先把树摊平，文本节点按码点展开
 * （占"码点数"个位置），其余叶子各占一个，容器占"起始边界 + 全部子节点 + 结束边界"。
 * 粒度与光标一致，所以"流位置到行内盒子下标几乎恒等"（差别只在簇与码点）。
 * 非编辑模式下被隐藏的控件整棵子树不参与展开，展开规则见下面的 FlowVisibility。
 *
 * 边界落在容器上时的归宿（对应参照项目"起始边界归一化为域之前、结束边界归一化为
 * 域内末尾"）：容器的起始边界自己占一个流位置，它属于「容器之前」；结束边界也占一个，
 * 它属于「容器内容的末尾」。于是"控件之前""进入控件""控件内容末尾""控件之后"是四个
 * 互不相同的流位置，控件被删空也不例外 —— 这正是上一轮四个边界缺陷的结构性解法。
 *
 * 根节点（Document）的展开不含边界：它不嵌在别处，没有任何位置指向"根之前/之后"。
 */
#pragma once

#include <vector>

#include "core/doc/node.h"

namespace heditor {

/**
 * 节点级位置。offset 的含义由 container 的种类决定：
 * 容器里是子节点槽位序号（0 到 children.size()），文本节点里是码点下标。
 */
struct Pos {
    NodeId container = kInvalidNodeId;
    int offset = 0;

    bool operator==(const Pos& o) const { return container == o.container && offset == o.offset; }
    bool operator!=(const Pos& o) const { return !(*this == o); }
};

/**
 * 流位置：摊平后的线性下标，取值 [0, size()]，size() 表示"最后一个元素之后"。
 */
using FlowPos = int;

/**
 * 流索引：由一棵树一次性预处理出每个节点的展开区间。
 *
 * 单独成类而不把区间挂在 Node 上，是因为它是派生量：树一改就要重算，而重算的时机
 * 该由调用方决定（1.2 的编辑步骤会在提交后重建）。挂在节点上会诱使人在编辑过程中读它，
 * 读到的是过期值。
 *
 * 被摘除的节点（Display 口径下的 hidden 子树）没有区间，任何指向它的 Pos
 * 都会被 flowAt 拒掉（返回 -1）——"不在流里"与"位置越界"用同一种回执。随之而来的一个
 * 后果要记住：容器槽位按完整的子节点数组编号，所以紧跟被摘除子节点的那个槽位不可寻址
 * （它的含义是"在被摘除者之后"，而那个位置不存在）。
 */
class FlowIndex {
public:
    explicit FlowIndex(const NodeTree& tree, FlowVisibility vis);

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
     * 这个节点当前在流里吗？Display 口径下被摘除的子树整个不在。
     * 公开是因为调用方也要问这个问题（例如"这个控件现在隐藏着，别去动它"），
     * 而 flowAt 的 -1 分不清"越界"与"不在流里"。
     *
     * 越界的 id（不是这棵树的节点）同样判不在流里：只有数组成员才谈得上位置。
     */
    bool inFlow(NodeId id) const {
        const size_t i = static_cast<size_t>(id);
        return i < first_.size() && first_[i] >= 0;
    }

    /**
     * 节点占用的流位置区间 [first, end)。不在流里时返回 false。
     * 用返回值表达"不在流里"而不抛异常：调用方多半是拿它做判断，
     * 异常会让"节点被摘除"从常规状态变成需要捕获的意外。
     */
    bool intervalOf(NodeId id, int* first, int* end) const;

private:
    /**
     * 递归算出这个节点的展开起点与跨度，cursor 是"下一个可用流位置"。
     *
     * 直接吃节点指针而不是 id：重建要按节点走一遍全树，而 find(id) 本身是一次哈希查找 ——
     * 拿 id 递归等于每个节点多付一次哈希，3000 段的文档里这一项与编译同量级。
     */
    void measure(const Node* n, int& cursor, bool is_root);

    int firstOf(NodeId id) const { return first_[static_cast<size_t>(id)]; }
    int spanOf(NodeId id) const { return span_[static_cast<size_t>(id)]; }
    /**
     * 节点展开之后的位置（= 第一个"不属于它"的流位置）。
     */
    int endOf(NodeId id) const { return firstOf(id) + spanOf(id); }
    /**
     * 容器内容开头。根没有起始边界，所以不加那一位。
     */
    int contentStartOf(NodeId id) const;

    const NodeTree* tree_ = nullptr;
    FlowVisibility vis_ = FlowVisibility::Design;
    /**
     * 位置表按节点 id 直接下标，既不缓存在节点上，也不走哈希表。
     *
     * 缓存在节点上会更快，但同一棵树上可以同时存在两个不同可见性口径的索引 ——
     * invariants 就是这么用的（design 与 display 各一个，用来判"摘除集合与隐藏声明
     * 是否相符"）；缓存放节点上，后构造的那个会把先构造的口径覆盖掉，于是 design
     * 对一个隐藏节点说"不在流里"，不变量当场报红。
     *
     * 数组够用是因为 id 是递增水位（node.h 的 maxAllocatedId）：构造时按水位开一次、
     * 顺序写一遍，比重建哈希表快一个量级。不在流里的节点留 -1（摘下的子树、呈现态下
     * 被隐藏的子树），inFlow 就是问"这一格不是 -1"。
     */
    std::vector<int> first_;
    std::vector<int> span_;
    int size_ = 0;
};

}  // namespace heditor
