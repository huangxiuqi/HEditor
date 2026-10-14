#include "core/doc/table.h"

#include <algorithm>
#include <unordered_set>

namespace heditor {

namespace {

/**
 * 表格的直接孩子必须全是行。别的东西挂在这里，网格推导无从下手，只能显式说不支持。
 */
bool collectRows(const NodeTree& tree, NodeId table, std::vector<const Node*>* out,
                 std::string* error) {
    const Node* n = tree.find(table);
    if (n == nullptr || n->kind != NodeKind::Table) {
        *error = "不是表格";
        return false;
    }
    for (const Node* c : n->children) {
        if (c->kind != NodeKind::TableRow) {
            *error = std::string("表格的直接孩子里有 ") + nodeKindName(c->kind) + "，不是行";
            return false;
        }
        out->push_back(c);
    }
    return true;
}

/**
 * 一行的跨度之和。
 */
int rowWidth(const Node* row) {
    int total = 0;
    for (const Node* c : row->children) total += c->cell.col_span;
    return total;
}

/**
 * 某个单元格在它所在行里是第几格。
 */
int indexInRow(const Node* row, NodeId cell) {
    for (size_t i = 0; i < row->children.size(); ++i) {
        if (row->children[i]->id == cell) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

const GridCell* TableGrid::at(int row, int col) const {
    if (row < 0 || col < 0 || row >= rows || col >= cols) return nullptr;
    return &slots[static_cast<size_t>(row) * static_cast<size_t>(cols) + static_cast<size_t>(col)];
}

bool TableGrid::sameOrigin(int row_a, int col_a, int row_b, int col_b) const {
    const GridCell* a = at(row_a, col_a);
    const GridCell* b = at(row_b, col_b);
    return a != nullptr && b != nullptr && a->id == b->id;
}

int TableGrid::originColOf(NodeId id) const {
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const GridCell* g = at(r, c);
            if (g->id == id && g->col == c && g->row == r) return c;
        }
    }
    return -1;
}

bool buildGrid(const NodeTree& tree, NodeId table, TableGrid* out, std::string* error) {
    std::vector<const Node*> rows;
    if (!collectRows(tree, table, &rows, error)) return false;

    out->rows = static_cast<int>(rows.size());
    out->cols = rows.empty() ? 0 : rowWidth(rows.front());

    const size_t total = static_cast<size_t>(out->rows) * static_cast<size_t>(out->cols);
    out->slots.assign(total, GridCell{});

    auto occupied = [&](int r, int c) -> bool {
        const GridCell& slot = out->slots[static_cast<size_t>(r) * static_cast<size_t>(out->cols) +
                                          static_cast<size_t>(c)];
        return slot.id != kInvalidNodeId;
    };

    for (int r = 0; r < out->rows; ++r) {
        int cursor = 0;
        for (const Node* cell : rows[static_cast<size_t>(r)]->children) {
            // 让过上面跨下来的格子覆盖掉的格位。
            while (cursor < out->cols && occupied(r, cursor)) ++cursor;

            const int rs = cell->cell.row_span;
            const int cs = cell->cell.col_span;
            if (rs < 1 || cs < 1) {
                *error = "跨度的两个方向都必须至少是 1";
                return false;
            }
            if (cursor + cs > out->cols) {
                *error = "第 " + std::to_string(r) + " 行放不下第 " + std::to_string(cursor) +
                         " 列起的 " + std::to_string(cs) + " 格跨度";
                return false;
            }
            if (r + rs > out->rows) {
                *error = "第 " + std::to_string(r) + " 行的纵向合并跨过了表格末尾";
                return false;
            }
            for (int dr = 0; dr < rs; ++dr) {
                for (int dc = 0; dc < cs; ++dc) {
                    const int rr = r + dr;
                    const int cc = cursor + dc;
                    if (occupied(rr, cc)) {
                        *error = "格位 (" + std::to_string(rr) + ", " + std::to_string(cc) +
                                 ") 被两个单元格覆盖";
                        return false;
                    }
                    GridCell& slot =
                        out->slots[static_cast<size_t>(rr) * static_cast<size_t>(out->cols) +
                                   static_cast<size_t>(cc)];
                    slot.id = cell->id;
                    slot.row = r;
                    slot.col = cursor;
                }
            }
            cursor += cs;
        }

        // 这一行必须被完整覆盖，且不留缝隙。这就是"每行的跨度之和相等、等于网格列数"那一条
        // 的落点 —— 注意它说的是这一行被覆盖的格位数（含从上面跨下来的合并），不是这一行
        // 自己的格子的跨度之和：纵向合并覆盖的行本来就没有那么多自己的格子，按后者判会把
        // 合法的表判成非法。
        for (int c = 0; c < out->cols; ++c) {
            if (!occupied(r, c)) {
                *error = "第 " + std::to_string(r) + " 行的格位 " + std::to_string(c) +
                         " 没有格子（这一行没有被完整覆盖）";
                return false;
            }
        }
    }
    return true;
}

StepBatch makeInsertTableRow(NodeTree& tree, NodeId table, int at_row,
                             const std::vector<NodeSpec>& cells) {
    StepBatch out;
    TableGrid grid;
    if (!buildGrid(tree, table, &grid, &out.error)) return out;
    if (at_row < 0 || at_row > grid.rows) {
        out.error = "插入行：行号越界";
        return out;
    }

    int width = 0;
    for (const NodeSpec& c : cells) {
        if (c.kind != NodeKind::TableCell) {
            out.error = std::string("插入行：行里只能放单元格，收到 ") + nodeKindName(c.kind);
            return out;
        }
        if (c.cell.row_span != 1) {
            out.error = "插入行：新行里的格子不能跨行（会与下面的行重叠），插入之后再合并";
            return out;
        }
        if (c.cell.col_span < 1) {
            out.error = "插入行：跨度至少是 1";
            return out;
        }
        width += c.cell.col_span;
    }
    if (width != grid.cols) {
        out.error = "插入行：格子的跨度之和是 " + std::to_string(width) + "，与列数 " +
                    std::to_string(grid.cols) + " 不等";
        return out;
    }

    NodeSpec row;
    row.kind = NodeKind::TableRow;
    row.children = cells;
    const NodeBuild b = makeInsertNode(tree, Pos{table, at_row}, row);
    if (!b.ok) {
        out.error = b.error;
        return out;
    }
    pushMappedStep(&out.steps, b.step);
    out.ok = true;
    return out;
}

StepBatch makeRemoveTableRow(NodeTree& tree, NodeId table, int at_row) {
    StepBatch out;
    TableGrid grid;
    if (!buildGrid(tree, table, &grid, &out.error)) return out;
    if (at_row < 0 || at_row >= grid.rows) {
        out.error = "删除行：行号越界";
        return out;
    }
    if (grid.rows == 1) {
        out.error = "删除行：删掉最后一行会让表格没有行，表格结构操作里不支持";
        return out;
    }

    const Node* table_node = tree.find(table);
    const Node* row = table_node->children[static_cast<size_t>(at_row)];

    // 自己带纵向合并的格子：它们的内容随行消失，但覆盖的下面几行会凭空少一格。
    for (const Node* cell : row->children) {
        if (cell->cell.row_span > 1) {
            out.error = "删除行：这一行有纵向合并的格子，先拆分合并再删行";
            return out;
        }
    }

    // 从上面跨进来的纵向合并先缩短一格，否则它的覆盖会落到已经不存在的行上。
    for (int r = 0; r < at_row; ++r) {
        for (const Node* cell : table_node->children[static_cast<size_t>(r)]->children) {
            if (r + cell->cell.row_span <= at_row) continue;
            SetTableCellSpan s;
            s.id = cell->id;
            s.span.row_span = cell->cell.row_span - 1;
            s.span.col_span = cell->cell.col_span;
            s.before = cell->cell;
            pushMappedStep(&out.steps, Step{s});
        }
    }

    const NodeBuild b = makeRemoveNode(tree, Pos{table, at_row});
    if (!b.ok) {
        out.error = b.error;
        return out;
    }
    pushMappedStep(&out.steps, b.step);
    out.ok = true;
    return out;
}

StepBatch makeInsertTableColumn(NodeTree& tree, NodeId table, int at_col) {
    StepBatch out;
    TableGrid grid;
    if (!buildGrid(tree, table, &grid, &out.error)) return out;
    if (at_col < 0 || at_col > grid.cols) {
        out.error = "插入列：列号越界";
        return out;
    }

    const Node* table_node = tree.find(table);
    const bool inside = at_col > 0 && at_col < grid.cols;

    // 一、跨过插入线的格子加宽一格。跨行合并的格子会在它覆盖的每一行里都吸收这一列，
    // 所以按原点格去重，不能按行数重复加宽。
    std::unordered_set<NodeId> widened;
    std::vector<NodeId> widened_order;
    if (inside) {
        for (int r = 0; r < grid.rows; ++r) {
            if (!grid.sameOrigin(r, at_col - 1, r, at_col)) continue;
            const GridCell* g = grid.at(r, at_col - 1);
            if (widened.insert(g->id).second) widened_order.push_back(g->id);
        }
    }

    // 二、没有格子跨过插入线的行各插一个新格子，否则这一行的总宽度对不上。
    for (int r = 0; r < grid.rows; ++r) {
        const bool absorbed = inside && grid.sameOrigin(r, at_col - 1, r, at_col);
        if (absorbed) continue;

        const Node* row = table_node->children[static_cast<size_t>(r)];
        int index = static_cast<int>(row->children.size());
        for (size_t i = 0; i < row->children.size(); ++i) {
            if (grid.originColOf(row->children[i]->id) >= at_col) {
                index = static_cast<int>(i);
                break;
            }
        }
        NodeSpec cell;
        cell.kind = NodeKind::TableCell;
        const NodeBuild b = makeInsertNode(tree, Pos{row->id, index}, cell);
        if (!b.ok) {
            out.error = b.error;
            return out;
        }
        pushMappedStep(&out.steps, b.step);
    }

    // 三、最后改跨度：它不动位置，放在后面免得多算一次映射。
    for (NodeId id : widened_order) {
        const Node* cell = tree.find(id);
        SetTableCellSpan s;
        s.id = id;
        s.span.row_span = cell->cell.row_span;
        s.span.col_span = cell->cell.col_span + 1;
        s.before = cell->cell;
        pushMappedStep(&out.steps, Step{s});
    }

    out.ok = true;
    return out;
}

StepBatch makeRemoveTableColumn(NodeTree& tree, NodeId table, int at_col) {
    StepBatch out;
    TableGrid grid;
    if (!buildGrid(tree, table, &grid, &out.error)) return out;
    if (at_col < 0 || at_col >= grid.cols) {
        out.error = "删除列：列号越界";
        return out;
    }
    if (grid.cols == 1) {
        out.error = "删除列：删掉最后一列会让表格没有列，表格结构操作里不支持";
        return out;
    }

    const Node* table_node = tree.find(table);

    // 覆盖这一列的格子：宽度减一；宽度降到 0 的连同它的内容一起删掉。每个原点格只出现在
    // 一行里（它的原点行），所以"一格一次"，不必担心同一行里多处删除导致下标错位。
    std::vector<NodeId> shrink;
    std::vector<NodeId> drop;
    std::unordered_set<NodeId> seen;
    for (int r = 0; r < grid.rows; ++r) {
        const GridCell* g = grid.at(r, at_col);
        if (g == nullptr || !seen.insert(g->id).second) continue;
        if (g->col < 0) {
            out.error = "删除列：格子没有原点，网格与文档对不上";
            return out;
        }
        const Node* cell = tree.find(g->id);
        if (cell->cell.col_span > 1) {
            shrink.push_back(g->id);
        } else {
            drop.push_back(g->id);
        }
    }

    for (NodeId id : shrink) {
        const Node* cell = tree.find(id);
        SetTableCellSpan s;
        s.id = id;
        s.span.row_span = cell->cell.row_span;
        s.span.col_span = cell->cell.col_span - 1;
        s.before = cell->cell;
        pushMappedStep(&out.steps, Step{s});
    }
    for (NodeId id : drop) {
        const GridCell* g = nullptr;
        for (int r = 0; r < grid.rows && g == nullptr; ++r) {
            const GridCell* cand = grid.at(r, at_col);
            if (cand != nullptr && cand->id == id) g = cand;
        }
        if (g == nullptr) continue;
        const Node* row = table_node->children[static_cast<size_t>(g->row)];
        const NodeBuild b = makeRemoveNode(tree, Pos{row->id, indexInRow(row, id)});
        if (!b.ok) {
            out.error = b.error;
            return out;
        }
        pushMappedStep(&out.steps, b.step);
    }

    out.ok = true;
    return out;
}

}  // namespace heditor
