/**
 * 模型树 → 修订：迁移路径与等价判据的桥。
 *
 * 它是这一层唯一一处与"模型层"互转的文件，所以依赖方向写在这里而不是 rev.h 之外：
 * rev 的其余部分不认识 NodeTree，将来把模型层换掉时只需要删掉这一个文件。
 *
 * 两个方向都只用于边界：装载（模型 → 修订）与审计（修订 → 模型，给独立参照物）。
 * 编辑路径一个方向都不走 —— 走了就又成了"每条编辑造一棵树"。
 */
#include <functional>
#include <string>
#include <utility>

#include "core/doc/node.h"
#include "core/doc/tree_adapter.h"

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
            return MarkPayload{n.para, n.styleRef, n.marks};
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
        case NodeKind::Section:
            return n.section;
        default:
            break;
    }
    return std::monostate{};
}

}  // namespace

void toTree(const Doc& doc, NodeTree* out) {
    if (out == nullptr || !doc.root) return;
    // 模型树构造时自带 Document → Section 骨架。先清干净，再按修订的形状重建：
    //   留着骨架会让"修订里没有节"的文档在参照侧多出一段。
    while (out->find(out->rootId())->children.size() > 0) out->eraseChild(out->rootId(), 0);

    std::function<void(const Node&, NodeId)> build = [&](const Node& n, NodeId parent) {
        NodeId id = parent;
        if (&n != doc.root.get()) {
            id = out->appendChild(parent, n.kind);
            if (id == kInvalidNodeId) return;
            ModelNode* m = out->find(id);
            m->hidden = n.hidden;
            switch (n.kind) {
                case NodeKind::TextRun: {
                    const auto& rp = std::get<RunPayload>(n.payload);
                    m->text = rp.text;
                    m->marks = rp.marks;
                    m->styleRef = rp.style;
                    break;
                }
                case NodeKind::ParagraphMark: {
                    const auto& mp = std::get<MarkPayload>(n.payload);
                    m->para = mp.para;
                    m->styleRef = mp.style;
                    m->marks = mp.marks;
                    break;
                }
                case NodeKind::ContentControl:
                    m->control = std::get<ControlSpec>(n.payload);
                    break;
                case NodeKind::Table:
                    m->table = std::get<TableSpec>(n.payload);
                    break;
                case NodeKind::TableRow:
                    m->row = std::get<RowSpec>(n.payload);
                    break;
                case NodeKind::TableCell:
                    m->cell = std::get<CellSpec>(n.payload);
                    break;
                case NodeKind::Section:
                    m->section = std::get<SectionSpec>(n.payload);
                    break;
                case NodeKind::Field:
                    m->field = std::get<FieldSpec>(n.payload);
                    break;
                default:
                    break;
            }
        }
        for (const Ref& c : n.children) build(*c, id);
    };
    build(*doc.root, out->rootId());
}

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
