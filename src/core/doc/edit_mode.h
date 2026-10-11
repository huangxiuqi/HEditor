/**
 * 编辑模式：文档 / 表单 / 只读三档。
 *
 * 三档是同一份文档上的策略差异，不是三套控件（见 docs/文档模型设计.md 第七节）：它们都不改
 * 文档结构，只裁三处能力 —— 光标能落在哪、能不能编辑、隐藏的控件是否参与流。模式值因此属于
 * 模型层，排版引擎与宿主门面共用这一个定义。
 *
 * 从引擎侧的 edit_policy.h 搬到这里：那个头依赖 ControlSet（按 TextPos 记区间的旧表示），
 * 而模式本身与那套表示无关。搬过来之后 edit_policy.h 反过来包含本头，调用方不受影响。
 */
#pragma once

namespace heditor {

/**
 * 三档模式。取值显式写出，宿主按整数传参时含义不会随枚举顺序改变。
 */
enum class EditMode : int {
    Document = 0,  // 文档模式：随便改（默认值，保证旧行为一字不变）
    Form = 1,      // 表单模式：光标只在可编辑控件内，越界编辑被拒
    ReadOnly = 2,  // 只读模式：光标自由移动（落点不受限），一个字都不许改
};

/**
 * 模式名，用于诊断输出。拿数字去对照枚举太费事。
 */
const char* editModeName(EditMode m);

/**
 * 从宿主传入的整数还原模式。未知取值回落到文档模式 —— 宿主传错时宁可放开，
 * 也不要让编辑器变成"一个字都打不进去"。
 */
EditMode editModeFromCode(int code);

}  // namespace heditor
