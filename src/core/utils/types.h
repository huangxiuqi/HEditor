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

/**
 * 页号加页内矩形（页面坐标，磅）。
 *
 * 为什么从引擎里提出来：光标矩形、选区矩形、控件矩形都是这个形状，而光标与选区那一层
 * （CaretResolver）不该反过来依赖引擎头。提成公共类型之后，Engine 侧保留一个同名别名，
 * 既有的 EditorEngine::PageRect 调用点不必动。
 */
struct PageRect {
    int page = 0;
    Rect r_pt{};
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace heditor
