/**
 * 修订留痕：文档是基线，修订是一串未被折叠的步骤叠加在基线之上（见 docs/文档模型设计.md
 * 第十二节）。
 *
 * 为什么走这条路而不是把修订标记写进文档树：后者会让树里同时存在"当前有效内容"与"待定的
 * 影子内容"，于是遍历、排版、命中、序列化全都要判断"这个节点有没有被删"，模型从此被污染。
 * 步骤叠加把修订留在文档之外 —— 树永远只表示"当前内容"，修订是视图态的叠加，与临时标记
 * 不污染真相是同一个思想。
 *
 * 基线的形状：一份独立的树值拷贝。整篇的快照在撤销那条路上是要避免的（回退快照会让整篇
 * 变脏，直接击穿增量排版），但修订留痕必须记住"这些编辑之前长什么样"，所以基线本来就得存，
 * 序列化时也要落盘（1.10）。
 *
 * 展开（expand）是这一步的核心：基线 + 全部未被拒绝的步骤按序应用。接受与拒绝只改状态，
 * 展开结果由它算出来，所以"接受与拒绝之后展开成什么样"是自足的、可手算比对的。
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/doc/node.h"
#include "core/doc/steps.h"

namespace heditor {

/**
 * 一条修订的状态。三态而不是布尔：待定与已拒绝都不是"已生效"，而已接受的修订已经与基线
 * 不可区分 —— 撤销菜单、界面的删除线呈现与落盘都要按这三种状态分开处理。
 */
enum class RevisionState : uint8_t {
    Pending = 0,   // 待定：展开时应用，界面上以作者颜色与删除线呈现
    Accepted = 1,  // 已接受：折叠进基线
    Rejected = 2,  // 已拒绝：展开时跳过
};

const char* revisionStateName(RevisionState state);

/**
 * 一条修订：一个步骤加上它的元数据。
 *
 * 步骤的坐标是"记录它那一刻的文档"里的坐标，不是基线的坐标。展开时按记录顺序逐步应用，
 * 所以坐标天然对得上；只有前面某条修订被拒绝时，才要把后面的坐标穿过它的逆步骤搬回来。
 */
struct Revision {
    Step step;
    std::string author;
    std::string time;
    RevisionState state = RevisionState::Pending;
};

/**
 * 这个步骤能不能作为一条修订记录。只认内容级的五个步骤（插入文本、插入片段、删除区间、
 * 插入段落标记、删除段落标记）—— 它们的位置全是流位置，能被映射搬动。结构步骤的位置含
 * Pos（槽位），搬不动，硬记下来只会在拒绝别的修订时错位，所以显式拒绝。
 */
bool isRevisionStep(const Step& step);

/**
 * 修订记录：一份基线加一串有序的修订。
 *
 * 不持有工作文档 —— 工作文档由调用方（命令层）改，这里只管"记了什么、展开成什么样"。
 * 于是模型层可以在没有界面的情况下把展开结果算出来并比对。
 */
class RevisionLog {
public:
    /**
     * 把当前文档记为基线，并清空全部修订。
     */
    void reset(const NodeTree& tree);

    bool hasBaseline() const { return has_baseline_; }

    /**
     * 记一条修订。只接受内容级步骤（见 isRevisionStep），其它种类返回失败并给原因。
     *
     * 它不应用步骤：工作文档由调用方改（与撤销栈同一条路），这里只记。分开的好处是
     * "记了什么"与"文档现在什么样"是两件事，展开时能各自独立地验。
     */
    StepResult record(const Step& step, const std::string& author, const std::string& time);

    /**
     * 接受一条修订：状态改成已接受，展开结果不变（它本来就在展开里）。
     *
     * 已接受的修订不能再被拒绝（它已经与基线不可区分），所以 reject 会挡掉它；反过来，
     * 接受一条已拒绝的修订是允许的，等于撤回拒绝。
     */
    StepResult accept(std::size_t index);
    StepResult reject(std::size_t index);

    const std::vector<Revision>& revisions() const { return log_; }
    int count() const { return static_cast<int>(log_.size()); }
    int count(RevisionState state) const;

    /**
     * 展开：把 out 置为"基线 + 全部未被拒绝的步骤"，按记录顺序应用，最后规范化一次。
     *
     * 规范化是为了让展开产物与正常编辑路径的产物可比 —— 正常编辑在事务提交时规范化（1.1），
     * 展开也应该产出同一种规范形状，否则两者只差在"同标记的相邻文本有没有合并"上。
     *
     * 前面某条修订被拒绝时，后面修订的坐标要穿过它的逆步骤搬回来（mapStepPositions，
     * 位置正好落在被恢复内容的插入点上时留在左边）。搬不动或某步应用不了时返回失败并写明
     * 是哪一条 —— 半个展开结果比"展开失败"难查得多。
     */
    StepResult expand(NodeTree& out) const;

    const NodeTree& baseline() const { return baseline_; }

private:
    NodeTree baseline_;
    bool has_baseline_ = false;
    std::vector<Revision> log_;
};

}  // namespace heditor
