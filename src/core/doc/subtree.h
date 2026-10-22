/**
 * 可移植子树：一棵子树的内容与结构的一份值拷贝（见 docs/文档模型设计.md 第五节）。
 *
 * 与 Fragment 分工不同：Fragment 装的是"内容"（文本与段落标记，扁平），NodeSpec 装的是"结构"
 * （控件、图片、表格这类整块元素，递归）。节点级步骤（InsertNode / RemoveNode）用它表达
 * "插进去的是什么 / 删掉的是什么"，于是步骤是值语义的、可以留给序列化（1.10）。
 *
 * 为什么需要它而不是直接传节点 id：id 只在当前文档里有意义，步骤要能说明"插的是什么东西"。
 * 反过来，需要固定身份的场合（撤销要把同一棵子树搬回原处）走另一条路 —— 物化成一棵摘下的子树，
 * 靠 NodeTree::attachChild 挂回，那条路上 id 不变，见 node.h。
 *
 * 不带 id：拷贝出来的是内容，挂回去时是新身份。这与 Fragment 不记节点 ID 同一个理由。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/char_format.h"
#include "core/doc/control_spec.h"
#include "core/doc/node.h"
#include "core/doc/para_format.h"
#include "core/doc/table_spec.h"

namespace heditor {
/**
 * 一棵子树的描述。字段与 Node 一一对应，只有两处例外：不带 id 与 parent，也不带节记录
 * （SectionSpec 里的页眉页脚引用是本文件内的节点 id，搬进一份可移植的值里没有意义；
 * 整棵节的搬运要等序列化那一步把片段解析成内容）。
 */
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
     * 字符格式（仅文本节点）。
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
 * 把树里的一棵子树拷成 NodeSpec（递归，含全部格式字段）。
 * @param tree 文档树
 * @param id 子树根
 * @return 可移植的描述（不带 id 与 parent）
 */
NodeSpec captureSubtree(const NodeTree& tree, NodeId id);

/**
 * 把 NodeSpec 建成真实节点。parent 有效时挂在它的第 at 个槽位；parent 为 kInvalidNodeId 时
 * 建成一棵摘下的子树（不挂在树上），返回新子树的根 id。建不出来时返回 kInvalidNodeId。
 *
 * 递归过程不校验子节点约束：调用方先问 acceptsChild，或让不变量检查器去报。
 * @param tree 文档树
 * @param parent 挂到哪个节点下
 * @param at 槽位
 * @param spec 描述
 * @return 新子树的根 id；失败为 kInvalidNodeId
 */
NodeId materializeSubtree(NodeTree& tree, NodeId parent, int at, const NodeSpec& spec);

}  // namespace heditor
