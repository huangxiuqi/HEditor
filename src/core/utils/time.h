/**
 * 计时。原先躺在 engine.cpp 的匿名 namespace 里，按职责拆分后排版、绘制、审计
 * 三块都要用它。提到公共位置是为了只有一份实现 —— 各写一份的话，统计口径会在
 * 不知不觉间分叉，而性能数字正是靠这些统计互相印证。
 */
#pragma once

#include <chrono>

namespace heditor {

/**
 * 单调时钟。用 steady_clock 而不是 system_clock：后者会被对时改动往回跳，
 * 一次往回跳能让耗时变成负数。
 */
using Clock = std::chrono::steady_clock;

/**
 * 从 t0 到现在的毫秒数。
 */
inline double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

}  // namespace heditor
