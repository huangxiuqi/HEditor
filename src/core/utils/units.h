/**
 * 长度量化。1/16 磅是引擎侧的坐标比较步长：指纹、脏页判定、缓存键都只认量化后的值。
 * 与 utils 里其他头并列，是因为它被排版与控件两处共用，不属于任何一层。
 */
#pragma once

#include <cmath>
#include <cstdint>

namespace ced {

/**
 * 把磅坐标量化到 1/16 磅再比较。
 * 不量化就会出问题：浮点末位抖动会让"内容没变"的页也判成脏页，于是每一页都重画，
 * 增量就等于没做。这类 bug 的表象是"看着对，但数字没下来"。
 */
inline int32_t quantPt(float v) { return static_cast<int32_t>(std::lround(v * 16.0f)); }

}  // namespace ced
