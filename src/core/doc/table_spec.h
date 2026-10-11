/**
 * 表格的记录：单元格跨度、可选的列定义、可选的显式列宽（见 docs/文档模型设计.md 第九节）。
 *
 * 三种做法的差别决定这份结构长什么样。OOXML 在模型里声明一个网格（tblGrid），纵向合并的
 * 续格也真实存在；HTML 把 rowspan / colspan 写在单元格上，被合并掉的格位在模型里根本
 * 不存在。本设计选 HTML 式：跨度是唯一真相，网格是派生的。
 *
 * 代价与收益都在同一处：续格不存在，"续格不能有独立内容"这条约束由结构本身保证，
 * 不需要另写一条不变量；换来的是行列操作必须先算清楚哪些格子被跨过。
 *
 * 与 para / marks / control / styleRef 一样挂在节点上，按种类取用：跨度只对 TableCell 有意义，
 * 列定义与列宽只对 Table 有意义。放在节点上而不是另建一张表，理由同 hidden —— 网格推导要按
 * 树顺序取它。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/control_spec.h"
#include "core/doc/length.h"

namespace heditor {

/**
 * 单元格跨度。被合并掉的格位在模型里不存在，所以跨度就是"这一格覆盖多少格位"的全部信息。
 *
 * 两个都至少是 1（0 或负数不是可表达的跨度，步骤层会拒绝）。
 */
struct CellSpan {
    int row_span = 1;
    int col_span = 1;

    bool operator==(const CellSpan& o) const = default;
};

/**
 * 一行（或一份）列定义。只有"列上要绑字段"的表格才需要它（第九节"列定义是可选的"）。
 *
 * fieldRef 是不透明字符串：编辑器负责把它标在列上、按它落值、按它取值，但不解释它的
 * 含义。"它指向业务系统的哪个数据元素"是宿主自己的映射表 —— 换一套业务系统不影响文档。
 */
struct TableColumn {
    std::string field_ref;
    std::string title;

    /**
     * 字符 / 数字 / 日期这类数据类型。影响编辑与校验，不改存储形态（与控件记录的
     * controlType 同一个分工）。
     */
    std::string data_type;

    /**
     * 输入框 / 下拉 / 日期选择这类编辑器提示，同样是给宿主的字符串。
     */
    std::string editor_style;

    Length width;
    Align align = Align::Start;

    bool operator==(const TableColumn& o) const = default;
};

/**
 * 表格自己的记录。默认为空 —— 普通表格不需要任何声明，列由跨度派生（第九节）。
 *
 * header_rows 是"前几行是表头行"。它必须显式写出来而不能靠约定"第一行就是表头"：有了列定义
 * 之后，数据行不允许合并而表头行不受限，这时"哪几行是数据行"就成了会影响合法性的判据。
 */
struct TableSpec {
    /**
     * 列定义。空表示普通表格：列由跨度派生，合并照常允许。
     * 非空表示这张表可以按数据落值，同时带来一条约束 —— 数据行每行的格数必须等于列数。
     */
    std::vector<TableColumn> columns;

    /**
     * 显式列宽（OOXML 的 tblGrid 等价物）。可选：列宽也可以由单元格的内容与跨度派生，
     * 只有作者指定了才存这里。长度不做求值，所以它现在只是一份作者意图。
     */
    std::vector<Length> grid_cols;

    /**
     * 表头行的行数（从第 0 行起算）。0 表示没有表头行，于是每一行都是数据行。
     */
    int header_rows = 0;

    /**
     * 这张表是否带列定义。带列定义就有"数据行不许合并"这条约束。
     */
    bool hasColumns() const { return !columns.empty(); }
};

}  // namespace heditor
