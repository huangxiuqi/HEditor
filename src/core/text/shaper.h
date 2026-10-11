/**
 * HarfBuzz 整形封装。
 * 职责边界（这点很重要）：
 * 本层只做"沿一条基线把一串文本整形出来"——字形选择、连写、上下文字形、
 * 竖排 feature（vert/vrt2/vkrn/vpal）、字距。不做断行、不做分页、不做列序。
 * 列序与断行属于布局层（core/layout），由 WritingMode 驱动。
 * 蒙文能走通的关键就在这：蒙文属上下文连写文字（词首/词中/词末/独立形态、
 * 连字、元音和谐的阴阳词形、FVS 自由变体选择符 U+180B-180D），
 * HarfBuzz 的 Arabic 整形模型覆盖 Mongolian，这些全部由它处理。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

struct hb_font_t;

namespace heditor {

struct WritingMode;

struct ShapedGlyph {
    uint32_t glyph_id = 0;
    int32_t x_advance = 0;  // scale 单位（1/kFontScalePerPt 磅）
    int32_t y_advance = 0;
    int32_t x_offset = 0;
    int32_t y_offset = 0;
    /**
     * 该字形溯源的码点下标（相对于喂进来的码点数组）。
     * 这里从"UTF-8 字节偏移"改成了"码点下标"，是整个档 1 的枢纽：
     * 喂 hb_buffer_add_utf32 时 HarfBuzz 原样回传本实现给的索引，于是
     * cluster 与文档偏移是同一把尺子，中间不再需要映射表。
     * 是"按码点决策、吐字节区间、靠 += byte_begin 硬接"，两套坐标穿行。
     */
    uint32_t cluster = 0;
};

struct ShapedText {
    std::vector<ShapedGlyph> glyphs;
    /**
     * 整形方向是 TTB（advance 取 y_advance 且为负）。侧倒模式为 false，
     * 因为它走横排整形、取 x_advance。
     */
    bool vertical = false;
    /**
     * 侧倒模式（蒙文竖排）：字形本身是横排形态，需要由布局/渲染层顺时针转 90°。
     */
    bool sideways = false;
    int32_t inline_advance = 0;  // 沿行内推进轴的总推进（scale 单位，恒为正值）

    bool empty() const { return glyphs.empty(); }
};

/**
 * 整形一段文本（码点数组）。language 传 nullptr 表示不指定（会影响 locl 之类的语言特性）。
 * 主入口是码点版：它让 ShapedGlyph::cluster 成为码点下标，与文档偏移一致。
 */
ShapedText shapeText(hb_font_t* font, const uint32_t* cps, size_t count, const WritingMode& wm,
                     const char* language = nullptr);

/**
 * 从码点数组的 [begin, end) 区间整形，但保持 cluster 是相对整个数组的下标。
 * 为什么必须有这个入口，而不是让调用方事后做 g.cluster += begin：
 * font fallback 把一段文本切成多个 run，每个 run 单独整形。若直接把 cps + begin
 * 交给 HarfBuzz，它给出的 cluster 是从 0 开始的相对下标，调用方就必须记得把那半段
 * 偏移加回去 —— 一份"记得就没事、忘了就静默错位"的隐形契约。
 * 而 hb_buffer_add_utf32 本身支持 item_offset：把整串指针和 [begin, end) 传进去，
 * HarfBuzz 会以 item_offset 为基准编号 cluster，偏移算术于是从调用方消失。
 */
ShapedText shapeTextRange(hb_font_t* font, const uint32_t* all, size_t begin, size_t end,
                          const WritingMode& wm, const char* language = nullptr);

/**
 * 便捷重载：UTF-8 进、内部转码点再整形。仅供"一次性看一眼"的诊断路径使用
 * （describeShaping / 单测），主链路一律走码点版，避免来回转码。
 */
ShapedText shapeText(hb_font_t* font, std::string_view utf8, const WritingMode& wm,
                     const char* language = nullptr);

}  // namespace heditor
