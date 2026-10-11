/**
 * 事务与撤销分组（第五节）。
 *
 * 一个事务是一次可撤销的编辑单位：一串步骤、编辑前后的两份选区、一句提交说明。撤销就是按逆序
 * 应用每个步骤的逆操作 —— 不需要文档快照。快照式撤销要保存整篇文档，更要紧的是回退到快照会
 * 让整篇变脏，直接击穿增量排版，而增量是这个项目最贵的那部分资产。
 *
 * 合并策略放在事务层，步骤本身保持无状态：相邻的同类编辑（连续输入）并成一个撤销单位，用户按
 * 一次撤销回退一整段输入而不是一个字。步骤不参与这个决定，所以加一条合并规则不必动步骤。
 *
 * 选区快照与位置映射是两条互补的路（第四节末尾）：映射管一次编辑之内位置的跟随，选区快照管
 * 跨撤销单位回到原处。撤销恢复到事务的编辑前选区，重做回到编辑后。
 *
 * 规范化只在提交时跑一次（1.1），不参与步骤。它合并同标记的相邻文本节点、清理零宽 run，
 * 两者都不改变流位置，所以提交之后逆操作用的坐标依然成立 —— 撤销能在规范化之后的文档上工作。
 */
#pragma once

#include <string>
#include <vector>

#include "core/doc/node.h"
#include "core/doc/range.h"
#include "core/doc/steps.h"

namespace heditor {

/**
 * 一次可撤销的编辑单位。
 *
 * 步骤按顺序应用，每步的坐标是它自己那一步开始时的状态：逆序应用逆操作正好回到原处。
 */
struct Transaction {
    std::vector<Step> steps;
    Range selection_before;
    Range selection_after;
    std::string label;  // 提交说明，撤销菜单显示用
};

/**
 * 两个事务能不能并成一个撤销单位。
 *
 * 约定只合并"连续输入"：旧事务整串都是 InsertText、标记与新的一笔相同、新的一笔紧接在旧串
 * 末尾。其它情况一律分开 —— 撤销粒度宁可细一点，也不要把两件不同的事并成一件。
 */
bool canMerge(const Transaction& older, const Transaction& newer);

/**
 * 撤销栈。持有文档的引用，提交时由它应用步骤（调用方负责构造步骤与两份选区）。
 */
class UndoStack {
public:
    explicit UndoStack(NodeTree& tree) : tree_(tree) {}

    /**
     * 提交一个事务：应用步骤、规范化一次、并入栈顶或压栈，并清空重做栈。
     *
     * 中途有步骤失败时整个事务作废：已经应用的按逆序撤回，文档与栈都回到提交前。
     */
    StepResult commit(Transaction tx);

    /**
     * 撤销一个事务，并把该事务的编辑前选区写进 restore。没有可撤销的事务时返回失败。
     */
    StepResult undo(Range* restore);

    /**
     * 重做，并把编辑后选区写进 restore。
     */
    StepResult redo(Range* restore);

    bool canUndo() const { return !done_.empty(); }
    bool canRedo() const { return !undone_.empty(); }
    int undoDepth() const { return static_cast<int>(done_.size()); }
    int redoDepth() const { return static_cast<int>(undone_.size()); }

private:
    /**
     * 按逆序撤回若干个已应用的步骤。
     */
    void revert(const std::vector<Step>& applied);

    NodeTree& tree_;
    std::vector<Transaction> done_;
    std::vector<Transaction> undone_;
};

}  // namespace heditor
