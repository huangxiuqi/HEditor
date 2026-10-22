/**
 * 事务与撤销分组（第五节）。
 *
 * 一个事务是一次可撤销的编辑单位：一串步骤、编辑前后的两份选区、一句提交说明。撤销就是按逆序
 * 应用每个步骤的逆操作 —— 不需要文档快照。快照式撤销要保存整篇文档，更要紧的是回退到快照会
 * 让整篇变脏，直接击穿增量排版，而增量是这个项目最贵的那部分资产。
 *
 * 撤销粒度 = 一个事务：一笔编辑就是一个撤销单位，不做任何合并。
 *
 * 不合并的理由是粒度要一致：只对插入生效的合并规则会让同一件事有两种粒度 —— 连续敲五个字
 * 一步退掉，连续退五个字却要五步（退格是另一种步骤）。粒度宁可细一点，也不要把两件事并成。
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
    /**
     * 这一笔的全部步骤（按顺序应用，撤销按逆序应用逆操作）。
     */
    std::vector<Step> steps;

    /**
     * 编辑前的选区（撤销回到它）。
     */
    Range selection_before;

    /**
     * 编辑后的选区（重做回到它）。
     */
    Range selection_after;

    /**
     * 提交说明，撤销菜单显示用。
     */
    std::string label;
};

/**
 * 撤销栈。持有文档的引用，提交时由它应用步骤（调用方负责构造步骤与两份选区）。
 */
class UndoStack {
public:
    explicit UndoStack(NodeTree& tree) : tree_(tree) {}

    /**
     * 撤销栈的深度上限（条数）。0 或负数表示不限制。
     *
     * 调小时立刻按上限裁剪（丢最旧的），不必等下一次提交 —— 否则"把上限调小"要等到用户再敲
     * 一下才生效，那一刻栈还超着限。
     * @param limit 最多留几条（0 或负数表示不限制）
     */
    void setLimit(int limit);

    /**
     * 撤销栈的深度上限。
     * @return 上限（0 或负数表示不限制）
     */
    int limit() const { return limit_; }

    /**
     * 提交一个事务：应用步骤、规范化一次、压栈（超出上限时丢最旧的），并清空重做栈。
     *
     * 中途有步骤失败时整个事务作废：已经应用的按逆序撤回，文档与栈都回到提交前。
     * @param tx 事务（按值收下，提交之后归栈）
     * @return 结果；中途失败时整笔作废、文档回到提交前
     */
    StepResult commit(Transaction tx);

    /**
     * 撤销一个事务，并把该事务的编辑前选区写进 restore。没有可撤销的事务时返回失败。
     * @param restore 写回编辑前选区
     * @return 结果；没有可撤销的事务时失败
     */
    StepResult undo(Range* restore);

    /**
     * 重做，并把编辑后选区写进 restore。
     * @param restore 写回编辑后选区
     * @return 结果；没有可重做的事务时失败
     */
    StepResult redo(Range* restore);

    /**
     * 有没有可撤销的事务。
     * @return true = 撤销栈非空
     */
    bool canUndo() const { return !done_.empty(); }

    /**
     * 有没有可重做的。
     * @return true = 重做栈非空
     */
    bool canRedo() const { return !undone_.empty(); }

    /**
     * 撤销栈里的事务数。
     * @return 条数
     */
    int undoDepth() const { return static_cast<int>(done_.size()); }

    /**
     * 重做栈里的事务数。
     * @return 条数
     */
    int redoDepth() const { return static_cast<int>(undone_.size()); }

private:

    /**
     * 按逆序撤回若干个已应用的步骤。
     * @param applied 已经应用的步骤（按应用顺序给，这里逆序撤回）
     */
    void revert(const std::vector<Step>& applied);

    /**
     * 超出上限时丢最旧的。重做栈不用单独裁：它只由撤销栈弹出来，规模不可能超过上限。
     */
    void trim();

    /**
     * 被编辑的树（不持有）。
     */
    NodeTree& tree_;

    /**
     * 已提交的事务，越靠后越新。
     */
    std::vector<Transaction> done_;

    /**
     * 撤销掉的事务，重做按它回去。
     */
    std::vector<Transaction> undone_;

    /**
     * 撤销栈的上限。0 表示不限制。
     */
    int limit_ = 0;
};

}  // namespace heditor
