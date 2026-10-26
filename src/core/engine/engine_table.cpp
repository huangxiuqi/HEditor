/**
 * 表格的用户命令：插删行列、合并拆分、三层记录的属性修改，以及两条读（现状与落点归属）。
 *
 * 结构操作全是"一批步骤 + 一次提交"，与插入表格同一条路；重排与重画由 AndRender 那一层
 * 统一补，所以这里没有第二套刷新逻辑。
 */
#include "core/engine/engine.h"

#include "core/engine/payload_schema.h"
#include "core/rev/table_ops.h"
#include "core/utils/units.h"

#include <sstream>

namespace heditor {

namespace {

/**
 * 一条长度记录折成磅。Auto 与相对长度给 -1（"没有绝对尺寸"）。
 * @param len 长度
 * @return 磅值；不是绝对长度时为 -1
 */
float ptOfLength(const Length& len) {
    return len.kind == LengthKind::Abs ? static_cast<float>(emuToPt(len.abs_emu)) : -1.0f;
}

/**
 * 表格直接孩子里的行（跳过杂孩子，理由同投影）。
 * @param node 表格节点
 * @return 行节点
 */
std::vector<const rev::Node*> tableRows(const rev::Node& node) {
    std::vector<const rev::Node*> rows;
    for (const rev::Ref& c : node.children) {
        if (c->kind == NodeKind::TableRow) rows.push_back(c.get());
    }
    return rows;
}

}  // namespace

bool EditorEngine::tableOp(TableOp op) {
    if (!loaded_) return false;
    switch (op) {
        case TableOp::InsertRowAbove:
            return edit_.insertTableRow(false);
        case TableOp::InsertRowBelow:
            return edit_.insertTableRow(true);
        case TableOp::RemoveRow:
            return edit_.removeTableRow();
        case TableOp::InsertColumnLeft:
            return edit_.insertTableColumn(false);
        case TableOp::InsertColumnRight:
            return edit_.insertTableColumn(true);
        case TableOp::RemoveColumn:
            return edit_.removeTableColumn();
        case TableOp::MergeCells:
            return edit_.mergeTableCells();
        case TableOp::SplitCell:
            return edit_.splitTableCell();
    }
    return false;
}

bool EditorEngine::tableOpAndRender(TableOp op) {
    if (!tableOp(op)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::setCellSpec(const CellPatch& patch) {
    if (!loaded_) return false;
    return edit_.setCellSpec(patch);
}

bool EditorEngine::setCellSpecAndRender(const CellPatch& patch) {
    if (!setCellSpec(patch)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::setRowSpec(const RowPatch& patch) {
    if (!loaded_) return false;
    return edit_.setRowSpec(patch);
}

bool EditorEngine::setRowSpecAndRender(const RowPatch& patch) {
    if (!setRowSpec(patch)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::setTableSpec(const TablePatch& patch) {
    if (!loaded_) return false;
    return edit_.setTableSpec(patch);
}

bool EditorEngine::setTableSpecAndRender(const TablePatch& patch) {
    if (!setTableSpec(patch)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

std::string EditorEngine::tableStateJson() const {
    const TextPos at = caret();
    const LayoutTable* table = nullptr;
    const LayoutCell* cell = layoutCellAt(at.para, &table);
    if (!loaded_ || !session_ || cell == nullptr || table == nullptr) {
        std::ostringstream none;
        none << "{\"inTable\":0,\"rows\":0,\"cols\":0,\"widthPt\":-1,\"align\":0,\"headerRows\":0"
             << ",\"row\":0,\"rowHeader\":0,\"rowSplit\":0,\"rowBreak\":0,\"heightPt\":-1"
             << ",\"heightRule\":0,\"cellRow\":0,\"cellCol\":0,\"rowSpan\":1,\"colSpan\":1"
             << ",\"vAlign\":0,\"selCells\":0,\"selRows\":0,\"selCols\":0}";
        return pk::checked(pk::table_state::kSpec, none.str());
    }

    const rev::Doc& doc = document();
    const TableSpec table_spec = [&]() {
        const rev::Node* n = rev::findNode(doc, static_cast<NodeId>(table->handle));
        if (n == nullptr) return TableSpec{};
        const auto* s = std::get_if<TableSpec>(&n->payload);
        return s == nullptr ? TableSpec{} : *s;
    }();

    // 标题行是逐行开关，"几行"只是数出来的：遇到第一个非标题行就停。
    int header_rows = 0;
    {
        const rev::Node* t = rev::findNode(doc, static_cast<NodeId>(table->handle));
        if (t != nullptr) {
            for (const rev::Node* row : tableRows(*t)) {
                const auto* rs = std::get_if<RowSpec>(&row->payload);
                if (rs == nullptr || !rs->is_header) break;
                ++header_rows;
            }
        }
    }

    RowSpec row_spec;
    rev::CellRef ref;
    if (rev::cellRefOf(doc, static_cast<NodeId>(table->handle), cell->row, cell->cell, &ref)) {
        if (const rev::Node* n = rev::findNode(doc, ref.row)) {
            if (const auto* rs = std::get_if<RowSpec>(&n->payload)) row_spec = *rs;
        }
    }
    CellSpec cell_spec;
    if (ref.cell != kInvalidNodeId) {
        if (const rev::Node* n = rev::findNode(doc, ref.cell)) {
            if (const auto* cs = std::get_if<CellSpec>(&n->payload)) cell_spec = *cs;
        }
    }

    // 整格选区的范围：格数直接数，行/列范围按格的坐标包络算。没有整格选区时三项都是 0。
    int sel_cells = 0;
    int sel_lo_row = 0;
    int sel_hi_row = 0;
    int sel_lo_col = 0;
    int sel_hi_col = 0;
    bool sel_any = false;
    for (const CaretResolver::CellBlocks& cb : caret_.cellBlocks()) {
        if (cb.empty()) continue;
        const int para = cb.first;
        if (para < 0 || para >= static_cast<int>(blocks_full_.size())) continue;
        const std::vector<CellSlot>& path = blocks_full_[static_cast<size_t>(para)].cell_path;
        if (path.empty()) continue;
        const CellSlot& slot = path.back();
        ++sel_cells;
        if (!sel_any || slot.row < sel_lo_row) sel_lo_row = slot.row;
        if (!sel_any || slot.row > sel_hi_row) sel_hi_row = slot.row;
        if (!sel_any || slot.cell < sel_lo_col) sel_lo_col = slot.cell;
        if (!sel_any || slot.cell > sel_hi_col) sel_hi_col = slot.cell;
        sel_any = true;
    }

    std::ostringstream o;
    o << "{\"inTable\":1";
    o << ",\"rows\":" << table->rows;
    o << ",\"cols\":" << table->cols;
    o << ",\"widthPt\":" << ptOfLength(table_spec.width);
    o << ",\"align\":" << static_cast<int>(table_spec.align);
    o << ",\"headerRows\":" << header_rows;
    o << ",\"row\":" << cell->row;
    o << ",\"rowHeader\":" << (row_spec.is_header ? 1 : 0);
    o << ",\"rowSplit\":" << (row_spec.can_split_by_page ? 1 : 0);
    o << ",\"rowBreak\":" << (row_spec.page_break_before ? 1 : 0);
    o << ",\"heightPt\":" << ptOfLength(row_spec.height);
    o << ",\"heightRule\":" << static_cast<int>(row_spec.height_rule);
    o << ",\"cellRow\":" << cell->row;
    o << ",\"cellCol\":" << cell->cell;
    o << ",\"rowSpan\":" << cell_spec.row_span;
    o << ",\"colSpan\":" << cell_spec.col_span;
    o << ",\"vAlign\":" << static_cast<int>(cell_spec.v_align);
    o << ",\"selCells\":" << sel_cells;
    o << ",\"selRows\":" << (sel_any ? sel_hi_row - sel_lo_row + 1 : 0);
    o << ",\"selCols\":" << (sel_any ? sel_hi_col - sel_lo_col + 1 : 0);
    o << "}";
    return pk::checked(pk::table_state::kSpec, o.str());
}

std::string EditorEngine::contextTargetJson(float x_pt_doc, float y_pt_doc) const {
    TextPos at{};
    int owner = -1;
    int cell_para = -1;
    const bool on_text = hitTestPt(x_pt_doc, y_pt_doc, &at, &owner, nullptr, nullptr, &cell_para);
    const ImageHit image = imageAtPt(x_pt_doc, y_pt_doc);

    const LayoutTable* table = nullptr;
    const LayoutCell* cell = on_text ? layoutCellAt(at.para, &table) : nullptr;

    // 优先级 = 最具体的那个：图片画在最上层，先认它；控件是行内容器，其次；表格再次。
    int kind = -1;
    if (image.index >= 0) {
        kind = 3;
    } else if (on_text && owner >= 0) {
        kind = 2;
    } else if (cell != nullptr && table != nullptr) {
        kind = 1;
    } else if (on_text) {
        kind = 0;
    }

    std::ostringstream o;
    o << "{\"kind\":" << kind;
    o << ",\"para\":" << (on_text ? at.para : -1);
    o << ",\"offset\":" << (on_text ? at.offset : 0);
    o << ",\"cellTable\":" << (cell != nullptr && table != nullptr ? table->handle : 0);
    o << ",\"cellRow\":" << (cell != nullptr ? cell->row : -1);
    o << ",\"cellCol\":" << (cell != nullptr ? cell->cell : -1);
    o << ",\"control\":" << (on_text ? owner : -1);
    o << ",\"image\":" << image.index;
    o << "}";
    return pk::checked(pk::context_target::kSpec, o.str());
}

}  // namespace heditor
