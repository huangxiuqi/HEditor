/**
 * 区间：一对流位置，左闭右开，带方向。
 */
#pragma once

#include "core/rev/flow_types.h"

namespace heditor {

/**
 * 区间：anchor 是不动的那一端，focus 跟着光标。
 */
struct Range {
    /**
     * 不动的那一端。
     */
    FlowPos anchor = 0;

    /**
     * 跟着光标走的那一端。
     */
    FlowPos focus = 0;

    /**
     * 靠前的那一端。
     * @returns 较小的流位置
     */
    FlowPos begin() const { return anchor < focus ? anchor : focus; }

    /**
     * 靠后的那一端。
     * @returns 较大的流位置
     */
    FlowPos end() const { return anchor < focus ? focus : anchor; }

    /**
     * 选区是否为空。
     * @returns true = 两端相等
     */
    bool empty() const { return anchor == focus; }

    /**
     * 两个区间相同。
     * @param o 另一个区间
     * @returns true = 两端与方向都相同
     */
    bool operator==(const Range& o) const { return anchor == o.anchor && focus == o.focus; }
};

}  // namespace heditor
