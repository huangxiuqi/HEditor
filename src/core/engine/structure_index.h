/**
 * 结构索引：控件区间与段到格的同一份反查。
 *
 * 这两张表都由投影产物派生，生命周期与重建时机一致。放在一个对象里，重建时只会
 * 有一个"结构版本"，不会出现控件索引已换、格索引还指旧表的错配。
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "core/doc/project.h"
#include "core/engine/control_spans.h"
#include "core/layout/layout.h"

namespace heditor {

/**
 * 一段落在哪张表的哪一格。
 */
struct CellRef {
    /**
     * 表下标；-1 = 不在表里。
     */
    int table = -1;

    /**
     * 该表 cells 的下标；-1 = 不在表里。
     */
    int cell = -1;
};

/**
 * 一格的块区间。
 */
struct CellExtent {
    /**
     * 表下标。
     */
    int table = -1;

    /**
     * 该表 cells 的下标。
     */
    int cell = -1;

    /**
     * 首段。
     */
    int first_block = -1;

    /**
     * 段数。
     */
    int block_count = 0;
};

/**
 * 一张表覆盖的段区间。
 */
struct TableSpan {
    /**
     * 首段；-1 = 空表。
     */
    int first_block = -1;

    /**
     * 末段（含）；-1 = 空表。
     */
    int last_block = -1;

    /**
     * 这张表在 cell_extents 里的起始下标。
     */
    int first_cell = 0;

    /**
     * 这张表在 cell_extents 里的格数。
     */
    int cell_count = 0;
};

/**
 * 结构索引本体。
 */
struct StructureIndex {
    /**
     * 控件区间索引。
     */
    ControlIndex controls;

    /**
     * 投影产出的控件跨度（块坐标）。
     */
    std::vector<doc::ControlSpan> control_spans;

    /**
     * 段号 → 最内层格。
     */
    std::vector<CellRef> cells;

    /**
     * 表下标 → 覆盖段区间。
     */
    std::vector<TableSpan> table_spans;

    /**
     * 每一格的块区间，按表序、格序。
     */
    std::vector<CellExtent> cell_extents;

    /**
     * 每段的行内槽（投影产物），按 cp 升序。
     */
    std::vector<std::vector<doc::Slot>> slots;

    /**
     * 重建控件区间。
     * @param flow 流索引
     * @param ids 控件节点表
     */
    void buildControls(const doc::FlowIndex& flow, const std::vector<NodeId>& ids) {
        controls.build(flow, ids);
    }

    /**
     * 重建段到格。外层表先填、内层表后填，覆盖顺序就是最内层规则。
     * @param tables 表格结构
     * @param block_count 段总数
     */
    void buildCells(const std::vector<LayoutTable>& tables, size_t block_count) {
        cells.assign(block_count, CellRef{});
        table_spans.assign(tables.size(), TableSpan{});
        cell_extents.clear();
        for (int ti = 0; ti < static_cast<int>(tables.size()); ++ti) {
            const LayoutTable& t = tables[static_cast<size_t>(ti)];
            TableSpan& span = table_spans[static_cast<size_t>(ti)];
            span.first_cell = static_cast<int>(cell_extents.size());
            for (int ci = 0; ci < static_cast<int>(t.cells.size()); ++ci) {
                const LayoutCell& c = t.cells[static_cast<size_t>(ci)];
                if (c.block_count <= 0 || c.first_block < 0) continue;
                cell_extents.push_back(CellExtent{ti, ci, c.first_block, c.block_count});
                ++span.cell_count;
                const int last =
                    std::min(c.first_block + c.block_count, static_cast<int>(cells.size()));
                for (int b = c.first_block; b < last; ++b) {
                    cells[static_cast<size_t>(b)] = CellRef{ti, ci};
                }
                if (span.first_block < 0 || c.first_block < span.first_block)
                    span.first_block = c.first_block;
                const int cell_last = last - 1;
                if (cell_last > span.last_block) span.last_block = cell_last;
            }
        }
    }

    /**
     * 重建每段的行内槽。
     * @param source 投影产出的槽
     * @param block_count 段总数
     */
    void buildSlots(const std::vector<doc::Slot>& source, size_t block_count) {
        slots.assign(block_count, {});
        for (const doc::Slot& o : source) {
            if (o.block < 0 || o.block >= static_cast<int>(slots.size())) continue;
            slots[static_cast<size_t>(o.block)].push_back(o);
        }
        for (std::vector<doc::Slot>& list : slots) {
            std::sort(list.begin(), list.end(),
                      [](const doc::Slot& a, const doc::Slot& z) { return a.cp < z.cp; });
        }
    }

    /**
     * 某个段内码点之前的括号统计。
     * @param para 段号
     * @param cp 段内码点
     * @param before 写回该偏移之前的所有槽数
     * @param opens 写回该偏移上的开括号数
     * @param closes 写回该偏移上的闭括号数
     * @returns 有这一段吗
     */
    bool bracketCounts(int para, size_t cp, int* before, int* opens, int* closes) const {
        if (para < 0 || para >= static_cast<int>(slots.size())) return false;
        const std::vector<doc::Slot>& list = slots[static_cast<size_t>(para)];
        const auto it = std::lower_bound(list.begin(), list.end(), static_cast<uint32_t>(cp),
                                         [](const doc::Slot& s, uint32_t v) { return s.cp < v; });
        if (before != nullptr) *before = static_cast<int>(it - list.begin());
        int o = 0;
        int c = 0;
        for (auto k = it; k != list.end() && k->cp == static_cast<uint32_t>(cp); ++k) {
            if (k->kind == InlineObjectKind::ControlStart) {
                ++o;
            } else {
                ++c;
            }
        }
        if (opens != nullptr) *opens = o;
        if (closes != nullptr) *closes = c;
        return true;
    }

    /**
     * 这一段的格。
     * @param para 段号
     * @returns 格引用；不在表里时 table/cell 为 -1
     */
    const CellRef& cellAt(int para) const {
        static const CellRef kNone{};
        if (para < 0 || para >= static_cast<int>(cells.size())) return kNone;
        return cells[static_cast<size_t>(para)];
    }

    /**
     * 一张表的段区间。
     * @param table 表下标
     * @returns 段区间；越界时 first/last 为 -1
     */
    const TableSpan& tableSpan(int table) const {
        static const TableSpan kNone{};
        if (table < 0 || table >= static_cast<int>(table_spans.size())) return kNone;
        return table_spans[static_cast<size_t>(table)];
    }
};

}  // namespace heditor
