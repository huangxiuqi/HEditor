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
    /**
     * 允不允许。
     */
    bool allowed = false;

    /**
     * 不允许的原因（给界面做禁用态提示）。
     */
    std::string reason;
};

/**
 * 这个流位置是不是落在"光标屏障"控件的内容里。
 *
 * 四种选择控件（单选、复选、单选组、复选组）是一整块：它们的内容是"图形 + 标签"的投影，
 * 不是可以打字的正文，所以光标不许停在里面 —— 三档模式都成立。选区不受限：端点走到屏障
 * 内部是读的需求（复制一个标签片段），与能不能写无关。
 *
 * 判的是内容区间 [开括号之后, 闭括号之前]，两枚括号本身不在里面：光标停在开括号之前、
 * 闭括号之后都合法，那正是"贴着这一块的前后"两个插入点。
 *
 * 索引必须由调用方给：流位置的口径随可见性变（表单模式摘掉隐藏子树），拿另一份索引
 * 去查等于拿两个坐标系对位。
 * @param doc 修订
 * @param idx 与调用方同一口径的流索引
 * @param pos 流位置
 * @return true = 落在屏障内容里
 */
bool caretBarrierAt(const Doc& doc, const FlowIndex& idx, FlowPos pos);

/**
 * 某个位置能不能落编辑（表单模式下要落在可编辑集合内）。
 * @param doc 修订
 * @param mode 编辑模式
 * @param pos 流位置
 * @return 允许不允许，不允许时带原因
 */
EditCheck canEditAt(const Doc& doc, EditMode mode, FlowPos pos);

/**
 * 一个区间能不能整段删掉（两端都要在可编辑集合内）。
 * @param doc 修订
 * @param mode 编辑模式
 * @param range 区间
 * @return 允许不允许，不允许时带原因
 */
EditCheck canEditRange(const Doc& doc, EditMode mode, Range range);

/**
 * 方向键普通移动一步。表单模式下若相邻位置落在可编辑集合外，就沿该方向找到最近的可编辑
 * 位置；找不到则原地不动。
 * @param doc 修订
 * @param mode 编辑模式
 * @param from 当前位置
 * @param dir 方向（负 = 向前）
 * @return 最近的可编辑位置；找不到时为原地
 * @param doc 修订
 * @param mode 编辑模式
 * @param from 当前位置
 * @param dir 方向（负 = 向前）
 * @return 最近的可编辑位置；找不到时为原地
 * @param doc 修订
 * @param mode 编辑模式
 * @param from 当前位置
 * @param dir 方向（负 = 向前）
 * @return 最近的可编辑位置；找不到时为原地
 */
FlowPos stepEditable(const Doc& doc, EditMode mode, FlowPos from, int dir);

/**
 * Tab 的目标。
 */
struct TabStop {
    /**
     * 编辑器消费了这个 Tab 没有。
     */
    bool handled = false;

    /**
     * handled 为真时的目标位置。
     */
    FlowPos target = 0;

    /**
     * 目标所属的控件。共享边界上"前一个控件的末尾"与"这个控件的开头"是同一个码点偏移，
     * 只给位置的话调用方再推一次归属会推成前一个控件。
     */
    NodeId owner = kInvalidNodeId;
};

/**
 * Tab 从当前位置去哪儿（到最后一个可编辑控件就放行，不绕回）。
 * @param doc 修订
 * @param mode 编辑模式
 * @param from 当前位置
 * @param backward true = Shift+Tab（往前找）
 * @return 消费没有、目标位置与归属
 */
TabStop tabFrom(const Doc& doc, EditMode mode, FlowPos from, bool backward);

/**
 * 结构操作（删除控件自身这类）能不能做。文档模式放行，只读模式一律拒绝，
 * 表单模式要求控件显式开放（structureLock 为 false）。
 * @param doc 修订
 * @param mode 编辑模式
 * @param id 控件节点号
 * @return 允许不允许，不允许时带原因
 * @param doc 修订
 * @param mode 编辑模式
 * @param id 控件节点号
 * @return 允许不允许，不允许时带原因
 */
EditCheck canDoStructureOp(const Doc& doc, EditMode mode, NodeId id);

/**
 * 一个具体步骤能不能做。把落点判定接到步骤上，于是"改一个字"与"删掉一个控件"共用同一套
 * 权限。步骤本身只回答"结构上能不能应用"，权限与它分开，两件事互不代替。
 * @param doc 修订
 * @param mode 编辑模式
 * @param step 一步编辑
 * @return 允许不允许，不允许时带原因
 */
EditCheck canEditStep(const Doc& doc, EditMode mode, const Step& step);

}  // namespace heditor::rev
