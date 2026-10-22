/**
 * 编辑模式：文档 / 表单 / 只读三档。
 *
 * 三档是同一份文档上的策略差异，不是三套控件（见 docs/文档模型设计.md 第七节）：它们都不改
 * 文档结构，只裁三处能力 —— 光标能落在哪、能不能编辑、隐藏的控件是否参与流。模式值因此属于
 * 模型层：编辑能力（edit_capability）、可见性（compile）与宿主门面共用这一个定义，
 * 引擎只把它存成一个字段。
 */
#pragma once

namespace heditor {
/**
 * 三档模式。取值显式写出，宿主按整数传参时含义不会随枚举顺序改变。
 */
enum class EditMode : int {
    Document = 0,  // 文档模式：随便改（默认值）
    Form = 1,      // 表单模式：光标位置不受限，只有"能不能编辑"受可编辑控件约束
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
