/**
 * 可移植子树描述（值类型）：NodeSpec 只描述结构与格式，不依赖文档树。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/char_format.h"
#include "core/doc/control_spec.h"
#include "core/doc/field_spec.h"
#include "core/doc/formula_spec.h"
#include "core/doc/image_spec.h"
#include "core/doc/node_kind.h"
#include "core/doc/para_format.h"
#include "core/doc/table_spec.h"

namespace heditor {
struct NodeSpec {
    /**
     * 节点种类。
     */
    NodeKind kind = NodeKind::TextRun;

    /**
     * 作者设定的默认可见性。
     */
    bool hidden = false;

    /**
     * 码点（仅文本节点）。
     */
    std::vector<uint32_t> text;

    /**
     * 段落格式覆盖（仅段落标记）。
     */
    ParaFormatPatch para;

    /**
     * 样式引用（段落标记与文本节点）。
     */
    std::string styleRef;

    /**
     * 字符格式（文本节点与段落标记：后者是空段落的字符格式）。
     */
    CharFormat marks;

    /**
     * 控件记录（仅内容控件）。
     */
    ControlSpec control;

    /**
     * 单元格记录（仅单元格）。
     */
    CellSpec cell;

    /**
     * 行记录（仅表格行）。
     */
    RowSpec row;

    /**
     * 表格记录（仅表格）。
     */
    TableSpec table;

    /**
     * 域记录（仅域）。
     */
    FieldSpec field;

    /**
     * 图片记录（仅图片）。没有它，摘下一棵含图片的子树再挂回去就只剩一个空图 ——
     * 撤销一次删除图片，恢复出来的是一张没有资源标识的图。
     */
    ImageSpec image;

    /**
     * 公式记录（仅公式）。
     */
    FormulaSpec formula;

    /**
     * 子节点，顺序就是流顺序。
     */
    std::vector<NodeSpec> children;

    /**
     * 这棵子树占几个流位置。口径与 FlowIndex 一致：文本按码点数，原子与段落标记各占一位，
     * 容器占"起始边界 + 全部孩子 + 结束边界"。
     *
     * 它让位置映射不必再读文档 —— 步骤只要带着子树就能算出自己移动了多少位置，
     * 这正是 mapPos 只吃步骤、不吃树的前提。
     * @return 占几个流位置（与 FlowIndex 同一口径）
     */
    int span() const;
};
/**
 * 新建表格的行列上限。
 */
inline constexpr int kMaxNewTableRows = 200;
inline constexpr int kMaxNewTableCols = 50;

/**
 * 纯文本是否含换行。
 *
 * 段文本里出现 U+000A 会让段号有两套算法（按块取与按分隔符切），所以含换行的输入
 * 必须先折成节点再落笔。判据与 nodesFromPlainText 的折行条件同一份。
 * @param utf8 待检文本
 * @returns 含 CR 或 LF 时为真
 */
bool hasLineBreak(const std::string& utf8);

/**
 * 纯文本 → 节点序列：换行折成段落标记，CRLF 算一个换行。
 *
 * 空行是有效内容（连续两个换行产出两个段落标记），所以文本 run 只在行非空时产出，
 * 段落标记无条件产出。粘贴纯文本与插入含换行的字符串共用这一份 —— 两条路对同一段
 * 文本必须产出同一个节点序列，否则同一份粘贴从两个入口进来会得到不同的段结构。
 * @param utf8 纯文本（UTF-8）
 * @returns 节点序列，原文没有任何内容时为空
 */
std::vector<NodeSpec> nodesFromPlainText(const std::string& utf8);

/**
 * 造一张空表：每格一枚段落标记。
 * @param rows 行数
 * @param cols 列数
 * @param header 第一行是否标题行
 * @param out 写回子树
 * @returns true = 行列合法
 */
bool makeEmptyTableSpec(int rows, int cols, bool header, NodeSpec* out);
}  // namespace heditor