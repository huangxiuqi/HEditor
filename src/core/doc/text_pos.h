/**
 * 文本位置：段号 + 段内码点偏移。
 */
#pragma once

#include <cstddef>

namespace heditor {

/**
 * 位置：段 + 段内码点偏移。
 */
struct TextPos {
    /**
     * 段号（编辑态）。
     */
    int para = 0;

    /**
     * 段内码点偏移，范围 [0, 段长]。
     */
    size_t offset = 0;

    /**
     * 两个位置相同。
     * @param o 另一个位置
     * @returns true = 相同
     */
    bool operator==(const TextPos& o) const { return para == o.para && offset == o.offset; }

    /**
     * 不等。
     * @param o 另一个位置
     * @returns true = 不同
     */
    bool operator!=(const TextPos& o) const { return !(*this == o); }

    /**
     * 文档顺序比较。
     * @param o 另一个位置
     * @returns true = 本位置在前
     */
    bool operator<(const TextPos& o) const {
        return para != o.para ? para < o.para : offset < o.offset;
    }
};

}  // namespace heditor
