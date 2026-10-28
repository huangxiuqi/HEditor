#include "core/doc/subtree.h"

namespace heditor {

int NodeSpec::span() const {
    if (isText(kind)) return static_cast<int>(text.size());
    if (!isContainer(kind)) return 1;
    int total = (kind == NodeKind::Document) ? 0 : 2;
    for (const NodeSpec& c : children) total += c.span();
    return total;
}

bool makeEmptyTableSpec(int rows, int cols, bool header, NodeSpec* out) {
    if (out == nullptr) return false;
    if (rows < 1 || rows > kMaxNewTableRows) return false;
    if (cols < 1 || cols > kMaxNewTableCols) return false;

    NodeSpec table;
    table.kind = NodeKind::Table;
    table.children.reserve(static_cast<size_t>(rows));
    for (int r = 0; r < rows; ++r) {
        NodeSpec row;
        row.kind = NodeKind::TableRow;
        row.row.is_header = header && r == 0;
        row.children.reserve(static_cast<size_t>(cols));
        for (int c = 0; c < cols; ++c) {
            NodeSpec cell;
            cell.kind = NodeKind::TableCell;
            NodeSpec mark;
            mark.kind = NodeKind::ParagraphMark;
            cell.children.push_back(std::move(mark));
            row.children.push_back(std::move(cell));
        }
        table.children.push_back(std::move(row));
    }
    *out = std::move(table);
    return true;
}
}  // namespace heditor
