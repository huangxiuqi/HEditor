/**
 * 能力裁剪表：三档模式下"光标能落在哪、一次编辑能不能发生、控件能不能被结构操作"。
 *
 * 三档模式都不改文档结构（见 docs/文档模型设计.md 第七节）：表单模式只是裁掉哪些位置可编辑，
 * 而不是把不可编辑的部分从文档里挖掉。所以这一层是一组纯查询，全部只读修订。
 *
 * 落点判定只在表单模式下受限。可编辑集合是一个交集：包住这个位置的每一层内容控件都允许，位置
 * 才可编辑，于是只读控件里再嵌一个可编辑控件时那个内层位置仍不可编辑。方向键普通移动与 Tab 的
 * 目标都必须落在集合内，点击落在集合外时不移动光标。选区与复制不受限 —— 端点是读的需求，可以
 * 跨出控件，与能不能写无关。
 *
 * 位置是流位置，坐标口径随模式变：表单与只读模式下隐藏的控件整棵子树不在流里，所以这一层按
 * 模式取可见性（编辑态用 Design，其余用 Display）换算。
 */
#pragma once

#include <string>

#include "core/doc/edit_mode.h"
#include "core/doc/flow.h"
#include "core/doc/node.h"
#include "core/doc/range.h"
#include "core/doc/steps.h"
#include "core/rev/flow.h"
#include "core/rev/rev.h"

namespace heditor::rev {

/**
 * 一次判定的回执。不允许时给出原因，说明是哪一层挡住的 —— 越界交互要靠它反馈，
 * 静默拒绝会让"点了没反应"变成 bug。
 */
struct EditCheck {
    bool allowed = false;
    std::string reason;
};

EditCheck canEditAt(const Doc& doc, EditMode mode, FlowPos pos);

EditCheck canEditRange(const Doc& doc, EditMode mode, Range range);

/**
 * 方向键普通移动一步。表单模式下若相邻位置落在可编辑集合外，就沿该方向找到最近的可编辑
 * 位置；找不到则原地不动。
 */
FlowPos stepEditable(const Doc& doc, EditMode mode, FlowPos from, int dir);

/**
 * Tab 的目标。
 */
struct TabStop {
    bool handled = false;  // 编辑器是否消费这次 Tab
    FlowPos target = 0;    // handled 为真时的目标位置
    /**
     * 目标所属的控件。共享边界上"前一个控件的末尾"与"这个控件的开头"是同一个码点偏移，
     * 只给位置的话调用方再推一次归属会推成前一个控件。
     */
    NodeId owner = kInvalidNodeId;
};

TabStop tabFrom(const Doc& doc, EditMode mode, FlowPos from, bool backward);

/**
 * 结构操作（删除控件自身这类）能不能做。文档模式放行，只读模式一律拒绝，
 * 表单模式要求控件显式开放（structureLock 为 false）。
 */
EditCheck canDoStructureOp(const Doc& doc, EditMode mode, NodeId id);

/**
 * 一个具体步骤能不能做。把落点判定接到步骤上，于是"改一个字"与"删掉一个控件"共用同一套
 * 权限。步骤本身只回答"结构上能不能应用"，权限与它分开，两件事互不代替。
 */
EditCheck canEditStep(const Doc& doc, EditMode mode, const Step& step);

}  // namespace heditor::rev
