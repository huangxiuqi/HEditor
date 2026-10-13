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
 * 演示表单里一个字段怎么摆在树里：起点 (para, so) 与终点 (epara, eo) 之间的那一段文档流、
 * 业务种类与编码、是否只读。
 *
 * epara 省略（小于 0）即"同段"，终点落在 para 段里的 eo 处 —— 大多数演示字段如此。
 * epara 大于 para 时这个字段跨段：中间那些段落连同它们的结束标记都落在它里面，
 * 这正是模型层允许的形状（见 node.h 的 ContentControl；控件可以跨段）。
 * so/eo 是段内码点偏移；业务种类走 ControlKind，建树时翻成 controlType 字符串，
 * 与引擎侧 controlKindFromBusinessType 那条映射互为逆。
 */
struct DemoFieldSpec {
    int para = 0;
    int so = 0;
    int epara = -1;  // 终点所在段；< 0 = 与起点同段
    int eo = 0;
    ControlKind kind = ControlKind::TextField;
    std::string key;
    bool readonly = false;
};

/**
 * 字段的终点段号：epara 没给就是起点那一段。
 */
inline int endParaOf(const DemoFieldSpec& f) { return f.epara < 0 ? f.para : f.epara; }

/**
 * 把一份块流连同块级格式建成模型树：每块 = 一个文本节点 + 一枚段落结束标记。
 *
 * 块级字段（字体、字号、书写方向、行距、段后距、分页、语言、颜色、回退链）逐项搬到标记的
 * 段落格式上 —— 编译正是从"标记 + 样式表"再解析出这些字段的，所以搬过去之后编译回来的块
 * 与原来逐字段相同。fields 指的区间换成内容控件，嵌套的按"外层先开"建。
 *
 * 每段自己的结束标记默认排在它内容之后、与字段并列；某个字段跨段时，中间那些段落连同
 * 它们的标记就落在这个字段里面 —— 那是跨段控件唯一可能的形状（容器的孩子必须连续）。
 */
NodeTree demoTree(const std::vector<BlockSpec>& blocks, const std::vector<DemoFieldSpec>& fields);

}  // namespace heditor
