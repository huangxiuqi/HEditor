/**
 * 命令层的表格与文档级记录：行列增删、合并拆分、三层属性、图片位移、水印与纸张。
 *
 * 表格命令的主语一律是"光标所在那一格"，拒绝条件在引擎侧判定。
 */
#include "core/engine/edit_command.h"
#include "core/engine/edit_command_util.h"

#include "core/doc/subtree.h"
#include "core/doc/table_grid.h"
#include "core/engine/control_spans.h"
#include "core/engine/structure_index.h"
#include "core/utils/utf8.h"

#include <algorithm>
#include <functional>
#include <variant>

namespace heditor {

using namespace detail;

bool EditCommand::currentTableCell(doc::CellRef* out, int* row, int* col) const {
    if (!hasFlow() || out == nullptr) return false;
    const int para = pos().caret().para;
    const CellNode* node =
        (view_.structure == nullptr) ? nullptr : view_.structure->cells_index.nodeAt(para);
    if (node == nullptr) return false;
    const CellSlot& slot = node->slot;
    if (!doc::cellRefAt(doc(), slot.table, slot.row, slot.cell, out)) return false;
    if (row != nullptr || col != nullptr) {
        int r = 0;
        int c = 0;
        if (!doc::cellOrigin(doc(), out->table, out->cell, &r, &c)) return false;
        if (row != nullptr) *row = r;
        if (col != nullptr) *col = c;
    }
    return true;
}

bool EditCommand::caretIntoCell(NodeId table, int row, int col) {
    TableGrid grid;
    std::string why;
    if (!doc::tableGrid(doc(), table, &grid, &why)) return false;
    if (grid.rows <= 0 || grid.cols <= 0) return false;
    const int r = std::max(0, std::min(row, grid.rows - 1));
    const int c = std::max(0, std::min(col, grid.cols - 1));
    const GridCell* slot = grid.at(r, c);
    if (slot == nullptr) return false;
    // 落在合并区域里时给的是原点格 —— 光标要落进那一格的内容，不是那个格位。
    doc::CellRef target;
    if (!doc::cellRefOf(doc(), table, slot->row, slot->col, &target)) return false;
    if (view_.structure == nullptr) return false;
    for (const CellNode& n : view_.structure->cells_index.nodes) {
        if (n.slot.table != static_cast<uint64_t>(target.table)) continue;
        if (n.slot.row != target.row_index || n.slot.cell != target.cell_index) continue;
        if (n.first_block < 0 || n.first_block >= static_cast<int>(blockFlow().size())) {
            return false;
        }
        pos().setCaretAtFlow(blockFlow()[static_cast<size_t>(n.first_block)]);
        return true;
    }
    return false;
}

bool EditCommand::insertTableRow(bool below) {
    if (!hasFlow()) return false;
    // 改表结构是设计动作：表单模式在填值、只读模式不许改。
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    int row = 0;
    int col = 0;
    if (!currentTableCell(&cell, &row, &col)) {
        noteBlocked();
        return false;
    }
    doc::TableSteps batch = doc::insertRow(doc(), flowIndex(), cell, below);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    // 光标进新行的那一列：插完就接着填，是"插入行"最常见的下一步。
    caretIntoCell(cell.table, below ? row + 1 : row, col);
    return true;
}

bool EditCommand::removeTableRow() {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    int row = 0;
    int col = 0;
    if (!currentTableCell(&cell, &row, &col)) {
        noteBlocked();
        return false;
    }
    doc::TableSteps batch = doc::removeRow(doc(), flowIndex(), cell);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    // 目标行号夹在删完之后的范围里：删的是最后一行时落到新的最后一行。
    caretIntoCell(cell.table, row, col);
    return true;
}

bool EditCommand::insertTableColumn(bool right) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    int row = 0;
    int col = 0;
    if (!currentTableCell(&cell, &row, &col)) {
        noteBlocked();
        return false;
    }
    doc::TableSteps batch = doc::insertColumn(doc(), flowIndex(), cell, right);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    // 插在左边时这一格整体右移一列，光标跟着它走（落在原内容上，不落到新插的空列里）。
    caretIntoCell(cell.table, row, right ? col : col + 1);
    return true;
}

bool EditCommand::removeTableColumn() {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    int row = 0;
    int col = 0;
    if (!currentTableCell(&cell, &row, &col)) {
        noteBlocked();
        return false;
    }
    doc::TableSteps batch = doc::removeColumn(doc(), flowIndex(), cell);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    // 这一列没了：光标落到顶替它的那一格（同列号，夹到新的列数里）。
    caretIntoCell(cell.table, row, col);
    return true;
}

bool EditCommand::mergeTableCells() {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const std::vector<CaretResolver::CellBlocks>& blocks_of_cells = pos().cellBlocks();
    if (blocks_of_cells.size() < 2) {
        // 没有整格选区就没有"要合并的那一片"：不替用户猜"和右边那一格并"。
        noteBlocked();
        return false;
    }
    std::vector<doc::CellRef> cells;
    for (const CaretResolver::CellBlocks& cb : blocks_of_cells) {
        if (cb.empty() || cb.first < 0) {
            noteBlocked();
            return false;
        }
        const CellNode* node =
            (view_.structure == nullptr) ? nullptr : view_.structure->cells_index.nodeAt(cb.first);
        if (node == nullptr) {
            noteBlocked();
            return false;
        }
        const CellSlot& slot = node->slot;
        doc::CellRef ref;
        if (!doc::cellRefAt(doc(), slot.table, slot.row, slot.cell, &ref)) {
            noteBlocked();
            return false;
        }
        cells.push_back(ref);
    }
    const NodeId table = cells.front().table;
    doc::TableSteps batch = doc::mergeCells(doc(), flowIndex(), cells);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    // 合并之后的落点是左上角那一格：整格选区随之收成"光标在合并后的那一格里"。
    int lo_row = 0;
    int lo_col = 0;
    {
        bool found = false;
        for (const doc::CellRef& c : cells) {
            int r = 0;
            int cc = 0;
            if (!doc::cellOrigin(doc(), table, c.cell, &r, &cc)) continue;
            if (!found || r < lo_row || (r == lo_row && cc < lo_col)) {
                lo_row = r;
                lo_col = cc;
                found = true;
            }
        }
        if (!found) {
            noteBlocked();
            return false;
        }
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    caretIntoCell(table, lo_row, lo_col);
    return true;
}

bool EditCommand::splitTableCell() {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    int row = 0;
    int col = 0;
    if (!currentTableCell(&cell, &row, &col)) {
        noteBlocked();
        return false;
    }
    doc::TableSteps batch = doc::splitCell(doc(), flowIndex(), cell);
    if (!batch.ok || batch.steps.empty()) {
        noteBlocked();
        return false;
    }
    pending_before_ = pos().selection();
    pending_after_ = pending_before_;
    const int dirty = pos().caret().para;
    if (!commit(std::move(batch.steps), dirty)) {
        noteBlocked();
        return false;
    }
    // 拆完留在左上角那一格：内容在它身上，用户的注意力也在它身上。
    caretIntoCell(cell.table, row, col);
    return true;
}

NodeId EditCommand::tableAtPara(int para) const {
    if (!view_.tables || view_.structure == nullptr) return kInvalidNodeId;
    const CellRef ref = view_.structure->cellAt(para);
    if (ref.table < 0 || ref.table >= static_cast<int>(view_.tables->size())) {
        return kInvalidNodeId;
    }
    return static_cast<NodeId>((*view_.tables)[static_cast<size_t>(ref.table)].handle);
}

bool EditCommand::removeTableAtCaret() {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const NodeId id = tableAtPara(pos().caret().para);
    if (id == kInvalidNodeId) {
        noteBlocked();
        return false;
    }
    FlowPos lo = 0;
    FlowPos hi = 0;
    if (!flowIndex().intervalOf(id, &lo, &hi) || hi <= lo) {
        noteBlocked();
        return false;
    }
    // 表是文档里唯一的内容时拒绝：删完一个块都不剩，光标没有能停的地方。
    int first_b = -1;
    int last_b = -1;
    const CellRef ref = view_.structure->cellAt(pos().caret().para);
    if (ref.table >= 0) {
        const TableSpan& span = view_.structure->tableSpan(ref.table);
        first_b = span.first_block;
        last_b = span.last_block;
    }
    if (first_b <= 0 && last_b >= static_cast<int>(blocks().size()) - 1) {
        noteBlocked();
        return false;
    }
    if (!doc::canEditRange(doc(), mode(), Range{lo, hi}).allowed) {
        noteBlocked();
        return false;
    }
    RemoveNode r;
    r.at_flow = lo;
    std::vector<Step> steps;
    steps.push_back(Step{r});
    pending_before_ = pos().selection();
    pending_after_ = Range{lo, lo};
    const int dirty = pos().caret().para;
    if (!commit(std::move(steps), dirty)) {
        noteBlocked();
        return false;
    }
    pos().setCaretAtFlow(lo);
    return true;
}

bool EditCommand::setCellSpec(const CellPatch& patch) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    if (!currentTableCell(&cell, nullptr, nullptr)) {
        noteBlocked();
        return false;
    }
    const doc::Node* n = doc::findNode(doc(), cell.cell);
    if (n == nullptr || n->kind != NodeKind::TableCell) return false;
    const auto* cur = std::get_if<CellSpec>(&n->payload);
    if (cur == nullptr) return false;

    CellSpec next = *cur;
    if (patch.v_align.has_value()) next.v_align = *patch.v_align;
    if (next == *cur) return true;  // 幂等：不提交、不进撤销栈

    SetCellSpec s;
    s.id = cell.cell;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    // 改记录不动光标：留着 null Range 会被当成"文档开头"。
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    const int dirty = pos().caret().para;
    if (!commit(std::move(steps), dirty)) {
        noteBlocked();
        return false;
    }
    return true;
}

bool EditCommand::setRowSpec(const RowPatch& patch) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    if (!currentTableCell(&cell, nullptr, nullptr)) {
        noteBlocked();
        return false;
    }
    const doc::Node* n = doc::findNode(doc(), cell.row);
    if (n == nullptr || n->kind != NodeKind::TableRow) return false;
    const auto* cur = std::get_if<RowSpec>(&n->payload);
    if (cur == nullptr) return false;

    RowSpec next = *cur;
    if (patch.is_header.has_value()) next.is_header = *patch.is_header;
    if (patch.can_split_by_page.has_value()) next.can_split_by_page = *patch.can_split_by_page;
    if (patch.page_break_before.has_value()) next.page_break_before = *patch.page_break_before;
    if (patch.height_pt.has_value()) {
        const float pt = *patch.height_pt;
        next.height = pt > 0.0f ? Length::abs(ptToEmu(pt)) : Length{};
    }
    if (patch.height_rule.has_value()) next.height_rule = *patch.height_rule;
    if (next == *cur) return true;

    SetRowSpec s;
    s.id = cell.row;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    const int dirty = pos().caret().para;
    if (!commit(std::move(steps), dirty)) {
        noteBlocked();
        return false;
    }
    return true;
}

bool EditCommand::setTableSpec(const TablePatch& patch) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef cell;
    if (!currentTableCell(&cell, nullptr, nullptr)) {
        noteBlocked();
        return false;
    }
    const doc::Node* n = doc::findNode(doc(), cell.table);
    if (n == nullptr || n->kind != NodeKind::Table) return false;
    const auto* cur = std::get_if<TableSpec>(&n->payload);
    if (cur == nullptr) return false;

    TableSpec next = *cur;
    if (patch.width_pt.has_value()) {
        const float pt = *patch.width_pt;
        next.width = pt > 0.0f ? Length::abs(ptToEmu(pt)) : Length{};
    }
    if (patch.align.has_value()) next.align = *patch.align;
    if (next == *cur) return true;

    SetTableSpec s;
    s.id = cell.table;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    const int dirty = pos().caret().para;
    if (!commit(std::move(steps), dirty)) {
        noteBlocked();
        return false;
    }
    return true;
}

/**
 * 把一张表的全部列宽钉成显式值。宽度 <= 0 的那一列保持 Auto（与拖拽无关的列原样保留）。
 */
bool EditCommand::setTableGridCols(uint64_t handle, const std::vector<float>& widths) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const NodeId id = static_cast<NodeId>(handle);
    const doc::Node* n = doc::findNode(doc(), id);
    if (n == nullptr || n->kind != NodeKind::Table) return false;
    const auto* cur = std::get_if<TableSpec>(&n->payload);
    if (cur == nullptr) return false;

    TableSpec next = *cur;
    next.grid_cols.clear();
    next.grid_cols.reserve(widths.size());
    float sum = 0.0f;
    for (const float w : widths) {
        next.grid_cols.push_back(w > 0.0f ? Length::abs(ptToEmu(w)) : Length{});
        if (w > 0.0f) sum += w;
    }
    // 表宽一起钉住。不钉的话表还是"撑满版心"：各列之和超过它时整体按比例缩回去，
    //   拖右边框那一下看起来什么都没发生（列宽写入成功、读回的值也对，画面没动）。
    if (sum > 0.0f) next.width = Length::abs(ptToEmu(sum));
    if (next == *cur) return true;

    SetTableSpec s;
    s.id = id;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    if (!commit(std::move(steps), pos().caret().para)) {
        noteBlocked();
        return false;
    }
    return true;
}

/**
 * 改一行的高度。落成"不小于"：拖高之后内容多了仍能长，这是 Word 的行为。
 */
bool EditCommand::setTableRowHeight(uint64_t handle, int row, float h_pt) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    doc::CellRef ref{};
    if (!doc::cellRefAt(doc(), handle, row, 0, &ref)) return false;
    const doc::Node* n = doc::findNode(doc(), ref.row);
    if (n == nullptr || n->kind != NodeKind::TableRow) return false;
    const auto* cur = std::get_if<RowSpec>(&n->payload);
    if (cur == nullptr) return false;

    RowSpec next = *cur;
    if (h_pt > 0.0f) {
        next.height = Length::abs(ptToEmu(h_pt));
        next.height_rule = HeightRule::AtLeast;
    } else {
        next.height = Length{};
        next.height_rule = HeightRule::Auto;
    }
    if (next == *cur) return true;

    SetRowSpec s;
    s.id = ref.row;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    if (!commit(std::move(steps), pos().caret().para)) {
        noteBlocked();
        return false;
    }
    return true;
}

/**
 * 把一段内容搬到 dest。删除与插入是同一批步骤：撤销只有一步，位置映射也按同一批算。
 */
bool EditCommand::moveRange(FlowPos lo, FlowPos hi, FlowPos dest,
                            const std::vector<NodeSpec>& nodes) {
    if (!hasFlow() || nodes.empty()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    if (!(hi > lo)) return false;
    // 目标落在源区间内部：搬过去再删掉，等于原地不动 —— 拒绝比"看起来什么都没发生"好。
    if (dest >= lo && dest <= hi) return false;
    // 块级元素（表格）不搬：整张表插进段落中间没有语义，而拆开它要另一套结构操作。
    if (hasBlockLevel(nodes)) return false;

    const FlowPos len = hi - lo;
    const FlowPos at = (dest >= hi) ? dest - len : dest;
    std::vector<Step> steps = eraseFlowSteps(lo, hi);
    if (steps.empty()) return false;
    FlowPos cur = at;
    for (const NodeSpec& spec : nodes) {
        InsertNode in;
        in.at_flow = cur;
        in.at = flowIndex().posAt(cur);
        in.subtree = spec;
        steps.push_back(Step{in});
        cur += static_cast<FlowPos>(spec.span());
    }
    pending_before_ = pos().selection();
    pending_after_ = Range{cur, cur};
    if (!commit(std::move(steps), pos().paraForFlow(at))) {
        noteBlocked();
        return false;
    }
    pos().setCaretAtFlow(cur);
    return true;
}

/**
 * 把一张图片搬到 dest。图片是原子，在流里占一格；搬它的位置就是"删一格 + 插一格"。
 */
bool EditCommand::moveImage(NodeId id, const ImageSpec& spec, FlowPos dest) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const doc::Node* parent = doc::parentOf(doc(), id);
    if (parent == nullptr) return false;
    int slot = -1;
    for (size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i]->id == id) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) return false;
    const FlowPos at = flowIndex().flowAt(Pos{parent->id, slot});
    if (at < 0) return false;
    // 原地：落点紧挨着图片（它前面或它后面）都不算移动。
    if (dest == at || dest == at + 1) return false;

    const FlowPos insert_at = (dest > at) ? dest - 1 : dest;
    NodeSpec sub;
    sub.kind = NodeKind::Image;
    sub.image = spec;

    std::vector<Step> steps;
    RemoveNode r;
    r.at_flow = at;
    steps.push_back(Step{r});
    InsertNode in;
    in.at_flow = insert_at;
    in.at = flowIndex().posAt(insert_at);
    in.subtree = sub;
    steps.push_back(Step{in});

    pending_before_ = pos().selection();
    pending_after_ = Range{insert_at + 1, insert_at + 1};
    if (!commit(std::move(steps), pos().paraForFlow(insert_at))) {
        noteBlocked();
        return false;
    }
    pos().setCaretAtFlow(insert_at + 1);
    return true;
}

/**
 * 换一张图的整份记录。一次一步撤销，与改尺寸同一档。
 */
bool EditCommand::setImageSpec(NodeId id, const ImageSpec& spec) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const doc::Node* n = doc::findNode(doc(), id);
    if (n == nullptr || n->kind != NodeKind::Image) return false;
    const auto* cur = std::get_if<ImageSpec>(&n->payload);
    if (cur == nullptr) return false;
    if (*cur == spec) return true;

    SetImageSpec s;
    s.id = id;
    s.spec = spec;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    if (!commit(std::move(steps), pos().caret().para)) {
        noteBlocked();
        return false;
    }
    return true;
}

/**
 * 改第一节的水印。脏段取第 0 段：水印画在每一页上，没有更窄的影响面。
 */
bool EditCommand::setWatermark(const WatermarkSpec& w) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const doc::Doc& d = doc();
    if (!d.root || d.root->children.empty()) return false;
    const doc::Node* sec = d.root->children.front().get();
    if (sec == nullptr || sec->kind != NodeKind::Section) return false;
    const auto* cur = std::get_if<SectionSpec>(&sec->payload);
    if (cur == nullptr) return false;
    if (cur->watermark == w) return true;

    SectionSpec next = *cur;
    next.watermark = w;
    SetSectionSpec s;
    s.id = sec->id;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    if (!commit(std::move(steps), 0)) {
        noteBlocked();
        return false;
    }
    return true;
}

/**
 * 改第一节的纸张与页边距。脏段取第 0 段：它们决定整篇的页度量，没有更窄的影响面。
 */
bool EditCommand::setSectionPaper(const Paper& paper, const SectionMargins& margins) {
    if (!hasFlow()) return false;
    if (mode() != EditMode::Document) {
        noteBlocked();
        return false;
    }
    const doc::Doc& d = doc();
    if (!d.root || d.root->children.empty()) return false;
    const doc::Node* sec = d.root->children.front().get();
    if (sec == nullptr || sec->kind != NodeKind::Section) return false;
    const auto* cur = std::get_if<SectionSpec>(&sec->payload);
    if (cur == nullptr) return false;
    if (cur->paper == paper && cur->margins == margins) return true;

    SectionSpec next = *cur;
    next.paper = paper;
    next.margins = margins;
    SetSectionSpec s;
    s.id = sec->id;
    s.spec = next;
    s.before = *cur;
    std::vector<Step> steps;
    steps.push_back(Step{s});
    pending_before_ = pos().selection();
    pending_after_ = pos().selection();
    if (!commit(std::move(steps), 0)) {
        noteBlocked();
        return false;
    }
    return true;
}

}  // namespace heditor
