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
#include "core/layout/layout.h"
#include "core/doc/document.h"

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
 * 一段中文横排正文块：书写方向、语言、颜色三项固定，字号与行距随场景给。
 *
 * 段后距取 BlockSpec 的默认值 —— 演示与压力文档各处一致，不必逐处声明。
 * 格式逐处手写时漏一项，两份样例的行高与页数就会不同，而症状只表现为
 * "某一份样例多了一页"。
 *
 * @param text 段文本（UTF-8）
 * @param cjk_font_index 中文字体下标
 * @param size_pt 字号（磅）
 * @param line_factor 行距倍数（以单倍行高为基准）
 * @returns 块
 */
BlockSpec cjkBlock(std::string text, int cjk_font_index, float size_pt, float line_factor);

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
 *
 * @param blocks 块流
 * @param fields 字段布局
 * @returns 修订
 */
doc::Doc demoDoc(const std::vector<BlockSpec>& blocks, const std::vector<DemoFieldSpec>& fields);

/**
 * 表格场景的演示文书。
 * @param cjk_font_index 中文字体下标
 * @returns 修订
 */
doc::Doc demoTableDoc(int cjk_font_index);

/**
 * 按场景号装配演示块流。
 *
 * 只造数据、不动引擎 —— 装载统一走宿主的 rebuildDocument。演示内容归这里：宿主入口
 *   只负责把场景号翻成调用，不带任何一段样例文本。
 *
 * @param scenario 场景号
 * @param cjk_font_index 中文字体下标
 * @param mongol_font_index 蒙文字体下标
 * @param latin_font_index 拉丁回退字体下标；-1 = 关闭回退
 * @returns 块流
 */
std::vector<BlockSpec> scenarioBlocks(int scenario, int cjk_font_index, int mongol_font_index,
                                      int latin_font_index = -1);

/**
 * 压力文档：paras 段、每段 chars 个汉字。
 *
 * 长度可调是为性能判据：演示场景只有一段，量出来的编译与索引耗时看不出 O(文档) 那一层，
 *   也证明不了增量省了多少。同一份代码在 10 段与 1000 段上的差别才是可证伪的证据。
 *
 * @param paras 段数（小于 1 按 1）
 * @param chars 每段字数（小于 1 按 1）
 * @param diverse 为真时按 800 个汉字轮转，否则整篇同一个字
 * @param cjk_font_index 中文字体下标
 * @returns 块流
 */
std::vector<BlockSpec> stressDocument(int paras, int chars, bool diverse, int cjk_font_index);

/**
 * 按换行切段造文档：每段一个块，整体重复 repeat 遍。
 *
 * 空串给一份"只有一个空段落"的文档 —— 那是"清空文档"唯一的入口，块流为空的话
 *   引擎里就没有可编辑的段落。
 *
 * @param text 段落文本（UTF-8，按换行切段）
 * @param repeat 整体重复遍数（小于 1 按 1）
 * @param cjk_font_index 中文字体下标
 * @returns 块流
 */
std::vector<BlockSpec> textDocument(const std::string& text, int repeat, int cjk_font_index);

/**
 * 表单样例：块流与字段布局。
 */
struct DemoFormSample {
    /**
     * 块流。
     */
    std::vector<BlockSpec> blocks;

    /**
     * 字段布局。
     */
    std::vector<DemoFieldSpec> fields;
};

/**
 * 表单样例的块流与字段布局，一次给全。
 *
 * 字段区间按文本的码点坐标算，与块流分居两处时改一个字就会整组错位 —— 而症状是
 *   "控件套到了错的字上"，控件数据本身看着完全正常，没有判据能直接指出偏的是哪一处。
 *
 * variant 0 只读字段夹在可编辑字段之间，看区间外打不进字与跨只读区右移；
 *   1 跨行长字段内嵌只读小字段，看控件不是矩形与内层策略优先；
 *   2 三个可编辑字段紧邻共享边界，看在边界上打字与把中间那段删空；
 *   3 三层嵌套且都可编辑，看归属取最内层与内层一动外层跟着长。
 *
 * @param variant 变体号（0 到 3，越界按 0）
 * @param cjk_font_index 中文字体下标
 * @param mongol_font_index 蒙文字体下标
 * @param latin_font_index 拉丁回退字体下标；-1 = 关闭回退
 * @returns 块流与字段布局
 */
DemoFormSample formSample(int variant, int cjk_font_index, int mongol_font_index,
                          int latin_font_index = -1);

}  // namespace heditor
