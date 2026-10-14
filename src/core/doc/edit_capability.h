/**
 * 能力裁剪表：三档模式下"光标能落在哪、一次编辑能不能发生、控件能不能被结构操作"。
 *
 * 三档模式都不改文档结构（见 docs/文档模型设计.md 第七节）：表单模式只是裁掉哪些位置可编辑，
 * 而不是把不可编辑的部分从文档里挖掉。所以这一层是一组纯查询，全部只读树 —— 段落、命中、坐标
 * 与脏段都建立在"流是完整的"这个前提上，挖掉一块会让模型与排版输入不一致。
 *
 * 落点判定只在表单模式下受限。可编辑集合是一个交集：包住这个位置的每一层内容控件都允许，位置
 * 才可编辑，于是只读控件里再嵌一个可编辑控件时那个内层位置仍不可编辑。方向键普通移动与 Tab 的
 * 目标都必须落在集合内，点击落在集合外时不移动光标。选区与复制不受限 —— 端点是读的需求，可以
 * 跨出控件，与能不能写无关。
 *
 * 位置是流位置，坐标口径随模式变：表单与只读模式下隐藏的控件整棵子树不在流里，所以这一层按
 * 模式取可见性（编辑态用 Design，其余用 Display）换算，调用方传进来的位置必须与该口径一致。
 *
 * 隐藏与可编辑集合是两件事，不要混：可编辑集合是谓词（结构不动），隐藏是作者声明的属性
 * （非编辑模式下把子树从流里摘除）。所以"位置在可编辑集合内"不等于"这个控件可见"。
 */
#pragma once

#include <string>

#include "core/doc/edit_mode.h"
#include "core/doc/flow.h"
#include "core/doc/node.h"
#include "core/doc/range.h"
#include "core/doc/steps.h"

namespace heditor {

/**
 * 一次判定的回执。不允许时给出原因，说明是哪一层挡住的 —— 越界交互要靠它反馈，
 * 静默拒绝会让"点了没反应"变成 bug。
 */
struct EditCheck {
    bool allowed = false;
    std::string reason;
};

/**
 * 光标能不能落在这里（插入动作也用它）。表单模式下就是"位置在可编辑集合内"。
 */
EditCheck canEditAt(const NodeTree& tree, EditMode mode, FlowPos pos);

/**
 * 一次编辑能不能落在区间上。空区间（光标处的插入）等同 canEditAt。
 *
 * 非空区间除了两端要可编辑，还不允许整块包住一个只读控件 —— 只查两端会漏掉"跨过只读控件"
 * 这一种，而它恰好能删掉只读控件的内容，把保护绕开。
 */
EditCheck canEditRange(const NodeTree& tree, EditMode mode, Range range);

/**
 * 方向键普通移动一步。表单模式下若相邻位置落在可编辑集合外，就沿该方向找到最近的可编辑
 * 位置；找不到则原地不动（跳不出集合）。文档与只读模式的光标落点不受限，就是相邻一步。
 *
 * 表单模式选择"跳过"而不是"顶住边界"，是为了不让键盘用户被困在单个控件里：顶住边界时，
 * 控件末尾按右键毫无反应，只能靠 Tab 出去。跳过仍然满足"落点不越出集合"。
 */
FlowPos stepEditable(const NodeTree& tree, EditMode mode, FlowPos from, int dir);

/**
 * Tab 的目标。
 */
struct TabStop {
    bool handled = false;  // 编辑器是否消费这次 Tab
    FlowPos target = 0;    // handled 为真时的目标位置
    /**
     * 目标所属的控件。带上它不是可有可无的：目标总是落在控件的内容起点上，而共享边界
     * 上"前面那个控件的末尾"与"这个控件的开头"是同一个码点偏移 —— 只给位置，调用方
     * 按"最内层含端点"再推一次归属，会推成前一个控件（同深度取下标小者），于是
     * "Tab 跳过去之后打不进字"。归属由这里给出，调用方不要再推。
     */
    NodeId owner = kInvalidNodeId;
};

/**
 * Tab / Shift+Tab。文档模式不消费 —— Tab 在那里该是制表符，但那一步还没落地（排版层没有制表位，
 * 见 EngineConfig::tab_spaces），所以文档模式的 Tab 目前由宿主当缩进处理；只读模式不消费 ——
 * 放行让焦点落到编辑器之外，吞掉 Tab 会把键盘用户困在编辑器里；表单模式在可编辑控件之间前进 /
 * 后退，已经没有下一个（或上一个）可编辑控件时同样放行。
 *
 * 这个能力目前没有默认键位：宿主的 Tab 一律走缩进（app.js 的 onKeyDown），谁都还没接它 ——
 * 表单模式的"Tab 跳下一个字段"要接的话，宿主一个分支就能接上。
 *
 * 可编辑控件的判据是"它的内容起点在可编辑集合内"，直接复用 canEditAt —— 于是"可编辑控件里
 * 嵌的只读控件"不会是 Tab 落点，反之亦然，两处口径不会各说各话。
 */
TabStop tabFrom(const NodeTree& tree, EditMode mode, FlowPos from, bool backward);

/**
 * 结构操作（删除控件自身这类）能不能做。
 * 文档模式放行 —— 作者在设计表单；只读模式一律拒绝；表单模式要求控件显式开放
 * （structureLock 为 false），默认锁住。增删行列这类操作的目标不是控件，开关挂在被操作的
 * 元素上，不在这里造通用的操作白名单。
 */
EditCheck canDoStructureOp(const NodeTree& tree, EditMode mode, NodeId id);

/**
 * 一个具体步骤能不能做。把落点判定接到步骤上，于是"改一个字"与"删掉一个控件"共用同一套
 * 权限：前者按可编辑集合判（区间两端都要在里面），后者按结构操作锁判。
 *
 * 表单模式下节点级步骤只对声明可删的控件开放 —— 填表不该插图片或表格，那些属于编辑态。
 * 步骤本身只回答"结构上能不能应用"（canApply），权限与它分开，两件事互不代替。
 */
EditCheck canEditStep(const NodeTree& tree, EditMode mode, const Step& step);

}  // namespace heditor
