/**
 * 模型树 → 修订：迁移路径与等价判据的桥。
 *
 * 它是这一层唯一一处以"模型层"为输入的文件，所以依赖方向写在这里而不是 rev.h 里：
 * rev 的其余部分不认识 NodeTree，将来把模型层换掉时只需要删掉这一个文件。
 */
#include <functional>
#include <string>
#include <utility>

#include "core/doc/node.h"
#include "core/rev/rev.h"

namespace heditor::rev {

namespace {

/**
 * 模型层的 Node 与本层的 Node 同名（同名不同物：一个是可变树的节点，一个是不可变节点）。
 * 这个文件同时看见两边，所以给模型那一边起个别名 —— 写成裸 Node 的话，编不过才知道撞了，
 * 而"看见的是哪一个"在别处会静默。
 */
using ModelNode = heditor::Node;

/**
 * 种类 → 载荷。没有载荷的种类给 monostate。
 *
 * 这里不写 default 分支的"按种类猜"：种类与载荷的对应是一条查表，两张表（这张与
 * 落盘那张）不一致时，等价判据会在第一次比较时红。
 */
Payload payloadOf(const ModelNode& n) {
    switch (n.kind) {
        case NodeKind::TextRun:
            return RunPayload{n.text, n.marks, n.styleRef};
        case NodeKind::ParagraphMark:
            return MarkPayload{n.para, n.styleRef};
        case NodeKind::ContentControl:
            return n.control;
        case NodeKind::Table:
            return n.table;
        case NodeKind::TableRow:
            return n.row;
        case NodeKind::TableCell:
            return n.cell;
        case NodeKind::Image:
            return n.image;
        case NodeKind::Formula:
            return n.formula;
        case NodeKind::Field:
            return n.field;
        default:
            break;
    }
    return std::monostate{};
}

}  // namespace

bool fromTree(const NodeTree& tree, Doc* out, std::string* error) {
    const ModelNode* root = tree.find(tree.rootId());
    if (root == nullptr) {
        *error = "模型树没有根";
        return false;
    }
    if (root->kind != NodeKind::Document) {
        *error = std::string("模型的根是") + nodeKindName(root->kind) + "，不是文档";
        return false;
    }
    // 多节要连节的记录（纸张、页边距、分节分页）一起搬，本切片的载荷里还没有它。
    // 只搬第一节会得到一份"后面几节凭空消失"的修订，而它看起来是完整的。
    if (tree.sectionCount() > 1) {
        *error = "多节文档尚未进修订载荷（投影只认第一节）";
        return false;
    }

    std::function<Ref(const ModelNode&)> copy = [&copy](const ModelNode& n) -> Ref {
        std::vector<Ref> kids;
        kids.reserve(n.children.size());
        for (const ModelNode* c : n.children) kids.push_back(copy(*c));
        return makeNode(n.id, n.kind, payloadOf(n), std::move(kids), n.hidden);
    };

    *out = adopt(copy(*root), tree.maxAllocatedId() + 1);
    return true;
}

}  // namespace heditor::rev
