/**
 * 表格结构操作的实现（修订层）。
 *
 * 三处口径与模型层逐条对齐（core/doc/table.cpp），差别只有数据源：
 * 跨度是唯一真相、网格是派生的；插一列 = 跨线格子加宽 + 其余行插新格；删一列 = 变窄 + 窄到
 * 零的删掉。对齐是必须的：同一份文档在两条路径上做同一件事，结果不同就是两套语义。
 */
#include "core/doc/table_ops.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace heditor::doc {

namespace {

/**
 * 表格的直接孩子里那些行。非 TableRow 的杂孩子跳过 —— 那种形状由不变量报，不在这里纠正。
 * @param table 表格节点
 * @return 行节点
 */
std::vector<const Node*> rowsOf(const Node& table) {
    std::vector<const Node*> rows;
    for (const Ref& c : table.children) {
        if (c->kind == NodeKind::TableRow) rows.push_back(c.get());
    }
    return rows;
}

/**
 * 一行的直接孩子里那些单元格。
 * @param row 行节点
 * @return 单元格节点
 */
std::vector<const Node*> cellsOf(const Node& row) {
    std::vector<const Node*> cells;
    for (const Ref& c : row.children) {
        if (c->kind == NodeKind::TableCell) cells.push_back(c.get());
    }
    return cells;
}

/**
 * 一格的记录。
 * @param cell 单元格节点
 * @return 记录；载荷不对时是默认值
 */
CellSpec cellSpecOf(const Node& cell) {
    const auto* s = std::get_if<CellSpec>(&cell.payload);
    return s == nullptr ? CellSpec{} : *s;
}

/**
 * 一枚空段落标记的描述。
 * @return 节点描述
 */
NodeSpec markSpec() {
    NodeSpec mark;
    mark.kind = NodeKind::ParagraphMark;
    return mark;
}

/**
 * 一个新单元格的描述：一枚空段落标记。没有标记的格在流里一位都不占，编译出来是零块格，
 * 命中测试落不进去。
 * @param col_span 横向跨度
 * @return 节点描述
 */
NodeSpec newCellSpec(int col_span) {
    NodeSpec cell;
    cell.kind = NodeKind::TableCell;
    cell.cell.col_span = col_span;
    cell.children.push_back(markSpec());
    return cell;
}

/**
 * 把流位置按已收集的步骤映射过去。一批步骤里每步的坐标属于它自己那一刻：前面插了一格，
 * 后面那些绝对流位置就过期了。
 * @param prev 已经收集的步骤
 * @param pos 起点
 * @return 映射之后的位置
 */
FlowPos through(const std::vector<Step>& prev, FlowPos pos) {
    for (const Step& s : prev) pos = mapPos(s, pos, Assoc::Before);
    return pos;
}

/**
 * 收一条步骤，先把它的流坐标按前面的步骤推过去。
 * @param out 收集处
 * @param step 要收的步骤
 * @return true = 收下了
 */
bool pushMapped(std::vector<Step>* out, Step step) {
    if (auto* ins = std::get_if<InsertNode>(&step)) {
        ins->at_flow = through(*out, ins->at_flow);
    } else if (auto* rm = std::get_if<RemoveNode>(&step)) {
        rm->at_flow = through(*out, rm->at_flow);
    } else {
        return false;
    }
    out->push_back(std::move(step));
    return true;
}

/**
 * 造一条"插入节点"步骤的节点级地址。
 * @param flow 流索引
 * @param at 节点级地址
 * @param spec 子树
 * @param out 写回的步骤
 * @return true = 这个地址在流里
 */
bool makeInsert(const FlowIndex& flow, const Pos& at, const NodeSpec& spec, Step* out) {
    const FlowPos f = flow.flowAt(at);
    if (f < 0) return false;
    InsertNode ins;
    ins.at = at;
    ins.at_flow = f;
    ins.subtree = spec;
    *out = Step{ins};
    return true;
}

/**
 * 造一条"删除节点"步骤的节点级地址。
 * @param flow 流索引
 * @param at 节点级地址
 * @param removed 被删子树的描述（撤销靠摘下的那棵，这里只用于校验与审计）
 * @param out 写回的步骤
 * @return true = 这个地址在流里
 */
bool makeRemove(const FlowIndex& flow, const Pos& at, const NodeSpec& removed, Step* out) {
    const FlowPos f = flow.flowAt(at);
    if (f < 0) return false;
    RemoveNode rm;
    rm.at = at;
    rm.at_flow = f;
    rm.removed = removed;
    *out = Step{rm};
    return true;
}

/**
 * 这一格有内容吗。空的判据是"只有一枚段落标记"：段落格式的覆盖不随合并丢失 —— 它挂在
 * 那枚标记上，而有标记就不算空。
 * @param cell 单元格节点
 * @return true = 只有一枚段落标记、没有别的孩子
 */
bool cellIsEmpty(const Node& cell) {
    if (cell.children.size() != 1) return false;
    const Ref& only = cell.children.front();
    return only->kind == NodeKind::ParagraphMark && only->children.empty();
}

/**
 * 一格的矩形覆盖：原点格坐标 + 跨度。
 */
struct CellRect {
    /**
     * 原点行号。
     */
    int row = 0;

    /**
     * 原点列号。
     */
    int col = 0;

    /**
     * 纵跨几行。
     */
    int rows = 1;

    /**
     * 横跨几列。
     */
    int cols = 1;
};

/**
 * 按网格求一格的矩形覆盖。网格里第一个落在它身上的格位就是原点格。
 * @param grid 网格
 * @param id 单元格节点号
 * @param out 写回矩形
 * @return true = 这一格在网格里
 */
bool rectOf(const TableGrid& grid, NodeId id, CellRect* out) {
    for (int r = 0; r < grid.rows; ++r) {
        for (int c = 0; c < grid.cols; ++c) {
            const GridCell* g = grid.at(r, c);
            if (g == nullptr || g->id != id || g->row != r || g->col != c) continue;
            // 原点是它自己：往下、往右数到不是它的地方就是跨度。
            int rows = 1;
            while (rows < grid.rows - r) {
                const GridCell* below = grid.at(r + rows, c);
                if (below == nullptr || below->id != id) break;
                ++rows;
            }
            int cols = 1;
            while (cols < grid.cols - c) {
                const GridCell* right = grid.at(r, c + cols);
                if (right == nullptr || right->id != id) break;
                ++cols;
            }
            out->row = r;
            out->col = c;
            out->rows = rows;
            out->cols = cols;
            return true;
        }
    }
    return false;
}

}  // namespace

bool tableGrid(const Doc& doc, NodeId table, TableGrid* out, std::string* error) {
    const Node* t = findNode(doc, table);
    if (t == nullptr || t->kind != NodeKind::Table) {
        if (error != nullptr) *error = "目标不是表格";
        return false;
    }
    std::vector<std::vector<GridCellSpan>> spans;
    for (const Node* row : rowsOf(*t)) {
        std::vector<GridCellSpan> line;
        for (const Node* cell : cellsOf(*row)) {
            const CellSpec cs = cellSpecOf(*cell);
            line.push_back(GridCellSpan{cell->id, cs.row_span, cs.col_span});
        }
        spans.push_back(std::move(line));
    }
    return buildGridFromSpans(spans, out, error);
}

bool cellRefOf(const Doc& doc, NodeId table, int row, int cell, CellRef* out) {
    const Node* t = findNode(doc, table);
    if (t == nullptr || t->kind != NodeKind::Table || out == nullptr) return false;
    const std::vector<const Node*> rows = rowsOf(*t);
    if (row < 0 || row >= static_cast<int>(rows.size())) return false;
    const std::vector<const Node*> cells = cellsOf(*rows[static_cast<size_t>(row)]);
    if (cell < 0 || cell >= static_cast<int>(cells.size())) return false;
    out->table = table;
    out->row = cells.empty() ? kInvalidNodeId : rows[static_cast<size_t>(row)]->id;
    out->cell = cells[static_cast<size_t>(cell)]->id;
    out->row_index = row;
    out->cell_index = cell;
    return true;
}

bool cellRefAt(const Doc& doc, uint64_t table_handle, int row, int cell, CellRef* out) {
    if (table_handle == 0 || table_handle == static_cast<uint64_t>(kInvalidNodeId)) return false;
    return cellRefOf(doc, static_cast<NodeId>(table_handle), row, cell, out);
}

bool cellOrigin(const Doc& doc, NodeId table, NodeId cell, int* row, int* col) {
    TableGrid grid;
    std::string why;
    if (!tableGrid(doc, table, &grid, &why)) return false;
    CellRect rect;
    if (!rectOf(grid, cell, &rect)) return false;
    if (row != nullptr) *row = rect.row;
    if (col != nullptr) *col = rect.col;
    return true;
}

TableSteps insertRow(const Doc& doc, const FlowIndex& flow, const CellRef& at, bool below) {
    TableSteps out;
    TableGrid grid;
    if (!tableGrid(doc, at.table, &grid, &out.error)) return out;
    const int at_row = at.row_index + (below ? 1 : 0);
    if (at_row < 0 || at_row > grid.rows) {
        out.error = "插入行：行号越界";
        return out;
    }

    const Node* t = findNode(doc, at.table);
    const std::vector<const Node*> rows = rowsOf(*t);
    const Node* ref = rows[static_cast<size_t>(at.row_index)];

    // 新行照着参照行的横向跨度序列造：参照行里那格跨三列，新行也是那一格跨三列。
    const std::vector<const Node*> ref_cells = cellsOf(*ref);
    if (ref_cells.empty()) {
        out.error = "插入行：参照行里没有单元格";
        return out;
    }
    NodeSpec row;
    row.kind = NodeKind::TableRow;
    for (const Node* cell : ref_cells) {
        row.children.push_back(newCellSpec(cellSpecOf(*cell).col_span));
    }

    Step step;
    if (!makeInsert(flow, Pos{at.table, at_row}, row, &step)) {
        out.error = "插入行：插入点不在流里";
        return out;
    }
    pushMapped(&out.steps, std::move(step));
    out.ok = true;
    return out;
}

TableSteps removeRow(const Doc& doc, const FlowIndex& flow, const CellRef& at) {
    TableSteps out;
    TableGrid grid;
    if (!tableGrid(doc, at.table, &grid, &out.error)) return out;
    if (at.row_index < 0 || at.row_index >= grid.rows) {
        out.error = "删除行：行号越界";
        return out;
    }
    if (grid.rows == 1) {
        out.error = "删除行：删掉最后一行会让表格没有行";
        return out;
    }

    const Node* t = findNode(doc, at.table);
    const std::vector<const Node*> rows = rowsOf(*t);

    // 自己带纵向合并的格子：它们的内容随行消失，而覆盖的下面几行会凭空少一格。
    for (const Node* cell : cellsOf(*rows[static_cast<size_t>(at.row_index)])) {
        if (cellSpecOf(*cell).row_span > 1) {
            out.error = "删除行：这一行有纵向合并的格子，先拆分合并再删行";
            return out;
        }
    }

    // 从上面跨进来的纵向合并先缩短一格，否则它的覆盖会落到已经不存在的行上。
    for (int r = 0; r < at.row_index; ++r) {
        for (const Node* cell : cellsOf(*rows[static_cast<size_t>(r)])) {
            const CellSpec cs = cellSpecOf(*cell);
            if (r + cs.row_span <= at.row_index) continue;
            SetCellSpec s;
            s.id = cell->id;
            s.spec = cs;
            s.spec.row_span = cs.row_span - 1;
            s.before = cs;
            out.steps.push_back(Step{s});
        }
    }

    Step step;
    const NodeSpec removed = toSpec(*rows[static_cast<size_t>(at.row_index)]);
    if (!makeRemove(flow, Pos{at.table, at.row_index}, removed, &step)) {
        out.error = "删除行：删除点不在流里";
        return out;
    }
    pushMapped(&out.steps, std::move(step));
    out.ok = true;
    return out;
}

TableSteps insertColumn(const Doc& doc, const FlowIndex& flow, const CellRef& at, bool right) {
    TableSteps out;
    TableGrid grid;
    if (!tableGrid(doc, at.table, &grid, &out.error)) return out;
    if (at.cell_index < 0) {
        out.error = "插入列：格号越界";
        return out;
    }
    const Node* probe = findNode(doc, at.cell);
    if (probe == nullptr) {
        out.error = "插入列：目标格不存在";
        return out;
    }
    CellRect ref_rect;
    if (!rectOf(grid, at.cell, &ref_rect)) {
        out.error = "插入列：目标格不在网格里";
        return out;
    }
    // 右边那一列的插入点 = 这一格覆盖区域的右边界；左边那一列 = 它的左边界。
    const int at_col = right ? ref_rect.col + ref_rect.cols : ref_rect.col;
    if (at_col < 0 || at_col > grid.cols) {
        out.error = "插入列：列号越界";
        return out;
    }

    const Node* t = findNode(doc, at.table);
    const std::vector<const Node*> rows = rowsOf(*t);
    const bool inside = at_col > 0 && at_col < grid.cols;

    // 一、跨过插入线的格子加宽一格。跨行合并的格子会在它覆盖的每一行里都吸收这一列，
    // 所以按原点格去重，不能按行数重复加宽。
    std::unordered_set<NodeId> widened;
    std::vector<NodeId> widened_order;
    if (inside) {
        for (int r = 0; r < grid.rows; ++r) {
            if (!grid.sameOrigin(r, at_col - 1, r, at_col)) continue;
            const GridCell* g = grid.at(r, at_col - 1);
            if (g != nullptr && widened.insert(g->id).second) widened_order.push_back(g->id);
        }
    }

    // 二、没有格子跨过插入线的行各插一个新格子，否则这一行的总宽度对不上。
    for (int r = 0; r < grid.rows; ++r) {
        const bool absorbed = inside && grid.sameOrigin(r, at_col - 1, r, at_col);
        if (absorbed) continue;
        const Node* row = rows[static_cast<size_t>(r)];
        const std::vector<const Node*> cells = cellsOf(*row);
        int index = static_cast<int>(row->children.size());
        for (size_t i = 0; i < cells.size(); ++i) {
            if (grid.originColOf(cells[i]->id) >= at_col) {
                index = static_cast<int>(i);
                break;
            }
        }
        Step step;
        if (!makeInsert(flow, Pos{row->id, index}, newCellSpec(1), &step)) {
            out.error = "插入列：插入点不在流里";
            return out;
        }
        pushMapped(&out.steps, std::move(step));
    }

    // 三、最后改记录：它不动位置，放在后面免得多算一次映射。
    for (NodeId id : widened_order) {
        const Node* cell = findNode(doc, id);
        if (cell == nullptr) continue;
        const CellSpec cs = cellSpecOf(*cell);
        SetCellSpec s;
        s.id = id;
        s.spec = cs;
        s.spec.col_span = cs.col_span + 1;
        s.before = cs;
        out.steps.push_back(Step{s});
    }

    out.ok = true;
    return out;
}

TableSteps removeColumn(const Doc& doc, const FlowIndex& flow, const CellRef& at) {
    TableSteps out;
    TableGrid grid;
    if (!tableGrid(doc, at.table, &grid, &out.error)) return out;
    if (grid.cols == 1) {
        out.error = "删除列：删掉最后一列会让表格没有列";
        return out;
    }

    CellRect ref_rect;
    if (!rectOf(grid, at.cell, &ref_rect)) {
        out.error = "删除列：目标格不在网格里";
        return out;
    }
    const int at_col = ref_rect.col;
    if (at_col < 0 || at_col >= grid.cols) {
        out.error = "删除列：列号越界";
        return out;
    }

    const Node* t = findNode(doc, at.table);
    const std::vector<const Node*> rows = rowsOf(*t);

    // 覆盖这一列的格子：宽度减一；宽度降到 0 的连同它的内容一起删掉。每个原点格只出现在
    // 它的原点行里，所以"一格一次"，不必担心同一行里多处删除导致下标错位。
    std::vector<NodeId> shrink;
    std::vector<NodeId> drop;
    std::unordered_set<NodeId> seen;
    for (int r = 0; r < grid.rows; ++r) {
        const GridCell* g = grid.at(r, at_col);
        if (g == nullptr || !seen.insert(g->id).second) continue;
        const Node* cell = findNode(doc, g->id);
        if (cell == nullptr) continue;
        if (cellSpecOf(*cell).col_span > 1) {
            shrink.push_back(g->id);
        } else {
            drop.push_back(g->id);
        }
    }

    for (NodeId id : shrink) {
        const Node* cell = findNode(doc, id);
        if (cell == nullptr) continue;
        const CellSpec cs = cellSpecOf(*cell);
        SetCellSpec s;
        s.id = id;
        s.spec = cs;
        s.spec.col_span = cs.col_span - 1;
        s.before = cs;
        out.steps.push_back(Step{s});
    }
    for (NodeId id : drop) {
        const Node* cell = findNode(doc, id);
        const Node* row = parentOf(doc, id);
        if (cell == nullptr || row == nullptr) continue;
        const std::vector<const Node*> cells = cellsOf(*row);
        int index = -1;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (cells[i]->id == id) {
                index = static_cast<int>(i);
                break;
            }
        }
        if (index < 0) continue;
        Step step;
        const NodeSpec removed = toSpec(*cell);
        if (!makeRemove(flow, Pos{row->id, index}, removed, &step)) {
            out.error = "删除列：删除点不在流里";
            return out;
        }
        pushMapped(&out.steps, std::move(step));
    }

    out.ok = true;
    return out;
}

TableSteps mergeCells(const Doc& doc, const FlowIndex& flow, const std::vector<CellRef>& cells) {
    TableSteps out;
    if (cells.size() < 2) {
        out.error = "合并：至少选中两格";
        return out;
    }
    const NodeId table = cells.front().table;
    for (const CellRef& c : cells) {
        if (c.table != table) {
            out.error = "合并：只支持同一张表里的格子";
            return out;
        }
    }
    TableGrid grid;
    if (!tableGrid(doc, table, &grid, &out.error)) return out;

    std::vector<CellRect> rects;
    rects.reserve(cells.size());
    for (const CellRef& c : cells) {
        CellRect r;
        if (!rectOf(grid, c.cell, &r)) {
            out.error = "合并：有一格不在网格里";
            return out;
        }
        rects.push_back(r);
    }

    // 包络：全部格子的最小外接矩形。之后要求它们正好铺满它 —— 缺角或重叠都拒绝，
    // 因为"合并出来的那一格"只能是一个矩形。
    int lo_row = rects.front().row;
    int hi_row = rects.front().row + rects.front().rows;
    int lo_col = rects.front().col;
    int hi_col = rects.front().col + rects.front().cols;
    for (const CellRect& r : rects) {
        lo_row = std::min(lo_row, r.row);
        hi_row = std::max(hi_row, r.row + r.rows);
        lo_col = std::min(lo_col, r.col);
        hi_col = std::max(hi_col, r.col + r.cols);
    }
    std::vector<std::vector<int>> cover(static_cast<size_t>(hi_row - lo_row),
                                        std::vector<int>(static_cast<size_t>(hi_col - lo_col), 0));
    for (const CellRect& r : rects) {
        for (int rr = r.row; rr < r.row + r.rows; ++rr) {
            for (int cc = r.col; cc < r.col + r.cols; ++cc) {
                ++cover[static_cast<size_t>(rr - lo_row)][static_cast<size_t>(cc - lo_col)];
            }
        }
    }
    for (const std::vector<int>& line : cover) {
        for (int n : line) {
            if (n != 1) {
                out.error = "合并：选中的格子没有正好铺满一个矩形";
                return out;
            }
        }
    }

    // 锚点是左上角那一格。它的节点身份就是"合并之后留下来的那一格"。
    NodeId anchor = kInvalidNodeId;
    for (size_t i = 0; i < rects.size(); ++i) {
        if (rects[i].row == lo_row && rects[i].col == lo_col) anchor = cells[i].cell;
    }
    if (anchor == kInvalidNodeId) {
        out.error = "合并：找不到左上角那一格";
        return out;
    }
    const Node* anchor_node = findNode(doc, anchor);
    if (anchor_node == nullptr) {
        out.error = "合并：左上角那一格不存在";
        return out;
    }

    // 除锚点以外必须是空的：合并会删掉那些格子，有内容就是丢数据。
    for (const CellRef& c : cells) {
        if (c.cell == anchor) continue;
        const Node* n = findNode(doc, c.cell);
        if (n == nullptr) {
            out.error = "合并：有一格不存在";
            return out;
        }
        if (!cellIsEmpty(*n)) {
            out.error = "合并：要并进去的格子里还有内容，先清空再合并";
            return out;
        }
    }

    const CellSpec before = cellSpecOf(*anchor_node);
    SetCellSpec s;
    s.id = anchor;
    s.spec = before;
    s.spec.row_span = hi_row - lo_row;
    s.spec.col_span = hi_col - lo_col;
    s.before = before;
    out.steps.push_back(Step{s});

    for (const CellRef& c : cells) {
        if (c.cell == anchor) continue;
        const Node* n = findNode(doc, c.cell);
        const Node* row = parentOf(doc, c.cell);
        if (n == nullptr || row == nullptr) {
            out.error = "合并：有一格找不到它所在的行";
            return out;
        }
        const std::vector<const Node*> siblings = cellsOf(*row);
        int index = -1;
        for (size_t i = 0; i < siblings.size(); ++i) {
            if (siblings[i]->id == c.cell) {
                index = static_cast<int>(i);
                break;
            }
        }
        if (index < 0) {
            out.error = "合并：有一格不在它所在的行里";
            return out;
        }
        Step step;
        const NodeSpec removed = toSpec(*n);
        if (!makeRemove(flow, Pos{row->id, index}, removed, &step)) {
            out.error = "合并：删除点不在流里";
            return out;
        }
        pushMapped(&out.steps, std::move(step));
    }

    out.ok = true;
    return out;
}

TableSteps splitCell(const Doc& doc, const FlowIndex& flow, const CellRef& cell) {
    TableSteps out;
    TableGrid grid;
    if (!tableGrid(doc, cell.table, &grid, &out.error)) return out;
    const Node* target = findNode(doc, cell.cell);
    if (target == nullptr || target->kind != NodeKind::TableCell) {
        out.error = "拆分：目标不是单元格";
        return out;
    }
    CellRect rect;
    if (!rectOf(grid, cell.cell, &rect)) {
        out.error = "拆分：目标格不在网格里";
        return out;
    }
    if (rect.rows == 1 && rect.cols == 1) {
        out.error = "拆分：这一格没有合并";
        return out;
    }

    const Node* t = findNode(doc, cell.table);
    const std::vector<const Node*> rows = rowsOf(*t);
    if (rect.row < 0 || rect.row + rect.rows > static_cast<int>(rows.size())) {
        out.error = "拆分：覆盖的行超出表格";
        return out;
    }

    // 一、锚点回到 1x1（记录不动位置，先收）。
    const CellSpec before = cellSpecOf(*target);
    SetCellSpec s;
    s.id = cell.cell;
    s.spec = before;
    s.spec.row_span = 1;
    s.spec.col_span = 1;
    s.before = before;
    out.steps.push_back(Step{s});

    // 二、锚点那一行补右侧的格：插在锚点之后。
    const Node* anchor_row = rows[static_cast<size_t>(rect.row)];
    const std::vector<const Node*> anchor_cells = cellsOf(*anchor_row);
    int anchor_index = -1;
    for (size_t i = 0; i < anchor_cells.size(); ++i) {
        if (anchor_cells[i]->id == cell.cell) {
            anchor_index = static_cast<int>(i);
            break;
        }
    }
    if (anchor_index < 0) {
        out.error = "拆分：锚点不在它所在的行里";
        return out;
    }
    for (int k = 1; k < rect.cols; ++k) {
        Step step;
        if (!makeInsert(flow, Pos{anchor_row->id, anchor_index + k}, newCellSpec(1), &step)) {
            out.error = "拆分：插入点不在流里";
            return out;
        }
        pushMapped(&out.steps, std::move(step));
    }

    // 三、下面每一行补 rect.cols 个格：插在"原点列不小于锚点左边界"的第一格之前。
    for (int r = rect.row + 1; r < rect.row + rect.rows; ++r) {
        const Node* row = rows[static_cast<size_t>(r)];
        const std::vector<const Node*> siblings = cellsOf(*row);
        int index = static_cast<int>(siblings.size());
        for (size_t i = 0; i < siblings.size(); ++i) {
            if (grid.originColOf(siblings[i]->id) >= rect.col) {
                index = static_cast<int>(i);
                break;
            }
        }
        for (int k = 0; k < rect.cols; ++k) {
            Step step;
            if (!makeInsert(flow, Pos{row->id, index + k}, newCellSpec(1), &step)) {
                out.error = "拆分：插入点不在流里";
                return out;
            }
            pushMapped(&out.steps, std::move(step));
        }
    }

    out.ok = true;
    return out;
}

}  // namespace heditor::doc
