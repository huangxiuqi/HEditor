/**
 * 流位置与节点级位置：只描述位置，不持有树。
 */
#pragma once

#include <cstdint>

#include "core/doc/node_id.h"

namespace heditor {

/**
 * 节点级位置：容器的第 offset 个槽位，或文本节点的第 offset 个码点。
 */
struct Pos {
    /**
     * 容器节点 id。
     */
    NodeId container = kInvalidNodeId;

    /**
     * 容器里是子节点槽位；文本节点里是码点下标。
     */
    int offset = 0;

    /**
     * 两个位置相同。
     * @param o 另一个位置
     * @returns true = 相同
     */
    bool operator==(const Pos& o) const { return container == o.container && offset == o.offset; }

    /**
     * 不等。
     * @param o 另一个位置
     * @returns true = 不同
     */
    bool operator!=(const Pos& o) const { return !(*this == o); }
};

/**
 * 流位置：摊平后的线性下标。
 */
using FlowPos = int;

/**
 * 流可见性口径。
 */
enum class FlowVisibility : uint8_t {
    Design,   // 编辑（设计）态：隐藏的控件照常参与流
    Display,  // 表单与只读：隐藏的控件整棵子树摘除，不占流位置
};

}  // namespace heditor
