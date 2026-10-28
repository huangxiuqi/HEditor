/**
 * 浮动图的排除带：一条占位框挖掉了多少版面。
 *
 * 单独成头是因为两个消费者都要它的完整类型：浮动流的实现（float_flow.h）与续排状态
 * （layout.h 的 ReflowState）。放进任一侧都会让两个头互相包含。
 */
#pragma once

namespace heditor {

/**
 * 一条浮动图的排除带（块轴坐标）。
 */
struct FloatBand {
    /**
     * 左沿（页内 x，磅）。
     */
    float x0 = 0.0f;

    /**
     * 右沿（页内 x，磅）。
     */
    float x1 = 0.0f;

    /**
     * 上沿（块轴 y，磅）。
     */
    float f0 = 0.0f;

    /**
     * 下沿（块轴 y，磅）。
     */
    float f1 = 0.0f;

    /**
     * 四周型：只挖掉两侧；false = 上下型：整行下推到下沿。
     */
    bool square = false;
};

}  // namespace heditor
