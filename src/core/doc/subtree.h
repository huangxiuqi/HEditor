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

#include "core/doc/node.h"
#include "core/rev/char_format.h"
#include "core/rev/control_spec.h"
#include "core/rev/formula_spec.h"
#include "core/rev/image_spec.h"
#include "core/rev/para_format.h"
#include "core/rev/subtree.h"
#include "core/rev/table_spec.h"

namespace heditor {

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
