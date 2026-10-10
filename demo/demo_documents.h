/**
 * 演示与测试共用的固定文书。
 *
 * 不属于引擎，所以不编进 ced_core：它是"喂给引擎的样例内容"，
 * 被两个宿主入口（原生 / WASM）与两个测试文件共用。核心库里躺着演示数据，
 * 会让"这个库到底提供什么"变得含糊，也会让宿主以为它是产品能力的一部分。
 */
#pragma once

#include <vector>

#include "core/layout/layout.h"

namespace ced {

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

}  // namespace ced
