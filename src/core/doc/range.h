/**
 * 树操作使用的区间辅助：区间值已移到值层，这里只留区间规范化。
 */
#pragma once

#include "core/doc/node.h"
#include "core/rev/range.h"

namespace heditor {

/**
 * 端点规范化：把落在不可半分单位内部的端点向外扩到那个单位的边界。
 * @param tree 文档树
 * @param range 区间
 * @returns 端点外扩后的区间
 */
Range normalizeRange(const NodeTree& tree, Range range);

}  // namespace heditor
