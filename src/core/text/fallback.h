/**
 * 字体回退：按"哪个字体能提供字形"把文本切段。
 * 单个字体不可能覆盖真实文档：NotoSansSC-VF 有 ¹²³（U+00B9/B2/B3），
 * 却没有 U+2070 与 U+2079。没有回退就是豆腐块。
 * 切段依据是 cmap 精确查询（hb_font_get_nominal_glyph），在整形之前定下来，
 * 于是每个 run 只整形一次；若改成"整形后发现 .notdef 再补"，需要整形两遍，
 * 且第二遍的 run 边界依赖第一遍的输出。
 *
 * 组合标记与变体选择符（含蒙文 FVS U+180B-180D）没有独立字形，必须跟随基字用同一
 * 字体整形，由 needsOwnGlyph() 排除。
 * 职责边界：本层只切段，不做整形、断行与列序。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace heditor {

class FontLibrary;

/**
 * 一段"必须由同一字体整形"的码点区间。
 * 单位是码点，不是字节 —— 与 ShapedText::cluster、与文档偏移保持一致。
 * 三者同一把尺子，切段结果才能直接和整形输出拼起来（是字节区间 + 码点决策，
 * 合并时得靠 g.cluster += byte_begin 硬接，能work全靠"两边边界恰好都落在码点起点上"）。
 */
struct FontRun {
    size_t cp_begin = 0;
    size_t cp_end = 0;    // 不含
    int face_index = -1;  // 命中的字体索引；-1 表示 chain 全都不支持（会用 chain[0] 出豆腐块）
};

/**
 * 该码点本身是否要求一个独立字形。
 * 返回 false 的码点必须"跟随前一个码点的字体"——否则会把组合标记/变体选择符
 * 与它的基字拆到两个字体里，连写与定位会散架（蒙文尤其致命）。
 */
bool needsOwnGlyph(uint32_t cp);

/**
 * 把码点数组按"chain 中第一个能提供字形的字体"切段。
 * chain[0] 是主字体。chain 为空时返回"整段都归 face_index=-1"。
 */
std::vector<FontRun> itemizeByFont(FontLibrary& lib, const uint32_t* cps, size_t count,
                                   const std::vector<int>& chain, float size_pt);

/**
 * 便捷重载：UTF-8 进，内部转码点。仅供诊断路径使用。
 */
std::vector<FontRun> itemizeByFont(FontLibrary& lib, std::string_view utf8,
                                   const std::vector<int>& chain, float size_pt);

}  // namespace heditor
