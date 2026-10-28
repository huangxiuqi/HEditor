/**
 * 测试与演示用的树装载适配器：把 NodeTree 投影成 rev::Doc 后走修订入口。
 *
 * 产品引擎只接受修订；测试仍用树构造文档，这一层把转换收在一处。
 */
#pragma once

#include <string>
#include <utility>

#include "core/engine/engine.h"
#include "core/rev/rev.h"

namespace heditor {

/**
 * 把模型树投影成修订并装载。
 * @param engine 引擎
 * @param tree 模型树
 * @param sheet 样式表
 * @returns true = 投影与装载成功
 */
inline bool loadTreeDocument(EditorEngine& engine, const NodeTree& tree,
                             const StyleSheet& sheet = {}) {
    rev::Doc doc;
    std::string error;
    if (!rev::fromTree(tree, &doc, &error)) return false;
    engine.setDocument(std::move(doc), sheet);
    return true;
}

}  // namespace heditor
