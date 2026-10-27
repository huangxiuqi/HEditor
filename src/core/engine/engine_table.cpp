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

#include <cmath>
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

/**
 * 拖格线时"落在格线附近"的容差（磅）。三条线都从这里取：太窄要瞄准像素、太宽会抢走
 * 贴边落光标的那几磅。
 */
constexpr float kGridLineTolPt = 6.0f;

/**
 * 文档 y 折成"第几页 + 页内磅"。
 *
 * 页在文档里按"页高 + 页间隙"的像素步距摆，折回磅只能反过来除密度 —— 与命中测试同一个
 * 口径。落在页间隙里的点既不在正文上，也不在表上，判假。
 *
 * @param y_pt_doc 文档纵坐标（磅）
 * @param zoom 缩放
 * @param page_px_h 一页的像素高
 * @param gap_px 页间隙（像素）
 * @param page 写回页号
 * @param local_y 写回页内磅
 * @return true = 这一点落在某一页上
 */
bool pageLocalYAt(float y_pt_doc, float zoom, int page_px_h, int gap_px, int* page,
                  float* local_y) {
    *page = 0;
    *local_y = y_pt_doc;
    const int step_px = page_px_h + gap_px;
    if (step_px <= 0 || !(zoom > 0.0f)) return true;
    const float y_px = y_pt_doc * zoom;
    const int p = std::max(0, static_cast<int>(std::floor(y_px / static_cast<float>(step_px))));
    const float local_px = y_px - static_cast<float>(p) * static_cast<float>(step_px);
    if (local_px < 0.0f || local_px > static_cast<float>(page_px_h)) return false;
    *page = p;
    *local_y = local_px / zoom;
    return true;
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
        case TableOp::RemoveTable:
            return edit_.removeTableAtCaret();
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

/**
 * 点所在的表格栅格：宿主拖拽列边界 / 行边界时用它换算指针位置。
 *
 * 折页与 imageAtPt 同一条（按宿主摆页的像素步距），两处各写一套的话页号会各自累出偏差。
 */
std::string EditorEngine::tableGridJson(float x_pt_doc, float y_pt_doc) const {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    int page = 0;
    float local_y = 0.0f;
    if (!pageLocalYAt(y_pt_doc, cfg_.zoom, pagePxHeight(), pageGapPx(), &page, &local_y)) {
        return pk::checked(pk::table_grid::kSpec, "{\"ok\":false}");
    }
    for (const PlacedTable& pt : layout_.placed_tables) {
        if (x_pt_doc < pt.x - 1.0f || x_pt_doc > pt.x + pt.w + 1.0f) continue;
        for (const PlacedRow& pr : pt.placed) {
            if (pr.page != page) continue;
            if (local_y < pr.y - 1.0f || local_y > pr.y + pr.h + 1.0f) continue;
            o << "{\"ok\":true,\"table\":" << pt.handle << ",\"page\":" << page << ",\"x\":" << pt.x
              << ",\"w\":" << pt.w << ",\"cols\":[";
            for (size_t i = 0; i < pt.col_x.size(); ++i) {
                if (i != 0) o << ",";
                o << pt.col_x[i];
            }
            o << "],\"rows\":[";
            bool first_row = true;
            for (const PlacedRow& r : pt.placed) {
                if (r.page != page) continue;
                if (!first_row) o << ",";
                first_row = false;
                o << "{\"row\":" << r.row << ",\"y\":" << r.y << ",\"h\":" << r.h << "}";
            }
            o << "]}";
            return pk::checked(pk::table_grid::kSpec, o.str());
        }
    }
    return pk::checked(pk::table_grid::kSpec, "{\"ok\":false}");
}

bool EditorEngine::tableGridLineAt(float x_pt_doc, float y_pt_doc, uint64_t* table, int* col,
                                   int* row) const {
    if (table != nullptr) *table = 0;
    if (col != nullptr) *col = 0;
    if (row != nullptr) *row = -1;
    int page = 0;
    float local_y = 0.0f;
    if (!pageLocalYAt(y_pt_doc, cfg_.zoom, pagePxHeight(), pageGapPx(), &page, &local_y)) {
        return false;
    }
    for (const PlacedTable& pt : layout_.placed_tables) {
        if (x_pt_doc < pt.x - 1.0f || x_pt_doc > pt.x + pt.w + 1.0f) continue;
        bool on_page = false;
        for (const PlacedRow& pr : pt.placed) {
            if (pr.page == page) {
                on_page = true;
                break;
            }
        }
        if (!on_page) continue;
        // 列边界取最近的一条：col_x 是含首尾的绝对边界序列，1..n 都算 —— 最后一条是
        //   表的右边框，拖它改的是最后一列（表宽跟着走）。没有它，用户只能把表往里改窄，
        //   没法加宽。
        int hit_col = 0;
        float col_dist = kGridLineTolPt;
        for (size_t i = 1; i < pt.col_x.size(); ++i) {
            const float d = std::fabs(x_pt_doc - (pt.x + pt.col_x[i]));
            if (d <= col_dist) {
                col_dist = d;
                hit_col = static_cast<int>(i);
            }
        }
        // 行边界：这一行与下一行都在本页之间才算 —— 跨页那条是页边界，拖它没有语义。
        //   表尾那一行的下边是表的底框（没有下一行），照拖，改的是最后一行的高度。
        int hit_row = -1;
        float row_dist = kGridLineTolPt;
        for (size_t i = 0; i < pt.placed.size(); ++i) {
            const bool last_row = (pt.placed[i].row == pt.rows - 1);
            if (pt.placed[i].page != page) continue;
            if (!last_row && (i + 1 >= pt.placed.size() || pt.placed[i + 1].page != page)) continue;
            const float d = std::fabs(local_y - (pt.placed[i].y + pt.placed[i].h));
            if (d <= row_dist) {
                row_dist = d;
                hit_row = pt.placed[i].row;
            }
        }
        if (hit_col == 0 && hit_row < 0) continue;
        if (table != nullptr) *table = pt.handle;
        if (col != nullptr) *col = hit_col;
        if (row != nullptr) *row = hit_row;
        return true;
    }
    return false;
}

bool EditorEngine::setTableColWidths(uint64_t handle, const std::vector<float>& widths) {
    if (!loaded_) return false;
    return edit_.setTableGridCols(handle, widths);
}

bool EditorEngine::setTableColWidthsAndRender(uint64_t handle, const std::vector<float>& widths) {
    if (!setTableColWidths(handle, widths)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::setTableRowHeight(uint64_t handle, int row, float h_pt) {
    if (!loaded_) return false;
    return edit_.setTableRowHeight(handle, row, h_pt);
}

bool EditorEngine::setTableRowHeightAndRender(uint64_t handle, int row, float h_pt) {
    if (!setTableRowHeight(handle, row, h_pt)) return false;
    relayout();
    repaintAfterEdit();
    return true;
}

bool EditorEngine::canTableOp(TableOp op) const {
    if (!loaded_ || !session_ || !flow_.has_value()) return false;
    // 改结构是设计动作：表单模式与只读模式下表格是锁住的。
    if (mode_ != EditMode::Document) return false;
    const TextPos at = caret();
    const LayoutTable* table = nullptr;
    const LayoutCell* cell = layoutCellAt(at.para, &table);
    if (cell == nullptr || table == nullptr) return false;

    const rev::Doc& doc = document();
    rev::CellRef ref;
    if (!rev::cellRefOf(doc, static_cast<NodeId>(table->handle), cell->row, cell->cell, &ref)) {
        return false;
    }
    const rev::FlowIndex& flow = *flow_;

    // 干跑：造一批步骤就知道成不成，与真正执行走的是同一批构造函数。
    switch (op) {
        case TableOp::InsertRowAbove:
            return rev::insertRow(doc, flow, ref, false).ok;
        case TableOp::InsertRowBelow:
            return rev::insertRow(doc, flow, ref, true).ok;
        case TableOp::RemoveRow:
            return rev::removeRow(doc, flow, ref).ok;
        case TableOp::InsertColumnLeft:
            return rev::insertColumn(doc, flow, ref, false).ok;
        case TableOp::InsertColumnRight:
            return rev::insertColumn(doc, flow, ref, true).ok;
        case TableOp::RemoveColumn:
            return rev::removeColumn(doc, flow, ref).ok;
        case TableOp::MergeCells: {
            // 合并的主语是整格选区，不是光标所在那一格。
            std::vector<rev::CellRef> cells;
            for (const CaretResolver::CellBlocks& cb : caret_.cellBlocks()) {
                if (cb.empty() || cb.first < 0 ||
                    cb.first >= static_cast<int>(blocks_full_.size())) {
                    return false;
                }
                const std::vector<CellSlot>& path =
                    blocks_full_[static_cast<size_t>(cb.first)].cell_path;
                if (path.empty()) return false;
                const CellSlot& slot = path.back();
                rev::CellRef one;
                if (!rev::cellRefOf(doc, slot.table, slot.row, slot.cell, &one)) return false;
                cells.push_back(one);
            }
            if (cells.size() < 2) return false;
            return rev::mergeCells(doc, flow, cells).ok;
        }
        case TableOp::SplitCell:
            return rev::splitCell(doc, flow, ref).ok;
        case TableOp::RemoveTable: {
            // 删整张表只要求光标在表里；"文档不能只剩零个块"这条由命令层再挡一次。
            FlowPos lo = 0;
            FlowPos hi = 0;
            if (!flow.intervalOf(static_cast<NodeId>(table->handle), &lo, &hi) || hi <= lo) {
                return false;
            }
            return rev::canEditRange(doc, mode_, Range{lo, hi}).allowed;
        }
    }
    return false;
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
             << ",\"vAlign\":0,\"selCells\":0,\"selRows\":0,\"selCols\":0"
             << ",\"canInsertRow\":0,\"canRemoveRow\":0,\"canInsertColumn\":0"
             << ",\"canRemoveColumn\":0,\"canMerge\":0,\"canSplit\":0,\"canRemoveTable\":0}";
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
    // 六条结构命令的可用性：宿主据此置灰，不自己重算前置条件。
    o << ",\"canInsertRow\":" << (canTableOp(TableOp::InsertRowAbove) ? 1 : 0);
    o << ",\"canRemoveRow\":" << (canTableOp(TableOp::RemoveRow) ? 1 : 0);
    o << ",\"canInsertColumn\":" << (canTableOp(TableOp::InsertColumnRight) ? 1 : 0);
    o << ",\"canRemoveColumn\":" << (canTableOp(TableOp::RemoveColumn) ? 1 : 0);
    o << ",\"canMerge\":" << (canTableOp(TableOp::MergeCells) ? 1 : 0);
    o << ",\"canSplit\":" << (canTableOp(TableOp::SplitCell) ? 1 : 0);
    o << ",\"canRemoveTable\":" << (canTableOp(TableOp::RemoveTable) ? 1 : 0);
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
