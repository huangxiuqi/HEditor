/**
 * 旧模型树上的域求值；doc 树层删除后本文件一起删除。
 */
#pragma once

#include "core/doc/node.h"
#include "core/rev/field_spec.h"

namespace heditor {

/**
 * 遍历整棵树求域值，页眉页脚也走到。
 * @param tree 模型树
 * @param ctx 求值上下文
 * @returns 检查与更新计数
 */
FieldEvalResult evaluateFields(NodeTree& tree, const FieldContext& ctx);

}  // namespace heditor
