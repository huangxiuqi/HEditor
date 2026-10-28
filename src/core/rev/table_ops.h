/**
 * 表格的结构操作（修订层）：插删行列、合并、拆分。
 *
 * 与 core/doc/table.h 那组构造器是同一套算法、两份数据源：那边按可变 NodeTree 就地改，
 * 这边按不可变修订产出步骤。不能复用同一份实现 —— 修订层没有"就地改名"这回事，改一格
 * 只能整份换一条路径，而步骤正是"换路径"的说明书。
 *
 * 一条共同的口径：结果永远是一批步骤（一个动作 = 一个事务 = 一步撤销）。跨行合并的格子被
 * 加宽、被吸收的格子被删掉，都是同一批里的几条最小步骤，不是一种新的步骤类型。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/rev/flow.h"
#include "core/rev/rev.h"
#include "core/rev/step_map.h"
#include "core/rev/step_types.h"
#include "core/rev/table_grid.h"

namespace heditor::rev {
/**
 * 一格的节点身份与格坐标。行列号是"过滤掉非 TableRow / TableCell 之后的下标"，
 * 与 CellSlot 同一口径 —— 两个数对不上时，命令层与排版层看到的就是两张表。
 */
struct CellRef {
    /**
     * 表格节点号。
     */
    NodeId table = kInvalidNodeId;

    /**
     * 行节点号。
     */
    NodeId row = kInvalidNodeId;

    /**
     * 单元格节点号。
     */
    NodeId cell = kInvalidNodeId;

    /**
     * 行号（从 0 起）。
     */
    int row_index = -1;

    /**
     * 格号（在这一行里的下标，从 0 起）。
     */
    int cell_index = -1;
};

/**
 * 表格步骤的构造结果：成与不成都要说清，失败时不返回一批"改了一半"的步骤。
 */
struct TableSteps {
    /**
     * 这一批步骤（按顺序应用）。
     */
    std::vector<Step> steps;

    /**
     * 构造成功没有。
     */
    bool ok = false;

    /**
     * 失败原因（越界、跨行合并、区域不是矩形、要并的格子有内容）。
     */
    std::string error;
};

/**
 * 推一张表的派生网格。
 * @param doc 修订
 * @param table 表格节点号
 * @param out 写回网格
 * @param error 写回失败原因
 * @return true = 推得出网格
 */
bool tableGrid(const Doc& doc, NodeId table, TableGrid* out, std::string* error);

/**
 * 按格坐标取一格的节点身份。
 * @param doc 修订
 * @param table 表格节点号
 * @param row 行号
 * @param cell 格号
 * @param out 写回这一格
 * @return true = 坐标可达
 */
bool cellRefOf(const Doc& doc, NodeId table, int row, int cell, CellRef* out);

/**
 * 按 CellSlot 的表格身份取一格。CellSlot 的 table 是不透明的 64 位身份（编译层填节点号），
 * 这里的用途只有一处：把"光标在哪一格"还原成节点号。
 * @param doc 修订
 * @param table_handle 表格身份（NodeId 的数值）
 * @param row 行号
 * @param cell 格号
 * @param out 写回这一格
 * @return true = 这张表在这一格里
 */
bool cellRefAt(const Doc& doc, uint64_t table_handle, int row, int cell, CellRef* out);

/**
 * 一格在网格里的原点坐标（左上角）。
 * @param doc 修订
 * @param table 表格节点号
 * @param cell 单元格节点号
 * @param row 写回原点行
 * @param col 写回原点列
 * @return true = 这一格在网格里
 */
bool cellOrigin(const Doc& doc, NodeId table, NodeId cell, int* row, int* col);

/**
 * 在参照行的上方或下方插一行。新行照参照行的横向跨度序列造：参照行有一格跨三列，
 * 新行也是那一格跨三列（Word 同一条）—— 新行因此自带网格宽度，不必再问列数。
 * @param doc 修订
 * @param flow 流索引（算插入点的流坐标）
 * @param at 参照行
 * @param below true = 插在下面
 * @return 一批步骤
 */
TableSteps insertRow(const Doc& doc, const FlowIndex& flow, const CellRef& at, bool below);

/**
 * 删一行。行里有纵向合并的格子时拒绝：那些格子的内容会随行消失，而它们覆盖的下面几行
 * 会凭空少一格 —— 要那样做请先拆分合并。
 * @param doc 修订
 * @param flow 流索引
 * @param at 要删的那一行
 * @return 一批步骤
 */
TableSteps removeRow(const Doc& doc, const FlowIndex& flow, const CellRef& at);

/**
 * 在参照格的左边或右边插一列。跨过插入线的格子加宽一格（它就是吸收这一列的那一格），
 * 其余行各插一个新格子。
 * @param doc 修订
 * @param flow 流索引
 * @param at 参照格
 * @param right true = 插在右边
 * @return 一批步骤
 */
TableSteps insertColumn(const Doc& doc, const FlowIndex& flow, const CellRef& at, bool right);

/**
 * 删一列。覆盖这一列的格子变窄一格；窄到 0 的连同它的内容一起删掉。删最后一列拒绝。
 * @param doc 修订
 * @param flow 流索引
 * @param at 参照格
 * @return 一批步骤
 */
TableSteps removeColumn(const Doc& doc, const FlowIndex& flow, const CellRef& at);

/**
 * 把一组格子合并成一格。
 *
 * 要求两点，都不放宽：这些格子必须正好铺满一个矩形（不做"缺角自动补"），除锚点以外
 * 必须都是空的（只有一枚空段落标记）。第二条是数据的边界 —— 合并会把那些格子删掉，
 * 有内容的格子删掉就是丢数据；宁可拒绝，也不替用户决定"先清空还是先搬走"。
 * @param doc 修订
 * @param flow 流索引
 * @param cells 参与合并的格子（至少两格）
 * @return 一批步骤
 */
TableSteps mergeCells(const Doc& doc, const FlowIndex& flow, const std::vector<CellRef>& cells);

/**
 * 把一格拆回它覆盖的每一个格位。拆出来的新格各有自己的一枚空段落标记，锚点保留原有内容
 * 与垂直对齐。
 * @param doc 修订
 * @param flow 流索引
 * @param cell 要拆的格子
 * @return 一批步骤
 */
TableSteps splitCell(const Doc& doc, const FlowIndex& flow, const CellRef& cell);

}  // namespace heditor::rev
