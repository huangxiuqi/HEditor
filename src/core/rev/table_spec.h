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
#include <optional>
#include <string>
#include <vector>

#include "core/rev/control_spec.h"
#include "core/rev/length.h"

namespace heditor {
/**
 * 单元格内文字的垂直对齐（Word 的 w:vAlign，挂在 w:tcPr 上而不是样式里）。
 *
 * 刻意另立一个枚举，不借节的 VerticalAlign：那个有 Both（两端对齐）这一档，而单元格没有
 * 这个取值。借一个值域更宽的枚举等于让非法值有地方可待。
 */
enum class CellVAlign : uint8_t {
    Top,
    Center,
    Bottom,
};

/**
 * 行高的规则（Word 的 trHeight）。参照项目用一个符号编码三档（0 自动 / 正数不小于 /
 * 负数固定），这里拆成"值 + 规则"两个字段：比符号可读，也不必约定负数表示固定。
 */
enum class HeightRule : uint8_t {
    Auto,     // 自动：按内容
    AtLeast,  // 不小于：内容多了会长高
    Exact,    // 固定：超出裁切
};

/**
 * 单元格的记录。跨度是"这一格覆盖多少格位"的全部信息 —— 被合并掉的格位在模型里不存在。
 *
 * 两个跨度都至少是 1（0 或负数不是可表达的跨度，步骤层会拒绝）。
 */
struct CellSpec {
    /**
     * 纵向跨度（至少 1）。
     */
    int row_span = 1;

    /**
     * 横向跨度（至少 1）。
     */
    int col_span = 1;

    /**
     * 格内垂直对齐。默认顶端，与 Word 的默认一致。
     */
    CellVAlign v_align = CellVAlign::Top;

    bool operator==(const CellSpec& o) const = default;
};

/**
 * 一行自己的记录，挂在 TableRow 上（与 CellSpec 对 TableCell 的关系同构）。
 *
 * 三个开关都是逐行属性 —— Word 里也都在 w:trPr 上：允许跨页断行（w:cantSplit 取反）、
 * 标题行重复（w:tblHeader）、行前分页（w:pageBreakBefore）。行高（w:trHeight）同样在这里，
 * 病历里那种"一大格用来写字"的表全靠它撑开。
 */
struct RowSpec {
    /**
     * 允许被分页线切开。默认允许（Word 的 cantSplit 默认是假）。
     */
    bool can_split_by_page = true;

    /**
     * 标题行：表格跨页时在每页顶端重复这一行。默认不是。
     *
     * 它必须满足"从第 0 行起的连续前缀"这条不变量 —— 重复哪几行只有连续前缀才有意义。
     */
    bool is_header = false;

    /**
     * 行前强制分页，效果同分页符。默认不是。
     */
    bool page_break_before = false;

    /**
     * 行高。height_rule 为 Auto 时无意义（Length 的 Auto 是同一个意思，两处取一处即可，
     * 判据要求它们不打架）。
     */
    Length height;

    /**
     * 行高规则：Auto 按内容、AtLeast 下限、Exact 固定。
     */
    HeightRule height_rule = HeightRule::Auto;

    bool operator==(const RowSpec& o) const = default;
};

/**
 * 一行（或一份）列定义。只有"列上要绑字段"的表格才需要它（第九节"列定义是可选的"）。
 *
 * fieldRef 是不透明字符串：编辑器负责把它标在列上、按它落值、按它取值，但不解释它的
 * 含义。"它指向业务系统的哪个数据元素"是宿主自己的映射表 —— 换一套业务系统不影响文档。
 */
struct TableColumn {
    /**
     * 列上绑的业务字段（不透明字符串，由宿主解释）。
     */
    std::string field_ref;

    /**
     * 列标题。
     */
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

    /**
     * 列宽。
     */
    Length width;

    /**
     * 这一列内容的对齐。
     */
    Align align = Align::Start;

    bool operator==(const TableColumn& o) const = default;
};

/**
 * 表格自己的记录。默认为空 —— 普通表格不需要任何声明，列由跨度派生（第九节）。
 *
 * 表头不再是一个"前几行"的数字：它改成逐行开关（RowSpec::is_header），于是"重复哪几行"
 * 由不变量约束成从第 0 行起的连续前缀，"几个标题行"降为派生量（headerRowCount）。
 * 这样"数据行不许合并"那条约束可以直接说"非标题行"，不再依赖"表头一定在前面"这个前提。
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
     * 表格宽度。Auto 表示撑满可用宽度（Word 默认）；Percent 相对内容区；Abs 是固定值。
     */
    Length width;

    /**
     * 表格在内容区里的水平位置。只在宽度小于可用宽度时才有意义。
     */
    Align align = Align::Start;

    /**
     * 这张表是否带列定义。带列定义就有"数据行不许合并"这条约束。
     * @return true = 这张表带列定义
     */
    bool hasColumns() const { return !columns.empty(); }

    /**
     * 逐字段相等。序列化要拿它判断"这张表是不是全默认"，从而决定写不写这个条目。
     */
    bool operator==(const TableSpec& o) const = default;
};

/**
 * 改单元格记录时"要改哪几项"。
 *
 * 只收垂直对齐：跨度由行列操作与合并拆分唯一改写 —— 那是结构动作，走的是同一批步骤，
 * 给跨度再开一条属性入口，等于让"格覆盖多少格位"有第二个可写真相。
 */
struct CellPatch {
    /**
     * 格内垂直对齐。
     */
    std::optional<CellVAlign> v_align;
};

/**
 * 改一行记录时"要改哪几项"。空表示这一项不动（与"改成假"是两件事）。
 *
 * 行高用磅值给：调用方量的是画面，模型存的是长度；<= 0 表示 Auto（按内容）。
 */
struct RowPatch {
    /**
     * 标题行（跨页时在每页顶端重复）。
     */
    std::optional<bool> is_header;

    /**
     * 允许被分页线切开。
     */
    std::optional<bool> can_split_by_page;

    /**
     * 行前强制分页。
     */
    std::optional<bool> page_break_before;

    /**
     * 行高（磅）。<= 0 表示按内容。
     */
    std::optional<float> height_pt;

    /**
     * 行高规则。
     */
    std::optional<HeightRule> height_rule;
};

/**
 * 改一张表记录时"要改哪几项"。
 *
 * 列定义与列宽不在这里：列定义是"按数据落值"的形状（改它等于换一张表的用法），
 * 列宽要等排版按列分配落地之后才有可写的入口。
 */
struct TablePatch {
    /**
     * 表格宽度（磅）。<= 0 表示撑满可用宽度。
     */
    std::optional<float> width_pt;

    /**
     * 表格在内容区里的水平位置。
     */
    std::optional<Align> align;
};

/**
 * 行高规则与单元格垂直对齐的文本形式。序列化与宿主共用同一份名字，
 * 两边各写一遍必然分叉（一个写 "atLeast"、另一个写 "at_least"）。
 * @param rule 行高规则
 * @return 名字
 */
const char* heightRuleName(HeightRule rule);

/**
 * 名字转行高规则。
 * @param name 名字
 * @param out 写回规则
 * @return true = 名字认识
 */
bool heightRuleFromName(const std::string& name, HeightRule* out);

/**
 * 格内垂直对齐的名字。
 * @param align 对齐
 * @return 名字
 */
const char* cellVAlignName(CellVAlign align);

/**
 * 名字转格内垂直对齐。
 * @param name 名字
 * @param out 写回对齐
 * @return true = 名字认识
 */
bool cellVAlignFromName(const std::string& name, CellVAlign* out);

}  // namespace heditor
