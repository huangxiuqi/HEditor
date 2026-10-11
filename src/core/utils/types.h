/**
 * 基础类型。全部与平台无关——这一层在原生和 WASM 上是同一份代码。
 */
#pragma once

#include <cstdint>

namespace heditor {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    static Color rgb(uint8_t r_, uint8_t g_, uint8_t b_) { return Color{r_, g_, b_, 255}; }

    /**
     * 逐字段相等。显式默认而不是靠调用方手写：字符属性集合要靠它判断"两个 run 的标记是否
     * 相同"，漏比一个通道就会把两个不同的颜色判成同一个。
     */
    bool operator==(const Color& o) const = default;
};

struct Point {
    float x = 0.0f, y = 0.0f;
};

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace heditor
