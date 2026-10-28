/**
 * 可移植子树描述（值类型）：NodeSpec 只描述结构与格式，不依赖文档树。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/rev/char_format.h"
#include "core/rev/control_spec.h"
#include "core/rev/field_spec.h"
#include "core/rev/formula_spec.h"
#include "core/rev/image_spec.h"
#include "core/rev/node_kind.h"
#include "core/rev/para_format.h"
#include "core/rev/table_spec.h"

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
}  // namespace heditor