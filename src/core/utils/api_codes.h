/**
 * 跨语言常量的唯一来源。
 *
 * 只放"两边真的会传的值"：纸张规格、日期单位位、图片版式码、编辑模式与标记档。
 * scripts/gen_codes.py 按每行一条记录的格式解析本文件，生成 web/src/codes.gen.ts；
 * 字段顺序不能动，加一档就加一行。
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace heditor::codes {

/**
 * 固定纸张的纵向宽高（磅）。
 */
struct PaperSize {
    /**
     * 规格名，与宿主配置文件里的写法一致。
     */
    const char* name;

    /**
     * 纵向宽（磅）。
     */
    float w_pt;

    /**
     * 纵向高（磅）。
     */
    float h_pt;
};

// 生成器按"一行一条记录"解析，所以这几张表不交给 clang-format 重排。
// clang-format off
/**
 * 纸张规格表。顺序即界面下拉里的顺序，横向由宿主把宽高对调。
 */
inline constexpr PaperSize kPaperSizes[] = {
    {"A4", 595.276f, 841.89f},
    {"A3", 841.89f, 1190.551f},
    {"A5", 419.528f, 595.276f},
    {"B5", 498.898f, 708.661f},
    {"Letter", 612.0f, 792.0f},
    {"Legal", 612.0f, 1008.0f},
};
// clang-format on

/**
 * 默认纸张在 kPaperSizes 里的下标（A4）。默认页度量与空白文档的纸张都从那一行取。
 */
inline constexpr size_t kDefaultPaperIndex = 0;

/**
 * 默认页边距（磅），四边相等，等于 2 厘米。
 */
inline constexpr float kDefaultMarginPt = 56.7f;

/**
 * 日期单位的位掩码。没列出的单位不进选择器；显示格式另由 format 决定。
 */
struct DateUnitBit {
    /**
     * 单位名，与宿主 API 里的写法一致。
     */
    const char* name;

    /**
     * 位掩码。
     */
    uint16_t bit;
};

// clang-format off
/**
 * 六个单位，顺序即界面里的先后。
 */
inline constexpr DateUnitBit kDateUnitBits[] = {
    {"year", 1u << 0},
    {"month", 1u << 1},
    {"day", 1u << 2},
    {"hour", 1u << 3},
    {"minute", 1u << 4},
    {"second", 1u << 5},
};
// clang-format on

/**
 * 年的位（其余同理）。模型层按名字用它们，不从下标取。
 */
inline constexpr uint16_t kDateUnitYear = 1u << 0;

/**
 * 月。
 */
inline constexpr uint16_t kDateUnitMonth = 1u << 1;

/**
 * 日。
 */
inline constexpr uint16_t kDateUnitDay = 1u << 2;

/**
 * 时。
 */
inline constexpr uint16_t kDateUnitHour = 1u << 3;

/**
 * 分。
 */
inline constexpr uint16_t kDateUnitMinute = 1u << 4;

/**
 * 秒。
 */
inline constexpr uint16_t kDateUnitSecond = 1u << 5;

/**
 * 图片版式的对外码。
 *
 * anchor 与 wrap 是模型层的两个字段，对外只有一个码：宿主的"版式"下拉只有一档可选，
 *   而两者必须一起改（行内只配 None，浮动才谈环绕）。
 */
struct ImageLayoutCode {
    /**
     * 版式名，与宿主 API 里的写法一致。
     */
    const char* name;

    /**
     * 对外的整数码。
     */
    int code;

    /**
     * ImageAnchor 的取值：0 行内 / 1 浮动。
     */
    int anchor;

    /**
     * ImageWrap 的取值：0 上下 / 1 四周 / 2 衬下 / 3 浮上。
     */
    int wrap;
};

// clang-format off
/**
 * 五种版式，顺序即界面下拉里的顺序。
 */
inline constexpr ImageLayoutCode kImageLayouts[] = {
    {"inline", 0, 0, 0},
    {"square", 1, 1, 1},
    {"topAndBottom", 2, 1, 0},
    {"behind", 3, 1, 2},
    {"inFront", 4, 1, 3},
};
// clang-format on

/**
 * 一个"名字对应引擎整数"的档位。
 */
struct EnumCode {
    /**
     * 档位名，与宿主 API 里的写法一致。
     */
    const char* name;

    /**
     * 引擎侧整数。
     */
    int code;
};

// clang-format off
/**
 * 编辑模式（与 EditMode 枚举同序）。
 */
inline constexpr EnumCode kEditModes[] = {
    {"document", 0},
    {"form", 1},
    {"readOnly", 2},
};
// clang-format on

// clang-format off
/**
 * 不可见字符标记（与 MarksMode 枚举同序）。
 */
inline constexpr EnumCode kMarksModes[] = {
    {"none", 0},
    {"paragraph", 1},
    {"all", 2},
};
// clang-format on

}  // namespace heditor::codes
