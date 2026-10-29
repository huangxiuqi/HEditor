#include "core/doc/subtree.h"

#include "core/doc/utf8.h"

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

bool hasLineBreak(const std::string& utf8) {
    return utf8.find_first_of("\r\n") != std::string::npos;
}

std::vector<NodeSpec> nodesFromPlainText(const std::string& utf8) {
    std::vector<NodeSpec> nodes;
    std::string line;
    const auto flush = [&nodes, &line]() {
        if (!line.empty()) {
            NodeSpec run;
            run.kind = NodeKind::TextRun;
            run.text = utf8ToCodepoints(line);
            nodes.push_back(std::move(run));
        }
        line.clear();
    };
    for (size_t i = 0; i < utf8.size(); ++i) {
        const char c = utf8[i];
        if (c != '\n' && c != '\r') {
            line.push_back(c);
            continue;
        }
        // CRLF 是两个字符一个换行：吃掉紧随的 LF，否则会多出一个空段落。
        if (c == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n') ++i;
        flush();
        NodeSpec mark;
        mark.kind = NodeKind::ParagraphMark;
        nodes.push_back(std::move(mark));
    }
    flush();
    return nodes;
}
}  // namespace heditor
