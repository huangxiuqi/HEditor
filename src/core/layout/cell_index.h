/**
 * 块与格的对应关系，唯一来源是表格结构。
 *
 * 每块"在哪一格"不另记一份：格用块区间声明自己有哪些块，索引把这些区间反推成块到格的
 * 路径。两份记录并存时它们可以互相矛盾，而矛盾总在很远的地方才表现出来（块说自己在格
 * 里、表框却没开）；只留一份，这类矛盾在结构上就不成立。
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "core/layout/layout.h"

namespace heditor {

/**
 * 一格的块区间、身份与嵌套位置。
 */
struct CellNode {
    /**
     * 表下标（在 tables 里的位置）。
     */
    int table = -1;

    /**
     * 该表 cells 的下标。
     */
    int cell = -1;

    /**
     * 占几行（合并行）。整格选区按矩形展开时要按它判相交：合并格跨进矩形也算选到。
     */
    int row_span = 1;

    /**
     * 占几列（合并列）。
     */
    int col_span = 1;

    /**
     * 首块。
     */
    int first_block = -1;

    /**
     * 块数。
     */
    int block_count = 0;

    /**
     * 表的身份与格的行列号。
     */
    CellSlot slot;

    /**
     * 外层格在 nodes 里的下标；-1 = 最外层。
     */
    int parent = -1;

    /**
     * 外层格数，等于格路径的长度；最外层为 1。
     */
    int depth = 1;
};

/**
 * 一张表覆盖的块区间。
 */
struct TableSpan {
    /**
     * 首块；-1 = 空表。
     */
    int first_block = -1;

    /**
     * 末块（含）；-1 = 空表。
     */
    int last_block = -1;

    /**
     * 这张表在 nodes 里的起始下标。
     */
    int first_cell = 0;

    /**
     * 这张表的格数。
     */
    int cell_count = 0;
};

/**
 * 块↔格索引。
 */
struct CellIndex {
    /**
     * 每格一条，按表序、格序；空格不占一条。
     */
    std::vector<CellNode> nodes;

    /**
     * 每张表覆盖的块区间，与 tables 同长同序。
     */
    std::vector<TableSpan> spans;

    /**
     * 每块的最内层格在 nodes 里的下标；-1 = 正文。
     */
    std::vector<int> innermost;

    /**
     * 由表格结构建索引。
     *
     * 表按进入顺序排列（外层先于内层），所以按表序走一遍即可定嵌套：一张表的块区间被
     * 某张更早的表某一格整个包住，那一格就是它的父。块区间是唯一依据 —— 格的内容高
     * 含着嵌套表，包含关系天然成立，不需要额外的父子字段。
     *
     * @param tables 表格结构
     * @param block_count 块总数
     */
    void build(const std::vector<LayoutTable>& tables, size_t block_count) {
        nodes.clear();
        spans.assign(tables.size(), TableSpan{});
        innermost.assign(block_count, -1);

        std::vector<int> open;
        std::vector<int> table_lo;
        std::vector<int> table_hi;
        table_lo.reserve(tables.size());
        table_hi.reserve(tables.size());

        for (int ti = 0; ti < static_cast<int>(tables.size()); ++ti) {
            const LayoutTable& t = tables[static_cast<size_t>(ti)];
            int lo = -1;
            int hi = -1;
            for (const LayoutCell& c : t.cells) {
                if (c.block_count <= 0 || c.first_block < 0) continue;
                if (lo < 0 || c.first_block < lo) lo = c.first_block;
                if (hi < 0 || c.first_block + c.block_count > hi)
                    hi = c.first_block + c.block_count;
            }
            table_lo.push_back(lo);
            table_hi.push_back(hi);

            TableSpan& span = spans[static_cast<size_t>(ti)];
            span.first_cell = static_cast<int>(nodes.size());
            int parent = -1;
            if (lo >= 0) {
                // 出栈的是"区间已经盖不住这张表"的层级：并列的表与已结束的父表都属这一类。
                while (!open.empty()) {
                    const int o = open.back();
                    if (table_lo[static_cast<size_t>(o)] >= 0 &&
                        table_lo[static_cast<size_t>(o)] <= lo &&
                        hi <= table_hi[static_cast<size_t>(o)]) {
                        break;
                    }
                    open.pop_back();
                }
                if (!open.empty()) {
                    const TableSpan& ps = spans[static_cast<size_t>(open.back())];
                    for (int k = ps.first_cell; k < ps.first_cell + ps.cell_count; ++k) {
                        const CellNode& pn = nodes[static_cast<size_t>(k)];
                        if (pn.first_block <= lo && lo < pn.first_block + pn.block_count) {
                            parent = k;
                            break;
                        }
                    }
                }
                open.push_back(ti);
            }

            for (int ci = 0; ci < static_cast<int>(t.cells.size()); ++ci) {
                const LayoutCell& c = t.cells[static_cast<size_t>(ci)];
                if (c.block_count <= 0 || c.first_block < 0) continue;
                CellNode n;
                n.table = ti;
                n.cell = ci;
                n.row_span = c.row_span;
                n.col_span = c.col_span;
                n.first_block = c.first_block;
                n.block_count = c.block_count;
                n.slot = CellSlot{t.handle, c.row, c.cell};
                n.parent = parent;
                n.depth = (parent < 0) ? 1 : nodes[static_cast<size_t>(parent)].depth + 1;
                nodes.push_back(n);
                ++span.cell_count;
                // 内层表后建，同一个块被两层认领时后写的胜出，最内层格因此直接落在这里。
                const int last =
                    std::min(c.first_block + c.block_count, static_cast<int>(innermost.size()));
                const int k = static_cast<int>(nodes.size()) - 1;
                for (int b = c.first_block; b < last; ++b) {
                    innermost[static_cast<size_t>(b)] = k;
                }
                if (span.first_block < 0 || c.first_block < span.first_block) {
                    span.first_block = c.first_block;
                }
                if (last - 1 > span.last_block) span.last_block = last - 1;
            }
        }
    }

    /**
     * 这一块的最内层格。
     * @param block 块号
     * @returns 节点；不在表里时为空
     */
    const CellNode* nodeAt(int block) const {
        if (block < 0 || block >= static_cast<int>(innermost.size())) return nullptr;
        const int k = innermost[static_cast<size_t>(block)];
        if (k < 0) return nullptr;
        return &nodes[static_cast<size_t>(k)];
    }

    /**
     * 某一格的第一个块。格坐标不含表：调用方按行列找格时手里常常没有表句柄。
     * @param row 逻辑行号
     * @param cell 逻辑格号
     * @returns 块号；找不到为 -1
     */
    int firstBlockIn(int row, int cell) const {
        for (const CellNode& n : nodes) {
            if (n.slot.row == row && n.slot.cell == cell) return n.first_block;
        }
        return -1;
    }

    /**
     * 这一块套了几层格。
     * @param block 块号
     * @returns 层数；正文为 0
     */
    int depthAt(int block) const {
        const CellNode* n = nodeAt(block);
        return (n == nullptr) ? 0 : n->depth;
    }

    /**
     * 这一块所在的最外层表。
     * @param block 块号
     * @returns 表下标；不在表里时为 -1
     */
    int outermostTable(int block) const {
        const CellNode* n = nodeAt(block);
        if (n == nullptr) return -1;
        while (n->parent >= 0) n = &nodes[static_cast<size_t>(n->parent)];
        return n->table;
    }

    /**
     * 这一块的格路径，外层在前。
     * @param block 块号
     * @param out 结果写到这里；正文给空
     */
    void pathAt(int block, std::vector<CellSlot>* out) const {
        out->clear();
        const CellNode* n = nodeAt(block);
        while (n != nullptr) {
            out->push_back(n->slot);
            n = (n->parent < 0) ? nullptr : &nodes[static_cast<size_t>(n->parent)];
        }
        std::reverse(out->begin(), out->end());
    }
};

}  // namespace heditor
