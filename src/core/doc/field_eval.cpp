#include "core/doc/field_eval.h"

#include <string>
#include <unordered_set>

namespace heditor {

namespace {

/**
 * 递归求值一棵子树；visited 防畸形文档里的环。
 * @param tree 模型树
 * @param id 子树根
 * @param ctx 求值上下文
 * @param visited 已访问集合
 * @param out 计数
 */
void evalSubtree(NodeTree& tree, NodeId id, const FieldContext& ctx,
                 std::unordered_set<NodeId>* visited, FieldEvalResult* out) {
    if (id == kInvalidNodeId || !visited->insert(id).second) return;
    Node* n = tree.find(id);
    if (n == nullptr) return;

    if (n->kind == NodeKind::Field) {
        ++out->examined;
        std::string next = fieldValue(n->field, ctx);
        if (next != n->field.cache) {
            n->field.cache = std::move(next);
            ++out->updated;
        }
        return;
    }
    for (Node* c : n->children) evalSubtree(tree, c->id, ctx, visited, out);
}

}  // namespace

FieldEvalResult evaluateFields(NodeTree& tree, const FieldContext& ctx) {
    FieldEvalResult result;
    std::unordered_set<NodeId> visited;
    evalSubtree(tree, tree.rootId(), ctx, &visited, &result);

    const int sections = tree.sectionCount();
    for (int i = 0; i < sections; ++i) {
        const Node* sec = tree.find(tree.sectionAt(i));
        if (sec == nullptr) continue;
        for (int v = 0; v < kHeaderFooterVariantCount; ++v) {
            evalSubtree(tree, sec->section.headers[v].root, ctx, &visited, &result);
            evalSubtree(tree, sec->section.footers[v].root, ctx, &visited, &result);
        }
    }
    return result;
}

}  // namespace heditor
