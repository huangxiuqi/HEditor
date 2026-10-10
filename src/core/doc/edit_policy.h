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
     *
     * @param preferred >= 0 时表示"编辑已经明确属于这个控件"（点画布得到的结果，
     *   见 EditorEngine::setCaretAt）。共享边界上两个相邻字段都覆盖同一个位置，
     *   只看最内层会恒定选中下标更小的那个（前一个字段）—— 于是"点后一个字段的开头"
     *   虽然光标停住了，字却会落进前一个字段；前一个字段只读时更糟：直接拒绝。
     *   默认 -1 = 没有侧别信息，按最内层判（键盘、宿主 API 走这条路）。
     */
    static bool canInsert(const ControlSet& ctrls, EditMode mode, const TextPos& pos,
                          int preferred = -1);

    /**
     * 删除 [a, b) 是否允许。
     * 要求整个删除区间完整落在同一个可编辑控件内 ——
     * 这一条同时挡掉了两种越界：从控件外往里删、从控件里往外删。
     * 只检查两个端点会漏掉"中间跨了一个只读控件"的情况。
     *
     * 刻意不看 preferred：删除的归属由"被删掉的那些字属于谁"决定，
     * 与光标停在边界的哪一侧无关 —— 在字段开头退格删的是前一个字段的最后一个字，
     * 那是用户按下去的那一下想删的东西（真要禁止得另立一条"退格不出字段"的规则，
     * 不能混进归属里）。
     */
    static bool canErase(const ControlSet& ctrls, EditMode mode, const TextPos& a,
                         const TextPos& b);

    /**
     * 在 pos 处插入一个段落分隔符（回车分段）是否允许。
     *
     * 判据就是 canInsert(pos) —— 回车等于"在断点处插入一个段落分隔符"，
     * 它能不能插由插入策略决定，不需要为它单开一套规则：
     * 只读模式一律 false；表单模式下断点必须落在可编辑控件里（或所有控件之外）。
     * 拆分不会让任何文字离开它所属的控件：区间跟着内容走
     * （见 adjustControlsOnSplitPara）。
     */
    static bool canSplit(const ControlSet& ctrls, EditMode mode, const TextPos& pos,
                         int preferred = -1);

    /**
     * 删掉 join 处的段落分隔符（把后一段并进前一段）是否允许。
     * join 取前一段的末尾（= 分隔符所在位置），与"光标停在段末算不算在字段内"
     * 同一口径（include_end = true）：于是"把只读字段后面的段落并进只读字段"会被拒绝，
     * 而"把可编辑字段后面的段落并进来"允许。
     * 合并只会把后一段的文字搬到前一段末尾，控件区间跟着搬 —— 只读保护不会被绕过。
     */
    static bool canMerge(const ControlSet& ctrls, EditMode mode, const TextPos& join,
                         int preferred = -1);

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
