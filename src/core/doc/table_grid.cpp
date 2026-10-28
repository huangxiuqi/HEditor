#include "core/doc/table_grid.h"

namespace heditor {

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

bool buildGridFromSpans(const std::vector<std::vector<GridCellSpan>>& rows, TableGrid* out,
                        std::string* error) {
    out->rows = static_cast<int>(rows.size());
    int cols = 0;
    if (!rows.empty()) {
        for (const GridCellSpan& c : rows.front()) cols += c.col_span;
    }
    out->cols = cols;

    const size_t total = static_cast<size_t>(out->rows) * static_cast<size_t>(out->cols);
    out->slots.assign(total, GridCell{});

    auto occupied = [&](int r, int c) -> bool {
        const GridCell& slot = out->slots[static_cast<size_t>(r) * static_cast<size_t>(out->cols) +
                                          static_cast<size_t>(c)];
        return slot.id != kInvalidNodeId;
    };

    for (int r = 0; r < out->rows; ++r) {
        int cursor = 0;
        for (const GridCellSpan& cell : rows[static_cast<size_t>(r)]) {
            while (cursor < out->cols && occupied(r, cursor)) ++cursor;

            const int rs = cell.row_span;
            const int cs = cell.col_span;
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
                    slot.id = cell.id;
                    slot.row = r;
                    slot.col = cursor;
                }
            }
            cursor += cs;
        }

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

}  // namespace heditor
