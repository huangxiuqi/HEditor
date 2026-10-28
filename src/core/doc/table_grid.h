/**
 * 表格派生网格：只由身份与跨度推出来，不依赖文档树。
 */
#pragma once

#include <string>
#include <vector>

#include "core/doc/node_id.h"

namespace heditor {

/**
 * 派生网格里的一个格位。
 */
struct GridCell {
    /**
     * 占这个格位的单元格节点 id。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 原点格的行号。
     */
    int row = 0;

    /**
     * 原点格的列号。
     */
    int col = 0;
};

/**
 * 派生网格，行优先。
 */
struct TableGrid {
    /**
     * 网格行数。
     */
    int rows = 0;

    /**
     * 网格列数。
     */
    int cols = 0;

    /**
     * 行优先的格位数组。
     */
    std::vector<GridCell> slots;

    /**
     * 查一个格位。
     * @param row 行号
     * @param col 列号
     * @returns 格位；越界为 nullptr
     */
    const GridCell* at(int row, int col) const;

    /**
     * 两个格位是否属于同一个单元格。
     * @param row_a 第一个格位所在行
     * @param col_a 第一个格位所在列
     * @param row_b 第二个格位所在行
     * @param col_b 第二个格位所在列
     * @returns true = 同一个单元格
     */
    bool sameOrigin(int row_a, int col_a, int row_b, int col_b) const;

    /**
     * 某个单元格在所在行里是第几格。
     * @param id 单元格节点 id
     * @returns 列号；不在网格里为 -1
     */
    int originColOf(NodeId id) const;
};

/**
 * 网格推导输入：一行里的格子身份与跨度。
 */
struct GridCellSpan {
    /**
     * 单元格节点 id。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 纵向跨度。
     */
    int row_span = 1;

    /**
     * 横向跨度。
     */
    int col_span = 1;
};

/**
 * 由跨度序列推出网格。
 * @param rows 每行的跨度序列
 * @param out 写回网格
 * @param error 写回失败原因
 * @returns true = 推得出网格
 */
bool buildGridFromSpans(const std::vector<std::vector<GridCellSpan>>& rows, TableGrid* out,
                        std::string* error);

}  // namespace heditor
