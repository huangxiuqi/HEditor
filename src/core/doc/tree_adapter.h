/**
 * 旧模型树与修订之间的迁移适配器。
 *
 * 只服务于测试、演示与审计；产品编辑路径不经过它。doc 树层删除后本文件一起删除。
 */
#pragma once

#include <string>

#include "core/doc/node.h"
#include "core/rev/rev.h"

namespace heditor::rev {

/**
 * 修订 → 模型树。
 * @param doc 修订
 * @param out 写出的模型树（先清空）
 */
void toTree(const Doc& doc, NodeTree* out);

/**
 * 模型树 → 修订。
 * @param tree 模型树
 * @param out 写出的修订（成功时才动）
 * @param error 写回失败原因
 * @returns true = 表示得了
 */
bool fromTree(const NodeTree& tree, Doc* out, std::string* error);

}  // namespace heditor::rev
