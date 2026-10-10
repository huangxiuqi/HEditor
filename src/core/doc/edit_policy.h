/**
 * 编辑策略：纯逻辑层，决定"这个位置能不能改"，不涉及任何几何。
 * 为什么把它单独成层，而不是塞进 EditorEngine：
 * 引擎负责"改了之后怎么重排、怎么重画"；策略负责"本次改动该不该被接受"。
 * 分开之后，"病历表单模式"这条需求就只是换一个策略对象 ——
 * 排版、分页、光栅、增量重绘一律不动。
 * 这一层存在的意义："只是为了在表单模式下加个编辑边界"。
 * 边界是策略，不是矩形 —— 因为它约束的是"哪些位置可改"，
 * 而"这些位置在屏幕上是什么形状"是布局的产物，随内容变。
 */
#pragma once

#include "core/doc/control.h"

namespace ced {

enum class EditMode : int {
    Document = 0,  // 文档模式：随便改（默认值，保证旧行为一字不变）
    Form = 1,      // 表单模式：只能在可编辑控件的区间里改
    ReadOnly = 2,  // 只读模式：一个字都不许改
};

const char* editModeName(EditMode m);
EditMode editModeFromCode(int code);

class EditPolicy {
public:
    void setMode(EditMode m) { mode_ = m; }
    EditMode mode() const { return mode_; }

    /**
     * 在 pos 处插入是否允许。
     */
    static bool canInsert(const ControlSet& ctrls, EditMode mode, const TextPos& pos);

    /**
     * 删除 [a, b) 是否允许。
     * 要求整个删除区间完整落在同一个可编辑控件内 ——
     * 这一条同时挡掉了两种越界：从控件外往里删、从控件里往外删。
     * 只检查两个端点会漏掉"中间跨了一个只读控件"的情况。
     */
    static bool canErase(const ControlSet& ctrls, EditMode mode, const TextPos& a,
                         const TextPos& b);

    /**
     * 光标能否停在这个插入点。
     * 只有表单模式会返回 false。文档模式与只读模式都放行 ——
     * "只读"约束的是编辑，不是浏览，不要混。
     */
    static bool canPlaceCaret(const ControlSet& ctrls, EditMode mode, const TextPos& pos);

    /**
     * 某个控件是否可编辑（供 UI 决定要不要画成"可写"的样子）。
     */
    static bool isEditable(const Control& c) { return !c.read_only; }

private:
    /**
     * 默认 Document —— 保证"没碰过这个开关的调用方"行为与引入前逐字节一致。
     */
    EditMode mode_ = EditMode::Document;
};

}  // namespace ced
