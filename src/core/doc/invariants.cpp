#include "core/doc/invariants.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "core/doc/flow.h"
#include "core/doc/para.h"
#include "core/doc/table.h"

namespace heditor {

namespace {

/**
 * 规则名。测试按它们断言"报了哪一条"，所以集中写在这里，改名只有一处要动。
 */
constexpr char kRootAndSections[] = "结构.根唯一且其下是节序列";
constexpr char kChildKinds[] = "结构.子节点类型与叶子约束";
constexpr char kNoOrphan[] = "结构.无孤立节点";
constexpr char kHiddenInFlow[] = "结构.隐藏只在编辑模式下在流里";
constexpr char kTextNonEmpty[] = "文本.文本节点非空";
constexpr char kAdjacentRuns[] = "文本.相邻同标记的 run 必须合并";
constexpr char kBoundaryOrder[] = "容器.边界成对有序不交叉";
constexpr char kParagraphTiling[] = "容器.切段无缝隙";
constexpr char kUniqueId[] = "身份.节点 ID 唯一且索引自洽";
constexpr char kKindValid[] = "身份.节点种类在声明范围内";
constexpr char kTableGrid[] = "表格.跨度之和相等、无重叠、无空洞、有列定义则数据行不许合并";

void add(InvariantReport* r, const char* rule, NodeId node, int at, std::string detail) {
    r->violations.push_back(Violation{rule, node, at, std::move(detail)});
}

bool holdsChild(const std::vector<Node*>& children, const Node* n) {
    return std::find(children.begin(), children.end(), n) != children.end();
}

/**
 * 从根做一次前序遍历：收集顺序、可达集合，以及"自身或祖先声明了隐藏"的期望。
 */
void walk(const NodeTree& tree, NodeId id, bool ancestor_hidden, std::vector<NodeId>* order,
          std::unordered_set<NodeId>* reached, std::unordered_map<NodeId, bool>* want_hidden,
          int* reached_twice) {
    if (reached->count(id) != 0) {
        ++(*reached_twice);
        return;
    }
    const Node* n = tree.find(id);
    if (n == nullptr) return;
    reached->insert(id);
    order->push_back(id);
    (*want_hidden)[id] = ancestor_hidden || n->hidden;
    for (Node* c : n->children) {
        walk(tree, c->id, ancestor_hidden || n->hidden, order, reached, want_hidden, reached_twice);
    }
}

/**
 * 现在拿不到输入的条目。每条写明等哪一步带来输入，免得读的人以为已经查过了。
 *
 * 有一条不在这里：区间与选区的约束（选区不进模型、端点带方向、端点用流位置、端点外扩到整块
 * 边界）。它们作用在文档之外的值（Range 与步骤）上，不是文档不变量 —— 硬要写成一条检查，
 * 只能写成恒真，那比不写更差。判据在 tests/model_test.cpp 的区间与步骤那几节。
 */
std::vector<PendingRule> pendingRules() {
    return {
        {"文本.不允许零宽标记区间（标记的无重复与顺序在结构体表示下不可违反）",
         "1.2 临时的带标记区间"},
        {"结构.schema 的完整声明（哪些能装哪些、最少几个、最多几个）",
         "1.5 / 1.7 在控件与表格落地时逐种登记"},
        {"身份.控件 id 唯一、key 不要求唯一", "1.5 控件索引"},
        {"取值与嵌套.取值拼接、含子控件拒绝整体设值、同 group 互斥、真原子的文本表示",
         "1.5 控件与 1.4 原子"},
    };
}

}  // namespace

InvariantReport checkInvariants(const NodeTree& tree) {
    InvariantReport r;
    FlowIndex design(tree, FlowVisibility::Design);
    FlowIndex display(tree, FlowVisibility::Display);
    ParagraphIndex para(tree, FlowVisibility::Design);

    std::vector<NodeId> order;
    std::unordered_set<NodeId> reached;
    std::unordered_map<NodeId, bool> want_hidden;
    int reached_twice = 0;
    walk(tree, tree.rootId(), false, &order, &reached, &want_hidden, &reached_twice);

    // 结构.根唯一且其下是节序列
    r.checked.push_back(kRootAndSections);
    {
        int docs = 0;
        bool root_is_doc = false;
        for (NodeId id = 1; id <= tree.maxAllocatedId(); ++id) {
            const Node* n = tree.find(id);
            if (n == nullptr || n->kind != NodeKind::Document) continue;
            ++docs;
            if (id == tree.rootId()) root_is_doc = true;
        }
        if (docs != 1 || !root_is_doc) {
            add(&r, kRootAndSections, tree.rootId(), -1,
                "Document 节点有 " + std::to_string(docs) + " 个，根是 Document：" +
                    (root_is_doc ? "是" : "否"));
        }
        const Node* root = tree.find(tree.rootId());
        for (size_t i = 0; i < root->children.size(); ++i) {
            const NodeKind ck = root->children[i]->kind;
            if (ck != NodeKind::Section) {
                add(&r, kRootAndSections, root->children[i]->id, static_cast<int>(i),
                    std::string("根的孩子里有非节元素：") + nodeKindName(ck));
            }
        }
    }

    // 结构.子节点类型与叶子约束
    r.checked.push_back(kChildKinds);
    for (NodeId id : order) {
        const Node* n = tree.find(id);
        if (!isContainer(n->kind) && !n->children.empty()) {
            add(&r, kChildKinds, id, -1,
                std::string("非容器有孩子（自身是 ") + nodeKindName(n->kind) + "）");
            continue;
        }
        for (size_t i = 0; i < n->children.size(); ++i) {
            const NodeKind ck = n->children[i]->kind;
            if (!acceptsChild(n->kind, ck)) {
                add(&r, kChildKinds, n->children[i]->id, static_cast<int>(i),
                    std::string(nodeKindName(n->kind)) + " 不接受 " + nodeKindName(ck) +
                        " 作为子节点");
            }
        }
    }

    // 结构.无孤立节点
    r.checked.push_back(kNoOrphan);
    {
        if (reached_twice != 0) {
            add(&r, kNoOrphan, kInvalidNodeId, -1,
                "有 " + std::to_string(reached_twice) +
                    " 个节点被到达两次（父链成环，或两个父节点都持有它）");
        }
        for (NodeId id : order) {
            if (id == tree.rootId()) continue;
            const Node* n = tree.find(id);
            if (n->parent == nullptr) {
                add(&r, kNoOrphan, id, -1, "可达节点的父指针为空");
            } else if (!holdsChild(n->parent->children, n)) {
                add(&r, kNoOrphan, id, -1,
                    "可达节点的父节点的孩子列表里找不到它（父指针与孩子列表不一致）");
            }
        }
        // 池里不可达的节点：只允许出现在某棵被摘下的子树里（顶端父指针为空）。
        // 撤销要把摘下的子树原样搬回，所以"留在池里但不在树上"是合法状态。
        const int cap = static_cast<int>(tree.nodeCount()) + 1;
        for (NodeId id = 1; id <= tree.maxAllocatedId(); ++id) {
            const Node* n = tree.find(id);
            if (n == nullptr || reached.count(id) != 0) continue;
            const Node* top = n;
            int steps = 0;
            while (top->parent != nullptr && steps < cap) {
                top = top->parent;
                ++steps;
            }
            if (top->parent != nullptr || reached.count(top->id) != 0) {
                add(&r, kNoOrphan, id, -1,
                    "池里有既不在树上、也不属于某棵被摘下子树的节点（父链上溯不到顶端）");
            }
        }
    }

    // 结构.隐藏只在编辑模式下在流里
    r.checked.push_back(kHiddenInFlow);
    for (NodeId id : order) {
        if (!design.inFlow(id)) {
            add(&r, kHiddenInFlow, id, -1, "编辑态下有节点不在流里（该口径不摘除任何节点）");
        }
        const bool want_out = want_hidden.at(id);
        if (display.inFlow(id) == want_out) {
            add(&r, kHiddenInFlow, id, -1,
                std::string("呈现态的摘除集合与隐藏声明不符：自身或祖先声明隐藏 = ") +
                    (want_out ? "是" : "否") +
                    "，而呈现态在流里 = " + (display.inFlow(id) ? "是" : "否"));
        }
    }

    // 文本.文本节点非空
    r.checked.push_back(kTextNonEmpty);
    for (NodeId id : order) {
        const Node* n = tree.find(id);
        if (isText(n->kind) && n->text.empty()) {
            add(&r, kTextNonEmpty, id, -1,
                "文本节点为空（相邻同标记的 run 必须合并，空 run 无意义）");
        }
    }

    // 文本.相邻同标记的 run 必须合并
    //
    // 判的是树上的相邻（隐藏与否都算兄弟），不是流上的相邻：规范形状是文档自身的性质，
    // 与呈现口径无关。这条规则描述"已提交文档"的形状 —— 事务内部的中间态可以暂时不满足，
    // 因为规范化在提交前才跑（见 normalize.h）。
    r.checked.push_back(kAdjacentRuns);
    for (NodeId id : order) {
        const Node* n = tree.find(id);
        if (!isContainer(n->kind)) continue;
        for (size_t i = 0; i + 1 < n->children.size(); ++i) {
            const Node* a = n->children[i];
            const Node* b = n->children[i + 1];
            if (isText(a->kind) && isText(b->kind) && a->marks == b->marks) {
                add(&r, kAdjacentRuns, b->id, static_cast<int>(i) + 1,
                    "相邻两个文本节点的标记集合相同，必须合并（左 " +
                        std::to_string(a->text.size()) + " 个码点、右 " +
                        std::to_string(b->text.size()) + " 个码点）");
            }
        }
    }

    // 容器.边界成对有序不交叉，以及容器.切段无缝隙
    r.checked.push_back(kBoundaryOrder);
    r.checked.push_back(kParagraphTiling);
    for (NodeId id : order) {
        const Node* n = tree.find(id);
        if (!isContainer(n->kind)) continue;

        int first = 0;
        int end = 0;
        if (!design.intervalOf(id, &first, &end)) {
            add(&r, kBoundaryOrder, id, -1, "编辑态下容器没有流位置区间");
            continue;
        }
        const bool is_root = (id == tree.rootId());
        const int content_begin = is_root ? first : first + 1;
        const int content_end = is_root ? end : end - 1;
        if (!is_root && end - first < 2) {
            add(&r, kBoundaryOrder, id, -1, "非根容器的区间装不下两个边界位置");
        }
        int prev_end = content_begin;
        for (size_t i = 0; i < n->children.size(); ++i) {
            int cf = 0;
            int ce = 0;
            if (!design.intervalOf(n->children[i]->id, &cf, &ce)) continue;
            if (cf < prev_end) {
                add(&r, kBoundaryOrder, n->children[i]->id, static_cast<int>(i),
                    "孩子区间与前面的重叠或逆序（区间必须并列或嵌套）");
            }
            if (cf < content_begin || ce > content_end) {
                add(&r, kBoundaryOrder, n->children[i]->id, static_cast<int>(i),
                    "孩子区间超出父容器的内容范围");
            }
            prev_end = ce;
        }

        const std::vector<int>* marks = para.marksOf(id);
        if (marks == nullptr) {
            add(&r, kParagraphTiling, id, -1, "在流的容器没有段落记账（建表与走树不一致）");
            continue;
        }
        const int slots = static_cast<int>(n->children.size());
        const int count = para.paragraphCount(id);
        int expect_begin = 0;
        for (int no = 1; no <= count; ++no) {
            int b = -1;
            int e = -1;
            if (!para.paragraphRange(id, no, &b, &e)) {
                add(&r, kParagraphTiling, id, no, "第 no 段取不到范围");
                break;
            }
            if (b != expect_begin) {
                add(&r, kParagraphTiling, id, no,
                    "段落范围有缝隙或乱序：期望从 " + std::to_string(expect_begin) +
                        " 起，实际从 " + std::to_string(b) + " 起");
                break;
            }
            expect_begin = e;
        }
        if (expect_begin != slots) {
            add(&r, kParagraphTiling, id, -1,
                "段落范围没有铺满容器的孩子（铺到 " + std::to_string(expect_begin) + "，孩子数 " +
                    std::to_string(slots) + "）");
        }
        int prev_no = 0;
        for (int i = 0; i <= slots; ++i) {
            const int no = para.paragraphNoIn(id, i);
            int before = 0;
            for (int mk : *marks) {
                if (mk < i) ++before;
            }
            if (no != before + 1) {
                add(&r, kParagraphTiling, id, i,
                    "槽位所属段落序号与标记个数不符：期望 " + std::to_string(before + 1) +
                        "，实际 " + std::to_string(no));
            }
            if (no < prev_no) {
                add(&r, kParagraphTiling, id, i, "段落序号随槽位倒退");
            }
            prev_no = no;
        }
    }

    // 身份.节点 ID 唯一且索引自洽
    r.checked.push_back(kUniqueId);
    {
        int found = 0;
        for (NodeId id = 1; id <= tree.maxAllocatedId(); ++id) {
            const Node* n = tree.find(id);
            if (n == nullptr) continue;
            ++found;
            if (n->id != id) {
                add(&r, kUniqueId, id, -1, "索引键与节点的 id 不符（索引自洽性被破坏）");
            }
        }
        if (found != static_cast<int>(tree.nodeCount())) {
            add(&r, kUniqueId, kInvalidNodeId, -1,
                "池里有 " + std::to_string(tree.nodeCount()) + " 个节点，按 id 只找到 " +
                    std::to_string(found) + " 个（有重复 id 或索引漏项）");
        }
    }

    // 身份.节点种类在声明范围内
    r.checked.push_back(kKindValid);
    {
        const int max_kind = static_cast<int>(NodeKind::ParagraphMark);
        for (NodeId id = 1; id <= tree.maxAllocatedId(); ++id) {
            const Node* n = tree.find(id);
            if (n == nullptr) continue;
            if (static_cast<int>(n->kind) > max_kind) {
                add(&r, kKindValid, id, -1, "节点种类超出声明范围（枚举值越界）");
            }
        }
    }

    // 表格.跨度之和相等、无重叠、无空洞、有列定义则数据行不许合并
    //
    // 前三条是派生网格的性质（第二节），所以这里是"推一遍网格，看它成不成立"。
    // 规则名沿用了第二节的措辞，但其中两条不可由数据构造，因此不构成数据判据：
    // "续格紧邻原点格"—— 模型里根本没有续格（被合并掉的格位不存在），由结构保证；
    // "无重叠"—— 放置算法总是把格子放在第一个空闲格位，两个格子落不到同一个格位。
    // 它们仍然留在网格推导里当守卫（将来改放置算法时先亮），只是别把它们当成"查过了"。
    r.checked.push_back(kTableGrid);
    for (NodeId id = 1; id <= tree.maxAllocatedId(); ++id) {
        const Node* n = tree.find(id);
        if (n == nullptr || n->kind != NodeKind::Table) continue;

        TableGrid grid;
        std::string why;
        if (!buildGrid(tree, id, &grid, &why)) {
            add(&r, kTableGrid, id, -1, why);
            continue;
        }
        if (!n->table.hasColumns()) continue;

        if (static_cast<int>(n->table.columns.size()) != grid.cols) {
            add(&r, kTableGrid, id, -1,
                "列定义有 " + std::to_string(n->table.columns.size()) + " 列，网格有 " +
                    std::to_string(grid.cols) + " 列");
            continue;
        }
        // 数据行不许合并：每一格的跨度都必须是 1x1（于是格数等于列数，"第 i 列"才是
        // 确定的位置）。表头行不受这条限制。
        for (size_t row_no = 0; row_no < n->children.size(); ++row_no) {
            if (static_cast<int>(row_no) < n->table.header_rows) continue;
            for (const Node* cell : n->children[row_no]->children) {
                if (cell->cell.row_span != 1 || cell->cell.col_span != 1) {
                    add(&r, kTableGrid, cell->id, static_cast<int>(row_no),
                        "有列定义的表格里数据行不允许合并（第 " + std::to_string(row_no) + " 行）");
                }
            }
        }
    }

    r.pending = pendingRules();
    return r;
}

}  // namespace heditor
