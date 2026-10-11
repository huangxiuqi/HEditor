/**
 * 控件索引：两张查找表，维护挂在编辑步骤上（见 docs/文档模型设计.md 第七节"控件索引"）。
 *
 * 表一是 id 到业务编码（顺带回答"这个 id 是不是控件"），表二是 key 到控件列表 —— 后者是
 * 一对多：同一个 key 可以有多个控件，这是"业务编码不唯一"的直接后果。
 *
 * 为什么必须有索引：回填会逐字段调用"按 key 批量设值"，如果每次都全篇扫一遍控件，
 * 性能与老编辑器一样差。差别不在 API 形状，在有没有索引。
 *
 * 维护挂在步骤上、不做手工同步：手工同步迟早漏一处，而漏一处的表现是"某个字段的值静默没改"，
 * 这是最难查的一类错。维护的分工由"子树里有没有控件"决定：没有控件就不动表（值替换只动文本
 * 节点，而一次批量设一千个字段会走一千遍这条路），有控件才重扫一遍（加字段删字段是低频的，
 * 重扫换"键表按文档顺序"这条正确性）。
 *
 * 文档里的第三张表（控件到内容位置）暂不维护：它唯一的消费者（焦点定位、依赖段落号的判据）
 * 还没落地，而现在就上表只能靠每次文本编辑在上面做加法。需要时再按步骤增量补。
 */
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/doc/node.h"
#include "core/doc/steps.h"

namespace heditor {

class ControlIndex {
public:
    /**
     * 重建两张表（一次前序遍历）。这份实现是正确性的基线：增量维护的结果必须与它逐项相同。
     */
    void rebuild(const NodeTree& tree);

    /**
     * 一步编辑之后维护索引。返回 false 表示这一步的效果跟不上（步骤里没记下被增删子树的身份），
     * 调用方必须重建。
     */
    bool onStep(const NodeTree& tree, const Step& step);

    bool has(NodeId id) const { return key_of_.find(id) != key_of_.end(); }
    int size() const { return static_cast<int>(key_of_.size()); }

    /**
     * 某个 id 的业务编码。不是控件时返回 nullptr。
     */
    const std::string* keyOf(NodeId id) const;

    /**
     * 同一个 key 的全部控件，按文档顺序（重建口径）。没有时返回空表 —— 返回引用而不是拷贝，
     * 因为批量设值会对每个 key 都问一次。
     */
    const std::vector<NodeId>& byKey(const std::string& key) const;

private:
    std::unordered_map<NodeId, std::string> key_of_;
    std::unordered_map<std::string, std::vector<NodeId>> by_key_;
};

/**
 * 全部控件的 id，按文档顺序。走一遍树 —— 枚举本身就是要 O(n)，不值得再维护一张顺序表。
 *
 * 它是自由函数而不是 ControlIndex 的成员：这件事与索引无关，还没有索引的地方（构建索引之前、
 * 校验索引是否与重建一致）也要用。
 */
std::vector<NodeId> controlIds(const NodeTree& tree);

}  // namespace heditor
