/**
 * 文档模型 0.1 / 0.2 / 0.3 的判据：位置换算、段落分段、不变量检查器。
 *
 * 这一层没法靠"看起来对"验收 —— 位置错一格，表象是光标跳、命中偏、撤销回错地方，
 * 全都要到界面才看得见。所以判据只落在数据层，三条可证伪的：
 *   0.1 流位置不丢：对每个流位置 f，flowAt(posAt(f)) == f；
 *       往返不改归属：对每个合法 Pos p，flowAt(posAt(flowAt(p))) == flowAt(p)。
 *       第二条这样写是为了绕开"同一位置有多种 Pos 写法"—— 容器的第 0 个槽位与第一个孩子的
 *       第一个位置本来就是同一个流位置，要求 Pos 逐字段相等会把正确实现判成错。
 *   0.2 下标数组与全部段落查询跟暴力算法逐项比对，并在随机增删中每步与重建结果整体比对。
 *   0.3 不变量检查器：对现有形状全绿，且逐条构造反例时必须报出对应的那条规则。
 *
 * 覆盖四层：写死的形状（嵌套、跨段落标记、空容器、表格、隐藏）、穷举、随机、以及增删序列，
 * 前两层在两种可见性口径下各过一遍（位置空间不同，但"往返互逆"必须各自成立）。
 */
#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "core/doc/flow.h"
#include "core/doc/invariants.h"
#include "core/doc/node.h"
#include "core/doc/para.h"

using namespace heditor;

namespace {

int g_checks = 0;
int g_failures = 0;

void expectTrue(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

std::vector<uint32_t> cps(std::initializer_list<uint32_t> list) { return std::vector<uint32_t>(list); }

/**
 * 前向声明：增量测试要顺带在增删之后的树上跑一遍不变量检查，
 * 而检查相关的辅助函数写在下面（0.3 那一节）。
 */
void expectClean(const NodeTree& tree, const std::string& label);

/**
 * 对一个流位置集合做两条判据，并顺带枚举该树的全部合法 Pos。
 * 返回该树流位置的个数，方便调用方打印规模。
 */
void checkTree(const NodeTree& tree, FlowVisibility vis, const std::string& label) {
    FlowIndex idx(tree, vis);

    int lost = 0;
    for (int f = 0; f <= idx.size(); ++f) {
        const Pos p = idx.posAt(f);
        if (idx.flowAt(p) != f) ++lost;
    }
    expectTrue(lost == 0, label + "：每个流位置都能回到自己（共 " +
                              std::to_string(idx.size() + 1) + " 个位置，不一致 " +
                              std::to_string(lost) + " 个）");

    int bad = 0;
    int tried = 0;
    int reachable_from_removed = 0;
    for (NodeId id = 1; id <= tree.maxAllocatedId(); ++id) {
        const Node* n = tree.find(id);
        if (n == nullptr) continue;

        // 被摘除的节点：整个不在流里，任何位置都该被拒（不是"没试到"）。
        if (!idx.inFlow(id)) {
            int offs = 0;
            if (isContainer(n->kind)) {
                offs = static_cast<int>(n->children.size());
            } else if (isText(n->kind)) {
                offs = static_cast<int>(n->text.size());
            }
            for (int off = 0; off <= offs; ++off) {
                if (idx.flowAt(Pos{id, off}) >= 0) ++reachable_from_removed;
            }
            continue;
        }

        int limit = 0;
        if (isContainer(n->kind)) {
            limit = static_cast<int>(n->children.size());
        } else if (isText(n->kind)) {
            limit = static_cast<int>(n->text.size());
        }
        for (int off = 0; off <= limit; ++off) {
            // 槽位 j 的含义是"第 j-1 个孩子之后"：那个孩子被摘除时这个位置本来就不存在，
            // 所以按"该不该可寻址"分别核对 —— 不能一律要求可寻址，也不能一律放过。
            if (off > 0 && isContainer(n->kind) && !idx.inFlow(n->children[off - 1]->id)) {
                if (idx.flowAt(Pos{id, off}) >= 0) ++bad;
                continue;
            }
            const FlowPos f = idx.flowAt(Pos{id, off});
            if (f < 0) {
                ++bad;
                continue;
            }
            ++tried;
            if (idx.flowAt(idx.posAt(f)) != f) ++bad;
        }
    }
    expectTrue(bad == 0, label + "：枚举 " + std::to_string(tried) +
                              " 个合法位置，往返后流位置不变（不一致 " + std::to_string(bad) +
                              " 个）");
    expectTrue(reachable_from_removed == 0, label + "：被摘除的节点没有任何可寻址位置");
    // 越界必须被拒，不能静默夹取 —— 夹取会把调用方的算错藏起来。
    const Node* sec = tree.find(tree.firstSectionId());
    expectTrue(idx.flowAt(Pos{sec->id, static_cast<int>(sec->children.size()) + 1}) < 0,
               label + "：越界槽位被拒（返回 -1）");
    expectTrue(idx.flowAt(Pos{kInvalidNodeId, 0}) < 0, label + "：无效容器被拒（返回 -1）");
}

/**
 * 单段：Section → TextRun("abc") → ParagraphMark
 */
NodeTree makeSingleParagraph() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61, 0x62, 0x63}));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

/**
 * 两个 run 挨着：Section → TextRun("ab"), TextRun("cd"), ParagraphMark
 */
NodeTree makeTwoRuns() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61, 0x62}));
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x63, 0x64}));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

/**
 * 删空的控件：Section → ContentControl（无子节点）→ ParagraphMark
 */
NodeTree makeEmptyControl() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.appendChild(sec, NodeKind::ContentControl);
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

/**
 * 控件跨过段落标记：Section → CC(TextRun("x"), ParagraphMark, TextRun("y")) → ParagraphMark
 */
NodeTree makeControlAcrossMark() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
    tree.setText(tree.appendChild(cc, NodeKind::TextRun), cps({0x78}));
    tree.appendChild(cc, NodeKind::ParagraphMark);
    tree.setText(tree.appendChild(cc, NodeKind::TextRun), cps({0x79}));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

/**
 * 嵌套控件：Section → CC(CC(TextRun("a"))) → ParagraphMark
 */
NodeTree makeNestedControls() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    const NodeId outer = tree.appendChild(sec, NodeKind::ContentControl);
    const NodeId inner = tree.appendChild(outer, NodeKind::ContentControl);
    tree.setText(tree.appendChild(inner, NodeKind::TextRun), cps({0x61}));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

/**
 * 表格：Section → Table(TableRow(TableCell(TextRun("36.8")))) → ParagraphMark
 */
NodeTree makeTable() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    const NodeId table = tree.appendChild(sec, NodeKind::Table);
    const NodeId row = tree.appendChild(table, NodeKind::TableRow);
    const NodeId cell = tree.appendChild(row, NodeKind::TableCell);
    tree.setText(tree.appendChild(cell, NodeKind::TextRun), cps({0x33, 0x36, 0x2E, 0x38}));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

/**
 * 空容器的四个语义位置必须是四个互不相同的流位置。
 *
 * 这是上一轮四个边界缺陷的正面判据：那时"在 [ 与 ] 之间"在整数偏移里没有编码，
 * 于是"控件之前""进入控件""控件之后"会撞在一起。现在它们各自有位置。
 */
void checkEmptyContainerBoundaries(FlowVisibility vis) {
    NodeTree tree = makeEmptyControl();
    const NodeId sec = tree.firstSectionId();
    const Node* s = tree.find(sec);
    const NodeId cc = s->children[0]->id;
    FlowIndex idx(tree, vis);

    const FlowPos before = idx.flowAt(Pos{sec, 0});
    const FlowPos inside = idx.flowAt(Pos{cc, 0});
    const FlowPos after = idx.flowAt(Pos{sec, 1});
    expectTrue(before >= 0 && inside >= 0 && after >= 0 && before != inside && inside != after &&
                   before != after,
               "删空的控件：'控件之前'/'进入控件'/'控件之后'是三个不同的流位置");
    expectTrue(idx.flowAt(Pos{cc, 1}) < 0,
               "删空的控件：内容槽位只有一个（0），越界的 1 被拒");
}

/**
 * 随机树：反复往随机一个容器上挂子节点，形状不可预知。
 */
NodeTree makeRandomTree(uint32_t seed, int target_nodes) {
    std::mt19937 rng(seed);
    NodeTree tree;

    static const NodeKind kKinds[] = {
        NodeKind::ContentControl, NodeKind::Quote,   NodeKind::ListItem, NodeKind::Link,
        NodeKind::Image,          NodeKind::Formula, NodeKind::Field,    NodeKind::Tab,
        NodeKind::TextRun,        NodeKind::ParagraphMark,
    };
    const int kind_count = static_cast<int>(sizeof(kKinds) / sizeof(kKinds[0]));

    while (static_cast<int>(tree.nodeCount()) < target_nodes) {
        // 收集可当父节点的容器，从里面挑一个。Document 不在其中：第二节要求根的孩子
        // 只能是节，把随机元素挂到根上会造出一份非法文档（0.3 的检查器抓到过这一点）。
        std::vector<NodeId> containers;
        for (NodeId id = 1; id <= tree.maxAllocatedId(); ++id) {
            const Node* n = tree.find(id);
            if (n == nullptr || !isContainer(n->kind)) continue;
            if (id == tree.rootId()) continue;
            containers.push_back(id);
        }
        const NodeId parent = containers[rng() % containers.size()];
        const NodeKind kind = kKinds[rng() % static_cast<uint32_t>(kind_count)];
        const NodeId id = tree.appendChild(parent, kind);
        if (id == kInvalidNodeId) continue;
        if (kind == NodeKind::TextRun) {
            const int len = 1 + static_cast<int>(rng() % 3);  // 文本节点非空
            std::vector<uint32_t> text;
            for (int i = 0; i < len; ++i) text.push_back(0x61 + (rng() % 26));
            tree.setText(id, std::move(text));
        }
    }
    return tree;
}

/**
 * 隐藏：Display 口径下整棵子树从流里摘除。
 *
 * 判据是"位置空间真的变了、且变的只是那一块"：落差正好是被摘除子树的跨度；
 * 被摘除的节点在呈现态没有任何可寻址位置；而它两侧的槽位仍然可寻址。
 */
void checkHiddenSubtree() {
    // Section → TextRun("ab"), ContentControl(hidden)[TextRun("xy"), ParagraphMark],
    //           ParagraphMark
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61, 0x62}));
    const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
    tree.setText(tree.appendChild(cc, NodeKind::TextRun), cps({0x78, 0x79}));
    const NodeId inner_mark = tree.appendChild(cc, NodeKind::ParagraphMark);
    tree.appendChild(sec, NodeKind::ParagraphMark);
    tree.find(cc)->hidden = true;

    checkTree(tree, FlowVisibility::Design, "隐藏树 / 编辑态");
    checkTree(tree, FlowVisibility::Display, "隐藏树 / 呈现态");

    FlowIndex design(tree, FlowVisibility::Design);
    FlowIndex display(tree, FlowVisibility::Display);

    // 编辑态：TextRun(2) + 控件(起始 1 + 内容 3 + 结束 1) + 段落标记(1) = 8，Section 两个边界 +2 → 10。
    // 呈现态摘除整个控件 → 2 + 1 + 2 = 5。
    expectTrue(design.size() == 10 && display.size() == 5,
               "隐藏：编辑态 10 个位置、呈现态 5 个（落差正好是被摘除子树的跨度）");
    expectTrue(design.inFlow(cc) && design.inFlow(inner_mark),
               "隐藏：编辑态下隐藏控件与它内部的段落标记都在流里");
    expectTrue(!display.inFlow(cc) && !display.inFlow(inner_mark),
               "隐藏：呈现态下整棵子树（含内部的段落标记）都不在流里");
    expectTrue(display.flowAt(Pos{cc, 0}) < 0 && display.flowAt(Pos{inner_mark, 0}) < 0,
               "隐藏：呈现态下被摘除的节点没有任何可寻址位置");
    // 槽位 2 的含义是"在被摘除的控件之后"，而那个位置不存在 —— 这条是要记住的后果。
    expectTrue(display.flowAt(Pos{sec, 2}) < 0,
               "隐藏：呈现态下被摘除者之后那个槽位不可寻址");
    expectTrue(display.flowAt(Pos{sec, 1}) >= 0 && display.flowAt(Pos{sec, 3}) >= 0,
               "隐藏：呈现态下被摘除者两侧的槽位仍然可寻址");
}

/**
 * 一个容器的暴力结果：标记下标数组、段落数、逐槽位的段落序号。
 */
struct ParaRef {
    std::vector<int> marks;
    int count = 0;
    std::vector<ParaNo> no_at;
};

/**
 * 暴力算法：直接扫子节点序列数一遍，作为段落索引的对照。
 */
ParaRef brutePara(const NodeTree& tree, FlowVisibility vis, NodeId container) {
    ParaRef r;
    const Node* c = tree.find(container);
    const int n = static_cast<int>(c->children.size());
    for (int i = 0; i < n; ++i) {
        const Node* ch = c->children[static_cast<size_t>(i)];
        if (vis == FlowVisibility::Display && ch->hidden) continue;
        if (isParagraphMark(ch->kind)) r.marks.push_back(i);
    }
    const int last = r.marks.empty() ? -1 : r.marks.back();
    r.count = static_cast<int>(r.marks.size()) + ((n - last - 1) > 0 ? 1 : 0);
    for (int at = 0; at <= n; ++at) {
        int before = 0;
        for (int mk : r.marks) {
            if (mk < at) ++before;
        }
        r.no_at.push_back(before + 1);
    }
    return r;
}

/**
 * 按可见性口径走一遍树，收集在流里的容器（升序）与其中的标记总数。
 */
void walkInFlow(const NodeTree& tree, FlowVisibility vis, NodeId id, std::vector<NodeId>* out,
                int* marks) {
    const Node* n = tree.find(id);
    if (n == nullptr || !isContainer(n->kind)) return;
    if (vis == FlowVisibility::Display && n->hidden) return;
    out->push_back(id);
    for (const Node* c : n->children) {
        if (vis == FlowVisibility::Display && c->hidden) continue;
        if (isParagraphMark(c->kind)) ++(*marks);
        walkInFlow(tree, vis, c->id, out, marks);
    }
}

/**
 * 结构上仍挂在树上的容器（与可见性无关），但排除根：第二节要求根的孩子只能是节，
 * 把随机元素挂到根上会造出非法文档。增量测试用它挑父节点。
 */
void collectAttached(const NodeTree& tree, NodeId id, std::vector<NodeId>* out) {
    const Node* n = tree.find(id);
    if (n == nullptr || !isContainer(n->kind)) return;
    if (id != tree.rootId()) out->push_back(id);
    for (const Node* c : n->children) collectAttached(tree, c->id, out);
}

/**
 * 0.2 的判据：对每个在流里的容器，把下标数组与全部段落查询跟暴力算法逐项比对。
 */
void checkParaIndex(const NodeTree& tree, FlowVisibility vis, const std::string& label) {
    ParaIndex idx(tree, vis);

    std::vector<NodeId> in_flow;
    int marks_in_flow = 0;
    walkInFlow(tree, vis, tree.rootId(), &in_flow, &marks_in_flow);
    // 走树得到的是前序，记账表按 id 升序 —— 比之前先对齐顺序，否则比的是两种排列。
    std::sort(in_flow.begin(), in_flow.end());
    expectTrue(idx.containers() == in_flow,
               label + "：记账的容器正好是在流里的那些（" + std::to_string(in_flow.size()) +
                   " 个）");

    int bad_marks = 0;
    int bad_count = 0;
    int bad_no = 0;
    int bad_range = 0;
    int bad_mark = 0;
    for (NodeId id : in_flow) {
        const ParaRef ref = brutePara(tree, vis, id);
        const std::vector<int>* m = idx.marksOf(id);
        if (m == nullptr || *m != ref.marks) ++bad_marks;
        if (idx.paragraphCount(id) != ref.count) ++bad_count;

        const Node* c = tree.find(id);
        const int n = static_cast<int>(c->children.size());
        const int k = static_cast<int>(ref.marks.size());
        for (int at = 0; at <= n; ++at) {
            if (idx.paragraphNoIn(id, at) != ref.no_at[static_cast<size_t>(at)]) ++bad_no;
        }
        for (int no = 1; no <= ref.count; ++no) {
            int b = -1;
            int e = -1;
            const int want_b = (no == 1) ? 0 : ref.marks[static_cast<size_t>(no) - 2] + 1;
            const int want_e = (no <= k) ? ref.marks[static_cast<size_t>(no) - 1] + 1 : n;
            if (!idx.paragraphRange(id, no, &b, &e)) {
                ++bad_range;
            } else if (b != want_b || e != want_e) {
                ++bad_range;
            }
            if (no <= k) {
                bool ok = false;
                const NodeId mk = idx.markOfParagraph(id, no, &ok);
                const NodeId want =
                    c->children[static_cast<size_t>(ref.marks[static_cast<size_t>(no) - 1])]->id;
                if (!ok || mk != want) ++bad_mark;
            }
        }
        // 段落数之后那一段要么不存在、要么已经结束：范围查询必须一致地拒绝。
        int b = -1;
        int e = -1;
        if (idx.paragraphRange(id, ref.count + 1, &b, &e)) ++bad_range;
        if (ref.count + 1 > k && idx.markOfParagraph(id, ref.count + 1) != kInvalidNodeId) {
            ++bad_mark;
        }
    }
    expectTrue(bad_marks == 0 && bad_count == 0 && bad_no == 0 && bad_range == 0 && bad_mark == 0,
               label + "：下标数组 / 段落数 / 序号 / 范围 / 结束标记全部与暴力算法一致");
    expectTrue(idx.markCount(kInvalidNodeId) == -1 && idx.marksOf(kInvalidNodeId) == nullptr,
               label + "：不在树上的容器没有标记数组（-1 与 nullptr）");
}

/**
 * 两个段落索引逐项相同吗？返回不同的条目数，0 表示一致。
 *
 * 比对范围含记账的容器、下标数组、段落数、逐槽位的段落序号与逐段的范围 ——
 * 只比下标数组会漏掉"数错了段落数"这类错，而那种错同样会让脏段标记失准。
 */
int diffIndexes(const NodeTree& tree, const ParaIndex& a, const ParaIndex& b) {
    const std::vector<NodeId> ca = a.containers();
    const std::vector<NodeId> cb = b.containers();
    if (ca != cb) return 1;

    int diff = 0;
    for (NodeId id : ca) {
        const std::vector<int>* ma = a.marksOf(id);
        const std::vector<int>* mb = b.marksOf(id);
        if (ma == nullptr || mb == nullptr) {
            ++diff;
            continue;
        }
        if (*ma != *mb) ++diff;
        if (a.paragraphCount(id) != b.paragraphCount(id)) ++diff;
        const int n = static_cast<int>(tree.find(id)->children.size());
        for (int at = 0; at <= n; ++at) {
            if (a.paragraphNoIn(id, at) != b.paragraphNoIn(id, at)) ++diff;
        }
        for (int no = 1; no <= n + 1; ++no) {
            int ba = -1;
            int ea = -1;
            int bb = -1;
            int eb = -1;
            const bool oa = a.paragraphRange(id, no, &ba, &ea);
            const bool ob = b.paragraphRange(id, no, &bb, &eb);
            if (oa != ob || (oa && (ba != bb || ea != eb))) ++diff;
        }
    }
    return diff;
}

/**
 * 分段边界：空容器 0 段、只有标记的容器 1 个空段、末尾未结束的段、内容末尾的探针。
 */
void checkParaBoundaries() {
    // 空容器：没有孩子也没有标记，所以 0 段；它的唯一槽位是"还没有内容的那一段"的位置。
    {
        NodeTree tree = makeEmptyControl();
        const NodeId sec = tree.firstSectionId();
        const NodeId cc = tree.find(sec)->children[0]->id;
        ParaIndex idx(tree, FlowVisibility::Design);
        int b = -1;
        int e = -1;
        expectTrue(idx.markCount(cc) == 0 && idx.paragraphCount(cc) == 0, "空容器：0 个标记、0 段");
        expectTrue(idx.paragraphNoIn(cc, 0) == 1, "空容器：唯一槽位属于第 1 段的位置");
        expectTrue(!idx.paragraphRange(cc, 1, &b, &e), "空容器：第 1 段没有内容，范围查询拒绝");
        expectTrue(idx.paragraphCount(sec) == 1, "空容器：外层 Section 仍是 1 段");
    }
    // 只有一个标记：它是一个空段落，而且内容末尾的槽位属于"下一段"。
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        const NodeId mk = tree.appendChild(sec, NodeKind::ParagraphMark);
        ParaIndex idx(tree, FlowVisibility::Design);
        int b = -1;
        int e = -1;
        expectTrue(idx.paragraphCount(sec) == 1 && idx.paragraphRange(sec, 1, &b, &e) && b == 0 &&
                       e == 1,
                   "只有标记：1 段，范围就是那个标记本身");
        bool ok = false;
        expectTrue(idx.markOfParagraph(sec, 1, &ok) == mk && ok, "只有标记：属性宿主就是它");
        expectTrue(idx.paragraphNoIn(sec, 1) == 2 && idx.paragraphCount(sec) == 1,
                   "只有标记：内容末尾那个槽位的段落序号比段落数大 1（属于尚未开始的下一段）");
    }
    // 末尾未结束的段：范围取到孩子末尾，且它没有结束标记 —— 属性无处可挂。
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61}));
        tree.appendChild(sec, NodeKind::ParagraphMark);
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x62}));
        ParaIndex idx(tree, FlowVisibility::Design);
        int b = -1;
        int e = -1;
        expectTrue(idx.paragraphCount(sec) == 2, "末尾未结束：2 段（第一段有标记，第二段还没有）");
        expectTrue(idx.paragraphRange(sec, 2, &b, &e) && b == 2 && e == 3,
                   "末尾未结束：第二段的范围是它那一个孩子");
        expectTrue(idx.markOfParagraph(sec, 2) == kInvalidNodeId,
                   "末尾未结束：第二段没有结束标记，属性无处可挂");
    }
}

/**
 * 隐藏与分段：呈现态下被摘除的子树整棵不在流里，其中的标记也跟着不在。
 *
 * 这条是第七节"隐藏会影响段落结构"的正面判据 —— 段落数随模式变，不是排版差异。
 */
void checkParaHidden() {
    // Section → TextRun("ab"), ContentControl(hidden)[TextRun("xy"), ParagraphMark],
    //           ParagraphMark
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61, 0x62}));
    const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
    tree.setText(tree.appendChild(cc, NodeKind::TextRun), cps({0x78, 0x79}));
    tree.appendChild(cc, NodeKind::ParagraphMark);
    tree.appendChild(sec, NodeKind::ParagraphMark);
    tree.find(cc)->hidden = true;

    checkParaIndex(tree, FlowVisibility::Design, "隐藏与分段 / 编辑态");
    checkParaIndex(tree, FlowVisibility::Display, "隐藏与分段 / 呈现态");

    ParaIndex design(tree, FlowVisibility::Design);
    ParaIndex display(tree, FlowVisibility::Display);

    int design_marks = 0;
    int display_marks = 0;
    for (NodeId id : design.containers()) design_marks += design.markCount(id);
    for (NodeId id : display.containers()) display_marks += display.markCount(id);
    expectTrue(design_marks == 2 && display_marks == 1,
               "隐藏与分段：呈现在流里的标记少 1 个（正好是隐藏子树里的那个）");
    expectTrue(display.marksOf(cc) == nullptr && display.markCount(cc) == -1,
               "隐藏与分段：呈现态下被摘除的容器没有段落结构");
    expectTrue(design.paragraphCount(cc) == 1, "隐藏与分段：编辑态下隐藏控件的内容也是一段");
    expectTrue(design.paragraphCount(sec) == 1 && display.paragraphCount(sec) == 1,
               "隐藏与分段：Section 自己的段落数不变（结束它那一段的标记仍在流里）");
}

/**
 * 0.2 的核心判据：随机增删之后，整张表必须与重新构造的结果逐项相同。
 *
 * 比对取"整张表"而不是抽查一个容器 —— 增量维护的错法（漏移下标、漏减一）常常只影响
 * 一条容器的后半段，抽查会漏掉。每一步都比对，所以错的步号也能从失败信息看出来。
 */
void checkParaIncremental(uint32_t seed, FlowVisibility vis, const char* vis_name) {
    std::mt19937 rng(seed);
    NodeTree tree = makeRandomTree(seed, 30);

    // 让一部分容器隐藏：呈现态下它们整棵被摘除，对它们的增删通知必须被拒。
    int hidden = 0;
    for (NodeId id = 1; id <= tree.maxAllocatedId() && hidden < 3; ++id) {
        Node* n = tree.find(id);
        if (n == nullptr || !isContainer(n->kind)) continue;
        if (n->id == tree.rootId() || n->id == tree.firstSectionId()) continue;
        if (rng() % 3 == 0) {
            n->hidden = true;
            ++hidden;
        }
    }

    const std::string pre = "增量 #" + std::to_string(seed) + " / " + vis_name;
    ParaIndex idx(tree, vis);
    checkParaIndex(tree, vis, pre + " 起始");

    static const NodeKind kKinds[] = {NodeKind::ContentControl, NodeKind::TextRun,
                                      NodeKind::ParagraphMark, NodeKind::Image,
                                      NodeKind::Quote,        NodeKind::Tab};
    const int kind_count = static_cast<int>(sizeof(kKinds) / sizeof(kKinds[0]));

    int inserts = 0;
    int erases = 0;
    int bad_notify = 0;
    int mismatch = 0;
    for (int step = 0; step < 80; ++step) {
        std::vector<NodeId> parents;
        collectAttached(tree, tree.rootId(), &parents);
        const NodeId parent = parents[rng() % parents.size()];
        const int n = static_cast<int>(tree.find(parent)->children.size());
        const bool do_insert = (n == 0) || (rng() % 100 < 55);

        bool accepted = false;
        if (do_insert) {
            const int at = static_cast<int>(rng() % static_cast<uint32_t>(n + 1));
            const NodeKind kind = kKinds[rng() % static_cast<uint32_t>(kind_count)];
            const NodeId id = tree.insertChild(parent, at, kind);
            if (id == kInvalidNodeId) continue;
            if (kind == NodeKind::TextRun) tree.setText(id, cps({0x61}));
            accepted = idx.onInsert(parent, at, id);
            ++inserts;
        } else {
            const int at = static_cast<int>(rng() % static_cast<uint32_t>(n));
            const NodeId erased = tree.eraseChild(parent, at);
            if (erased == kInvalidNodeId) continue;
            accepted = idx.onErase(parent, at, erased);
            ++erases;
        }
        // 通知的接受与拒绝必须与"父容器是否被记账"一致：拒绝只可能因为容器不在流里。
        if (accepted != (idx.marksOf(parent) != nullptr)) ++bad_notify;

        if (diffIndexes(tree, idx, ParaIndex(tree, vis)) != 0) ++mismatch;
    }

    expectTrue(inserts > 20 && erases > 15,
               pre + "：随机序列真的做了增删（插入 " + std::to_string(inserts) + "、删除 " +
                   std::to_string(erases) + "）");
    expectTrue(bad_notify == 0, pre + "：每次通知的接受与拒绝都与记账范围一致");
    expectTrue(mismatch == 0, pre + "：每一步之后整张表都与重新构造的结果逐项相同");
    checkParaIndex(tree, vis, pre + " 终态");
    // 0.3："任何编辑操作之后都要重新满足"——这里就是对真实增删序列之后的树跑一遍。
    expectClean(tree, pre + " 增删之后的不变量");
}

/**
 * 0.3：这份文档跑一遍不变量检查必须零违规。有违规就报第一条，便于定位。
 */
void expectClean(const NodeTree& tree, const std::string& label) {
    const InvariantReport rep = checkInvariants(tree);
    if (rep.ok()) {
        expectTrue(true, label + "：不变量全绿（查了 " + std::to_string(rep.checked.size()) +
                           " 条，待落地 " + std::to_string(rep.pending.size()) + " 条）");
        return;
    }
    expectTrue(false, label + "：违规 " + std::to_string(rep.violations.size()) +
                          " 条，第一条是 " + rep.violations[0].rule + " —— " +
                          rep.violations[0].detail);
}

/**
 * 违规清单里有没有报出这条规则。
 */
bool reported(const InvariantReport& rep, const std::string& rule) {
    for (const Violation& v : rep.violations) {
        if (v.rule == rule) return true;
    }
    return false;
}

/**
 * 实际跑过的规则里有这一条吗。
 */
bool checked(const InvariantReport& rep, const std::string& rule) {
    for (const std::string& s : rep.checked) {
        if (s == rule) return true;
    }
    return false;
}

/**
 * 检查器自身的判据：跑过的规则正好是登记的那些，待落地的每条都写明了等哪一步。
 *
 * 这两条是给"检查器本身"设的闸：少跑一条规则（静默跳过）或把 pending 写成空壳，
 * 都会让上面那些"全绿"的断言变得没有意义。
 */
void checkInvariantRuleSet() {
    const InvariantReport rep = checkInvariants(makeSingleParagraph());
    static const char* kWant[] = {
        "结构.根唯一且其下是节序列", "结构.子节点类型与叶子约束",
        "结构.无孤立节点",           "结构.隐藏只在编辑模式下在流里",
        "文本.文本节点非空",         "容器.边界成对有序不交叉",
        "容器.切段无缝隙",           "身份.节点 ID 唯一且索引自洽",
        "身份.节点种类在声明范围内",
    };
    int missing = 0;
    for (const char* w : kWant) {
        if (!checked(rep, w)) ++missing;
    }
    expectTrue(missing == 0 && rep.checked.size() == sizeof(kWant) / sizeof(kWant[0]),
               "0.3：实际跑过的规则正好是这 " + std::to_string(sizeof(kWant) / sizeof(kWant[0])) +
                   " 条（少一条就是静默跳过）");

    int incomplete = 0;
    for (const PendingRule& p : rep.pending) {
        if (p.rule.empty() || p.waiting_for.empty()) ++incomplete;
    }
    expectTrue(!rep.pending.empty() && incomplete == 0,
               "0.3：待落地 " + std::to_string(rep.pending.size()) +
                   " 条，每条都写明了等哪一步带来输入");
}

/**
 * 反向判据：逐条手工构造违规，断言检查器报出的正是那条规则。
 *
 * 少了这一半，"永远返回全绿"的检查器也能过 —— 那时上面所有"全绿"什么都没证明。
 * 手工改字段是允许的：Node 的字段公开就是为了取证（见 node.h）。
 * 能构造反例的是"数据类"规则（约束树的字段）；"派生量交叉校验"类规则
 * （边界有序、切段无缝隙、隐藏与流）约束的是从树算出来的量，数据层构造不出反例，
 * 它们的反例要改 flow.cpp 或 para.cpp —— 那类规则的价值是守住那两个实现。
 */
void checkInvariantCounterexamples() {
    {
        NodeTree tree;
        tree.insertChild(tree.rootId(), 0, NodeKind::TextRun);
        expectTrue(reported(checkInvariants(tree), "结构.根唯一且其下是节序列"),
                   "反例：根下塞进非节元素 → 报根唯一与节序列");
    }
    {
        NodeTree tree;
        const NodeId table = tree.appendChild(tree.firstSectionId(), NodeKind::Table);
        tree.appendChild(table, NodeKind::TextRun);
        expectTrue(reported(checkInvariants(tree), "结构.子节点类型与叶子约束"),
                   "反例：表格里塞文本 → 报子节点类型");
    }
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        const NodeId a = tree.appendChild(sec, NodeKind::TextRun);
        tree.setText(a, cps({0x61}));
        const NodeId b = tree.appendChild(sec, NodeKind::TextRun);
        tree.setText(b, cps({0x62}));
        tree.find(a)->children.push_back(tree.find(b));
        expectTrue(reported(checkInvariants(tree), "结构.子节点类型与叶子约束"),
                   "反例：文本节点挂了孩子 → 报叶子约束");
    }
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        const NodeId a = tree.appendChild(sec, NodeKind::TextRun);
        tree.setText(a, cps({0x61}));
        const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
        tree.find(a)->parent = tree.find(cc);
        expectTrue(reported(checkInvariants(tree), "结构.无孤立节点"),
                   "反例：父指针与孩子列表不一致 → 报无孤立节点");
    }
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        const NodeId t = tree.appendChild(sec, NodeKind::TextRun);
        tree.setText(t, {});
        expectTrue(reported(checkInvariants(tree), "文本.文本节点非空"),
                   "反例：空文本节点 → 报文本非空");
    }
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        const NodeId a = tree.appendChild(sec, NodeKind::TextRun);
        tree.setText(a, cps({0x61}));
        const NodeId b = tree.appendChild(sec, NodeKind::TextRun);
        tree.setText(b, cps({0x62}));
        tree.find(b)->id = a;
        expectTrue(reported(checkInvariants(tree), "身份.节点 ID 唯一且索引自洽"),
                   "反例：两个节点同 id → 报 ID 唯一与索引自洽");
    }
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        const NodeId a = tree.appendChild(sec, NodeKind::TextRun);
        tree.setText(a, cps({0x61}));
        tree.find(a)->kind = static_cast<NodeKind>(250);
        expectTrue(reported(checkInvariants(tree), "身份.节点种类在声明范围内"),
                   "反例：节点种类越界 → 报种类在声明范围内");
    }
    // 摘下的子树（eraseChild 的产物）是合法状态：节点留在池里等撤销搬回，
    // 不属于"孤立节点"。这条反过来说也很重要 —— 检查器不能把合法状态判成违规。
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
        tree.setText(tree.appendChild(cc, NodeKind::TextRun), cps({0x61}));
        const NodeId taken = tree.eraseChild(sec, 0);
        expectTrue(taken == cc && checkInvariants(tree).ok(),
                   "反例的对照：被摘下的子树留在池里不算孤立节点，检查器必须仍然全绿");
    }
}

}  // namespace

int main() {
    std::printf("=== 文档模型 0.1 / 0.2：位置换算与段落分段 ===\n");

    // 两种可见性口径各过一遍：位置空间不同，但"往返互逆"必须各自成立。
    const FlowVisibility kBoth[2] = {FlowVisibility::Design, FlowVisibility::Display};
    const char* kVisName[2] = {"编辑态", "呈现态"};
    for (int v = 0; v < 2; ++v) {
        const std::string pre = std::string(kVisName[v]) + " / ";
        std::printf("  --- 可见性口径：%s ---\n", kVisName[v]);
        checkTree(makeSingleParagraph(), kBoth[v], pre + "单段三码点");
        checkTree(makeTwoRuns(), kBoth[v], pre + "相邻两个 run");
        checkTree(makeEmptyControl(), kBoth[v], pre + "删空的控件");
        checkTree(makeControlAcrossMark(), kBoth[v], pre + "控件跨段落标记");
        checkTree(makeNestedControls(), kBoth[v], pre + "嵌套控件");
        checkTree(makeTable(), kBoth[v], pre + "表格");
        checkEmptyContainerBoundaries(kBoth[v]);
        for (uint32_t seed = 1; seed <= 3; ++seed) {
            checkTree(makeRandomTree(seed, 40), kBoth[v],
                      pre + "随机树 #" + std::to_string(seed));
        }
    }

    std::printf("  --- 隐藏：整棵子树从流里摘除 ---\n");
    checkHiddenSubtree();

    std::printf("  --- 0.2：每容器的标记下标数组与段落定位 ---\n");
    checkParaBoundaries();
    for (int v = 0; v < 2; ++v) {
        const std::string pre = std::string(kVisName[v]) + " / ";
        checkParaIndex(makeSingleParagraph(), kBoth[v], pre + "单段三码点");
        checkParaIndex(makeTwoRuns(), kBoth[v], pre + "相邻两个 run");
        checkParaIndex(makeEmptyControl(), kBoth[v], pre + "删空的控件");
        checkParaIndex(makeControlAcrossMark(), kBoth[v], pre + "控件跨段落标记");
        checkParaIndex(makeNestedControls(), kBoth[v], pre + "嵌套控件");
        checkParaIndex(makeTable(), kBoth[v], pre + "表格");
    }
    checkParaHidden();
    for (int v = 0; v < 2; ++v) {
        for (uint32_t seed = 1; seed <= 3; ++seed) {
            checkParaIncremental(seed, kBoth[v], kVisName[v]);
        }
    }

    std::printf("  --- 0.3：不变量检查器 ---\n");
    checkInvariantRuleSet();
    checkInvariantCounterexamples();
    expectClean(makeSingleParagraph(), "0.3 / 单段三码点");
    expectClean(makeTwoRuns(), "0.3 / 相邻两个 run");
    expectClean(makeEmptyControl(), "0.3 / 删空的控件");
    expectClean(makeControlAcrossMark(), "0.3 / 控件跨段落标记");
    expectClean(makeNestedControls(), "0.3 / 嵌套控件");
    expectClean(makeTable(), "0.3 / 表格");
    for (uint32_t seed = 1; seed <= 3; ++seed) {
        expectClean(makeRandomTree(seed, 40), "0.3 / 随机树 #" + std::to_string(seed));
    }
    // 隐藏形状：呈现态下整棵子树（含它内部的段落标记）被摘除，两种口径都要成立。
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61, 0x62}));
        const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
        tree.setText(tree.appendChild(cc, NodeKind::TextRun), cps({0x78, 0x79}));
        tree.appendChild(cc, NodeKind::ParagraphMark);
        tree.appendChild(sec, NodeKind::ParagraphMark);
        tree.find(cc)->hidden = true;
        expectClean(tree, "0.3 / 隐藏子树");
    }

    std::printf("\n共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
