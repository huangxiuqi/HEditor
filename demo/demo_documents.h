/**
 * 演示与测试共用的固定文书。
 *
 * 不属于引擎，所以不编进 heditor_core：它是"喂给引擎的样例内容"，
 * 被两个宿主入口（原生 / WASM）与两个测试文件共用。核心库里躺着演示数据，
 * 会让"这个库到底提供什么"变得含糊，也会让宿主以为它是产品能力的一部分。
 */
#pragma once

#include <string>
#include <vector>

#include "core/doc/control.h"
#include "core/doc/node.h"
#include "core/layout/layout.h"

namespace heditor {

/**
 * 样例文书：中文横排 → 蒙文 vertical-lr（列序从左往右）→ 蒙文 vertical-rl（对照）
 * latin_font_index >= 0 时，中文块挂上拉丁字体作为回退（上标数字等中文子集缺字）
 */
std::vector<BlockSpec> sampleDocument(int cjk_font_index, int mongol_font_index,
                                      int latin_font_index = -1);

/**
 * 中英文混排块（病历里最常见的形态：中文叙述 + 英文缩写/数值/单位/拉丁术语）。
 * 用于验证同一段落里 CJK 与 Latin 的断行与度量是否正确。
 * latin_font_index 给出后，把它作为回退字体挂上 —— "10⁹/L" 的上标 9 正是靠它兜住。
 */
BlockSpec mixedParagraphBlock(int cjk_font_index, int latin_font_index = -1);

/**
 * 演示表单里一个字段怎么摆在树里：第几段、码点区间 [so, eo)、业务种类与编码、是否只读。
 *
 * 与引擎侧那个按 TextPos 记区间的旧表示（Control）用途相同，差别是它描述的是"树里怎么切"。
 * 这里的 so/eo 是段内偏移加段号，于是这份演示数据按约定只摆段内字段 —— 那是演示的简化，
 * 模型本身允许控件跨段（见 node.h 的 ContentControl）。业务种类走 ControlKind，
 * 建树时翻成 controlType 字符串，与引擎侧 controlKindFromBusinessType 那条映射互为逆。
 */
struct DemoFieldSpec {
    int para = 0;
    int so = 0;
    int eo = 0;
    ControlKind kind = ControlKind::TextField;
    std::string key;
    bool readonly = false;
};

/**
 * 把一份块流连同块级格式建成模型树：每块 = 一个文本节点 + 一枚段落结束标记。
 *
 * 块级字段（字体、字号、书写方向、行距、段后距、分页、语言、颜色、回退链）逐项搬到标记的
 * 段落格式上 —— 编译正是从"标记 + 样式表"再解析出这些字段的，所以搬过去之后编译回来的块
 * 与原来逐字段相同。fields 指的区间换成内容控件，嵌套的按"外层先开"建。
 *
 * 它存在的理由是宿主从"拼块流"切到"建树交给引擎"必须产出同一份排版：没有这份等价，
 * 切换本身就是一次静默的显示回归。判据在 tests/edit_test.cpp 的等价一节。
 */
NodeTree demoTree(const std::vector<BlockSpec>& blocks, const std::vector<DemoFieldSpec>& fields);

}  // namespace heditor
