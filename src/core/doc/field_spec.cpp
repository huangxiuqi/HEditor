#include "core/doc/field_spec.h"

#include <string>
#include <unordered_set>

#include "core/doc/node.h"

namespace heditor {

namespace {

/**
 * 递归求值一棵子树。visited 防的是畸形文档里的环（同一个节点被两次挂进可达范围）——
 * 没有它，求值会在环上转不出来，而求值是被宿主在打开文档时调用的，不能挂住。
 */
void evalSubtree(NodeTree& tree, NodeId id, const FieldContext& ctx,
                 std::unordered_set<NodeId>* visited, FieldEvalResult* out) {
    if (id == kInvalidNodeId || !visited->insert(id).second) return;
    Node* n = tree.find(id);
    if (n == nullptr) return;

    if (n->kind == NodeKind::Field) {
        ++out->examined;
        std::string next = fieldValue(n->field, ctx);
        // 只在真的变化时写并计数：这就是幂等的落点 —— 第二次求值算出的值与缓存相同，
        // updated 保持 0，文档一个字节都不动。
        if (next != n->field.cache) {
            n->field.cache = std::move(next);
            ++out->updated;
        }
        return;
    }
    for (Node* c : n->children) evalSubtree(tree, c->id, ctx, visited, out);
}

}  // namespace

const char* fieldKindName(FieldKind kind) {
    switch (kind) {
        case FieldKind::PageNumber:
            return "pageNumber";
        case FieldKind::PageCount:
            return "pageCount";
        case FieldKind::PrintDate:
            return "printDate";
        case FieldKind::DocProperty:
            return "docProperty";
    }
    return "（未知）";
}

bool fieldKindFromName(const std::string& name, FieldKind* out) {
    if (out == nullptr) return false;
    const FieldKind all[] = {FieldKind::PageNumber, FieldKind::PageCount, FieldKind::PrintDate,
                             FieldKind::DocProperty};
    for (FieldKind k : all) {
        if (name == fieldKindName(k)) {
            *out = k;
            return true;
        }
    }
    return false;
}

bool isLayoutResult(FieldKind kind) {
    return kind == FieldKind::PageNumber || kind == FieldKind::PageCount;
}

std::string fieldValue(const FieldSpec& field, const FieldContext& ctx) {
    switch (field.kind) {
        case FieldKind::PageNumber:
            return std::to_string(ctx.page_number);
        case FieldKind::PageCount:
            return std::to_string(ctx.page_count);
        case FieldKind::PrintDate:
            return ctx.print_date;
        case FieldKind::DocProperty: {
            const auto it = ctx.doc_properties.find(field.name);
            return it == ctx.doc_properties.end() ? std::string() : it->second;
        }
    }
    return std::string();
}

FieldEvalResult evaluateFields(NodeTree& tree, const FieldContext& ctx) {
    FieldEvalResult result;
    std::unordered_set<NodeId> visited;
    evalSubtree(tree, tree.rootId(), ctx, &visited, &result);

    // 页眉页脚片段由节的记录引用，不在正文流里，所以正文那一遍走不到它们。
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
