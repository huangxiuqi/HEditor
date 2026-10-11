/**
 * 文档模型 0.1 / 0.2 / 0.3 / 0.4 / 0.5 的判据：位置换算、段落分段、不变量检查器、编译、长度。
 *
 * 这一层没法靠"看起来对"验收 —— 位置错一格，表象是光标跳、命中偏、撤销回错地方，
 * 全都要到界面才看得见。所以判据只落在数据层，五条可证伪的：
 *   0.1 流位置不丢：对每个流位置 f，flowAt(posAt(f)) == f；
 *       往返不改归属：对每个合法 Pos p，flowAt(posAt(flowAt(p))) == flowAt(p)。
 *       第二条这样写是为了绕开"同一位置有多种 Pos 写法"—— 容器的第 0 个槽位与第一个孩子的
 *       第一个位置本来就是同一个流位置，要求 Pos 逐字段相等会把正确实现判成错。
 *   0.2 下标数组与全部段落查询跟暴力算法逐项比对，并在随机增删中每步与重建结果整体比对。
 *   0.3 不变量检查器：对现有形状全绿，且逐条构造反例时必须报出对应的那条规则。
 *   0.4 编译：同一份内容走"现有 BlockSpec 路径"与"树 → 编译"两条路，结果逐字段一致；
 *       另加切段口径（标记切段、容器边界不切）、末尾未结束的段、隐藏摘除、不支持元素记诊断。
 *   0.5 长度：单位换算的两支往返（正向量化误差不超过半个 EMU、反向对整数倍精确）、
 *       五种性质的文本形式往返后种类与数值都不丢、畸形输入全被拒、非绝对长度在编译期被拒。
 *
 * 覆盖四层：写死的形状（嵌套、跨段落标记、空容器、表格、隐藏）、穷举、随机、以及增删序列，
 * 前两层在两种可见性口径下各过一遍（位置空间不同，但"往返互逆"必须各自成立）。
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "core/doc/compile.h"
#include "core/doc/flow.h"
#include "core/doc/invariants.h"
#include "core/doc/length.h"
#include "core/doc/node.h"
#include "core/doc/para.h"
#include "core/utils/units.h"
#include "demo/demo_documents.h"

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
void checkParagraphIndex(const NodeTree& tree, FlowVisibility vis, const std::string& label) {
    ParagraphIndex idx(tree, vis);

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
int diffIndexes(const NodeTree& tree, const ParagraphIndex& a, const ParagraphIndex& b) {
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
        ParagraphIndex idx(tree, FlowVisibility::Design);
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
        ParagraphIndex idx(tree, FlowVisibility::Design);
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
        ParagraphIndex idx(tree, FlowVisibility::Design);
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

    checkParagraphIndex(tree, FlowVisibility::Design, "隐藏与分段 / 编辑态");
    checkParagraphIndex(tree, FlowVisibility::Display, "隐藏与分段 / 呈现态");

    ParagraphIndex design(tree, FlowVisibility::Design);
    ParagraphIndex display(tree, FlowVisibility::Display);

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
    ParagraphIndex idx(tree, vis);
    checkParagraphIndex(tree, vis, pre + " 起始");

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

        if (diffIndexes(tree, idx, ParagraphIndex(tree, vis)) != 0) ++mismatch;
    }

    expectTrue(inserts > 20 && erases > 15,
               pre + "：随机序列真的做了增删（插入 " + std::to_string(inserts) + "、删除 " +
                   std::to_string(erases) + "）");
    expectTrue(bad_notify == 0, pre + "：每次通知的接受与拒绝都与记账范围一致");
    expectTrue(mismatch == 0, pre + "：每一步之后整张表都与重新构造的结果逐项相同");
    checkParagraphIndex(tree, vis, pre + " 终态");
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

/**
 * 0.4：把一份 BlockSpec 列表搬进树 —— 每段一个 TextRun 加一个结束标记，段落格式挂在标记上。
 *
 * 这是 0.4 的对照构造：同一份内容用"现有 BlockSpec 路径"与"树"两条路表达，再断言编译结果
 * 与前者逐字段一致。字段逐个搬而不是整体塞进节点，因为段落格式本来就该由样式/控件设置。
 */
NodeTree treeFromBlocks(const std::vector<BlockSpec>& blocks) {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    for (const BlockSpec& b : blocks) {
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), b.cp_text);
        const NodeId mark = tree.appendChild(sec, NodeKind::ParagraphMark);
        ParaFormat& f = tree.find(mark)->para;
        f.font_index = b.font_index;
        f.size = Length::abs(ptToEmu(b.size_pt));
        f.wm = b.wm;
        f.line_factor = b.line_factor;
        f.space_after = Length::abs(ptToEmu(b.space_after_pt));
        f.page_break_before = b.page_break_before;
        f.language = b.language;
        f.color = b.color;
        f.fallback_font_indices = b.fallback_font_indices;
    }
    return tree;
}

/**
 * 两份 BlockSpec 逐字段比。返回空串表示一致，否则返回第一处不同的字段名。
 *
 * 语种按内容比而不是比指针：两边指向的驻留字符串可能是不同字面量。
 * 现有 BlockSpec 没有 operator==，所以这里显式列出每个字段 —— 漏比一个字段，
 * 判据就变成一句空话。
 */
std::string diffBlock(const BlockSpec& a, const BlockSpec& b) {
    if (a.cp_text != b.cp_text) return "cp_text";
    if (a.font_index != b.font_index) return "font_index";
    if (a.size_pt != b.size_pt) return "size_pt";
    if (a.wm.inline_axis != b.wm.inline_axis || a.wm.block_flow != b.wm.block_flow ||
        a.wm.orientation != b.wm.orientation) {
        return "wm";
    }
    if (a.line_factor != b.line_factor) return "line_factor";
    if (a.space_after_pt != b.space_after_pt) return "space_after_pt";
    if (a.page_break_before != b.page_break_before) return "page_break_before";
    const std::string la = a.language != nullptr ? a.language : "";
    const std::string lb = b.language != nullptr ? b.language : "";
    if (la != lb) return "language";
    if (a.color.r != b.color.r || a.color.g != b.color.g || a.color.b != b.color.b ||
        a.color.a != b.color.a) {
        return "color";
    }
    if (a.fallback_font_indices != b.fallback_font_indices) return "fallback_font_indices";
    return "";
}

/**
 * 0.4 的核心判据：同一份内容走"现有 BlockSpec 路径"与"树 → 编译"两条路，结果逐字段一致。
 */
void checkCompileMatchesBlocks(const std::vector<BlockSpec>& blocks, const std::string& label) {
    const NodeTree tree = treeFromBlocks(blocks);
    const CompileResult r = compileDocument(tree, FlowVisibility::Design);
    expectTrue(r.ok(), label + "：只有文本与段落，编译无诊断");
    if (r.blocks.size() != blocks.size()) {
        expectTrue(false, label + "：段落数一致（参照 " + std::to_string(blocks.size()) +
                              "，编译 " + std::to_string(r.blocks.size()) + "）");
        return;
    }
    int diff = 0;
    std::string first;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const std::string d = diffBlock(blocks[i], r.blocks[i]);
        if (!d.empty()) {
            ++diff;
            if (first.empty()) first = "第 " + std::to_string(i + 1) + " 段差在 " + d;
        }
    }
    std::string msg = label + "：" + std::to_string(blocks.size()) + " 段逐字段一致（不一致 " +
                      std::to_string(diff) + "）";
    if (!first.empty()) msg += "，首处 " + first;
    expectTrue(diff == 0, msg);
    expectClean(tree, label + " / 构造出的树");
}

/**
 * 0.4 的形状判据：切段口径、末尾未结束的段、跨容器边界、隐藏、不支持元素。
 */
void checkCompileShapes() {
    // 只有标记：标记属于它结束的那一段，所以这里是一段空段落。
    {
        NodeTree tree;
        tree.appendChild(tree.firstSectionId(), NodeKind::ParagraphMark);
        const CompileResult r = compileDocument(tree, FlowVisibility::Design);
        expectTrue(r.ok() && r.blocks.size() == 1 && r.blocks[0].cp_text.empty(),
                   "0.4 / 只有标记：编译出一段空段落");
    }
    // 空节：没有标记也没有内容，0 段。
    {
        NodeTree tree;
        const CompileResult r = compileDocument(tree, FlowVisibility::Design);
        expectTrue(r.ok() && r.blocks.empty(), "0.4 / 空节：0 段");
    }
    // 相邻两个 run 拼在同一段；末尾还没有结束标记的那一段照落，用默认段落格式。
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61, 0x62}));
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x63}));
        tree.appendChild(sec, NodeKind::ParagraphMark);
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x64}));
        const CompileResult r = compileDocument(tree, FlowVisibility::Design);
        expectTrue(r.ok() && r.blocks.size() == 2 &&
                       r.blocks[0].cp_text == cps({0x61, 0x62, 0x63}) &&
                       r.blocks[1].cp_text == cps({0x64}),
                   "0.4 / 相邻两个 run 拼成一段，末尾未结束的一段照落");
        expectTrue(r.blocks.size() == 2 && r.blocks[1].size_pt == 12.0f &&
                       r.blocks[1].line_factor == 1.6f && r.blocks[1].language == nullptr,
                   "0.4 / 末尾未结束的一段用默认段落格式");
    }
    // 控件跨段落标记：切段只看段落标记，容器边界不是切分点 —— 于是切出两段。
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61}));
        const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
        tree.setText(tree.appendChild(cc, NodeKind::TextRun), cps({0x78}));
        tree.appendChild(cc, NodeKind::ParagraphMark);
        tree.setText(tree.appendChild(cc, NodeKind::TextRun), cps({0x79}));
        tree.appendChild(sec, NodeKind::ParagraphMark);
        const CompileResult r = compileDocument(tree, FlowVisibility::Design);
        expectTrue(r.blocks.size() == 2 && r.blocks[0].cp_text == cps({0x61, 0x78}) &&
                       r.blocks[1].cp_text == cps({0x79}),
                   "0.4 / 控件跨段落标记：按流切出两段（容器边界不切段）");
        expectTrue(!r.ok() && r.diagnostics.size() == 1 &&
                       r.diagnostics[0].kind == NodeKind::ContentControl,
                   "0.4 / 控件跨段落标记：控件本身未编译，记一条诊断");
    }
    // 不支持的元素：图片叶子与容器各记一条，容器内部文本仍按流顺序并入当前段。
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61}));
        tree.appendChild(sec, NodeKind::Image);
        const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
        tree.setText(tree.appendChild(cc, NodeKind::TextRun), cps({0x62}));
        tree.appendChild(sec, NodeKind::ParagraphMark);
        const CompileResult r = compileDocument(tree, FlowVisibility::Design);
        expectTrue(r.blocks.size() == 1 && r.blocks[0].cp_text == cps({0x61, 0x62}),
                   "0.4 / 不支持的元素：容器内部文本仍按流顺序并入当前段");
        expectTrue(r.diagnostics.size() == 2 && r.diagnostics[0].kind == NodeKind::Image &&
                       r.diagnostics[1].kind == NodeKind::ContentControl,
                   "0.4 / 不支持的元素：图片与控件各记一条诊断（不含其内部）");
        expectTrue(!r.ok(), "0.4 / 不支持的元素：有诊断时 ok() 为假");
    }
    // 隐藏：编辑态照常参与，呈现态整棵摘除 —— 与 FlowIndex / ParagraphIndex 同一口径。
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61}));
        const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
        tree.setText(tree.appendChild(cc, NodeKind::TextRun), cps({0x78}));
        tree.find(cc)->hidden = true;
        tree.appendChild(sec, NodeKind::ParagraphMark);
        const CompileResult design = compileDocument(tree, FlowVisibility::Design);
        const CompileResult display = compileDocument(tree, FlowVisibility::Display);
        expectTrue(design.blocks.size() == 1 && design.blocks[0].cp_text == cps({0x61, 0x78}),
                   "0.4 / 隐藏：编辑态下隐藏控件的内容照常参与编译");
        expectTrue(display.blocks.size() == 1 && display.blocks[0].cp_text == cps({0x61}) &&
                       display.diagnostics.empty(),
                   "0.4 / 隐藏：呈现态下整棵子树摘除（连诊断都不产生）");
    }
}

/**
 * 0.5：单位换算往返一致。
 *
 * 判据分两支，缺一支都不成立：
 *   正向 —— "数值 → EMU → 数值"的量化误差不超过半个 EMU（在 EMU 网格上的值精确相等）；
 *   反向 —— "整数倍 EMU → 数值 → EMU"必须原样回来。
 * 只测正向会漏掉截断与四舍五入的差别（两者都在半个 EMU 内），只测反向会漏掉量化方向。
 * 另外七个单位还要各自验一支"文档里写死的定值"，免得表抄错。
 */
void checkUnitRoundTrip() {
    const Unit units[] = {Unit::Pt, Unit::HalfPt, Unit::Twip,
                          Unit::Px, Unit::Mm,     Unit::Cm, Unit::Inch};
    // 覆盖实际会用到的量级：字号、行距、线宽、页边距、纸张（A4 / Letter）。
    const double values[] = {0.5,  0.75, 1.0,  2.0,  8.5,
                             10.0, 11.0, 12.0, 12.5, 21.0,
                             24.0, 29.7, 56.7};
    int cases = 0;
    int over_quantum = 0;
    int bad_backward = 0;
    for (Unit u : units) {
        for (double v : values) {
            ++cases;
            const double back = fromEmu(toEmu(v, u), u);
            if (std::fabs(back - v) * static_cast<double>(emuPerUnit(u)) > 0.5 + 1e-9) {
                ++over_quantum;
            }
        }
        // 反向：整数倍的 EMU 必须原样回来，正负各走一遍（四舍五入对负数的方向要一致）。
        for (int k = -3; k <= 2000; k += 7) {
            const Emu e = static_cast<Emu>(k) * emuPerUnit(u);
            if (toEmu(fromEmu(e, u), u) != e) ++bad_backward;
        }
    }
    expectTrue(over_quantum == 0, "0.5 / 单位换算：正向 " + std::to_string(cases) +
                                      " 组取值的量化误差都在半个 EMU 以内（越界 " +
                                      std::to_string(over_quantum) + "）");
    expectTrue(bad_backward == 0, "0.5 / 单位换算：反向对整数倍 EMU 全部原样（不一致 " +
                                      std::to_string(bad_backward) + "）");

    int non_positive = 0;
    for (Unit u : units) {
        if (emuPerUnit(u) <= 0) ++non_positive;
    }
    expectTrue(non_positive == 0, "0.5 / 单位换算：七个单位的 EMU 因子都是正整数");
    expectTrue(ptToEmu(12.0) == 152400 && cmToEmu(2.54) == 914400 && mmToEmu(1.0) == 36000 &&
                   halfPtToEmu(24) == 152400 && halfPtToEmu(21) == ptToEmu(10.5),
               "0.5 / 单位换算：定值核对（12 磅 = 152400、2.54 厘米 = 914400、"
               "小四 = 半磅 24 = 152400、五号 = 半磅 21 = 10.5 磅）");
}

/**
 * 0.5 的核心判据：长度类型经文本形式往返之后，种类与数值都不丢。
 *
 * "不丢"要两头都验：数值相同是基础，种类相同才是这一条真正防的东西 ——
 * 把五种性质压成一个数的实现，数值可能照样对，种类却塌了。
 */
void checkLengthRoundTrip() {
    const Length cases[] = {
        Length::automatic(),
        Length::abs(0),
        Length::abs(914400),
        Length::abs(-360000),
        Length::percent(1, 2),
        Length::percent(50, 100),
        Length::percent(-1, 3),
        Length::percent(0, 7),
        Length::em(1.6),
        Length::em(2.0),
        Length::chars(3.0),
        Length::chars(0.5),
    };
    int bad = 0;
    int kind_lost = 0;
    std::string first;
    for (const Length& len : cases) {
        Length back;
        const std::string text = printLength(len);
        if (!parseLength(text, &back)) {
            ++bad;
            if (first.empty()) first = text + " 解析失败";
            continue;
        }
        if (back != len) {
            ++bad;
            if (first.empty()) first = text + " → " + printLength(back);
        }
        if (back.kind != len.kind) ++kind_lost;
    }
    expectTrue(bad == 0, "0.5 / 长度：五种性质的文本形式往返都原样（" +
                             std::to_string(sizeof(cases) / sizeof(cases[0])) + " 例，不一致 " +
                             std::to_string(bad) + (first.empty() ? "" : "，首处 " + first) + "）");
    expectTrue(kind_lost == 0, "0.5 / 长度：往返之后种类不变（没有塌成一个数）");
}

/**
 * 0.5：文本形式的形状与约定的定值。约分是约定的一部分 —— 50/100 与 1/2 是同一个长度、
 * 同一个文本形式，否则相等比较与落盘都不唯一。
 */
void checkLengthShape() {
    expectTrue(Length::percent(50, 100) == Length::percent(1, 2) &&
                   printLength(Length::percent(50, 100)) == "pct:1/2",
               "0.5 / 长度：比例按 gcd 约分，文本形式唯一");
    expectTrue(printLength(Length::abs(914400)) == "abs:914400" &&
                   printLength(Length::em(1.6)) == "em:8/5" &&
                   printLength(Length::em(2.0)) == "em:2" &&
                   printLength(Length::chars(3.0)) == "chars:3" &&
                   printLength(Length::chars(0.5)) == "chars:1/2" &&
                   printLength(Length::percent(-1, 3)) == "pct:-1/3" &&
                   printLength(Length::automatic()) == "auto",
               "0.5 / 长度：文本形式的定值（分母为 1 时不写斜杠，负号归到分子）");
    expectTrue(Length::em(2.0) != Length::chars(2.0) && Length::abs(1) != Length::automatic() &&
                   Length::percent(1, 2) != Length::abs(1),
               "0.5 / 长度：种类参与相等比较（类型不是注释）");
    // 分母为 0 不是可表达的比例：返回自动值，而不是拿 0 做除法或猜一个分母。
    expectTrue(Length::percent(1, 0).kind == LengthKind::Auto,
               "0.5 / 长度：分母为 0 → 自动值（没有数值可表达）");
}

/**
 * 0.5：文本形式必须严格。静默纠正（跳过空白、截断、夹取、把 1.5 读成 1）会把
 * "文件坏了"变成"读出来一个别的值"，那比报错难查得多。
 */
void checkLengthParseRejects() {
    static const char* kBad[] = {
        "",      "auto ", "Auto",   "abs:",  "abs:1e3", "abs:1.5", "abs:+1", "abs:1 2",
        "pct:",  "pct:1/", "pct:/2", "pct:1/0", "pct:1/-2", "pct:1/2/3", "pct:abc",
        "em:",   "em:1/",  "chars:1/2/3", "pt:1", "1/2", "  auto", "abs:99999999999999999999999",
    };
    const int total = static_cast<int>(sizeof(kBad) / sizeof(kBad[0]));
    int accepted = 0;
    for (const char* bad : kBad) {
        Length out;
        if (parseLength(bad, &out)) ++accepted;
    }
    expectTrue(accepted == 0, "0.5 / 长度：畸形输入全部被拒（" + std::to_string(total) +
                                  " 例，放过 " + std::to_string(accepted) + " 例）");
    expectTrue(!parseLength("auto", nullptr), "0.5 / 长度：输出参数为空时被拒，不解引用空指针");
}

/**
 * 0.5：长度进入编译的那一条路。段落格式里的字号与段后距必须是绝对长度 ——
 * 相对字号算不出磅值，静默取零会让一整段文字凭空没了字号。
 */
void checkCompileRejectsRelativeLength() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61}));
    const NodeId mark = tree.appendChild(sec, NodeKind::ParagraphMark);
    tree.find(mark)->para.size = Length::em(2.0);

    const CompileResult r = compileDocument(tree, FlowVisibility::Design);
    expectTrue(!r.ok() && r.diagnostics.size() == 1 &&
                   r.diagnostics[0].kind == NodeKind::ParagraphMark &&
                   r.diagnostics[0].node == mark,
               "0.5 / 编译：非绝对的段落字号被拒，记一条诊断");

    // 同一段的段后距用相对单位，也要各自记一条 —— 两个字段分头检查。
    tree.find(mark)->para.size = Length::abs(ptToEmu(12.0));
    tree.find(mark)->para.space_after = Length::percent(50, 100);
    const CompileResult r2 = compileDocument(tree, FlowVisibility::Design);
    expectTrue(!r2.ok() && r2.diagnostics.size() == 1,
               "0.5 / 编译：非绝对的段后距被拒，且只报这一条");
}

}  // namespace

int main() {
    std::printf("=== 文档模型 0.1 ~ 0.5：位置、分段、不变量、编译、长度 ===\n");

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
        checkParagraphIndex(makeSingleParagraph(), kBoth[v], pre + "单段三码点");
        checkParagraphIndex(makeTwoRuns(), kBoth[v], pre + "相邻两个 run");
        checkParagraphIndex(makeEmptyControl(), kBoth[v], pre + "删空的控件");
        checkParagraphIndex(makeControlAcrossMark(), kBoth[v], pre + "控件跨段落标记");
        checkParagraphIndex(makeNestedControls(), kBoth[v], pre + "嵌套控件");
        checkParagraphIndex(makeTable(), kBoth[v], pre + "表格");
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

    std::printf("  --- 0.4：编译到引擎输入 ---\n");
    checkCompileShapes();
    checkCompileMatchesBlocks(sampleDocument(0, 1, 2), "0.4 / 演示文书 sampleDocument");
    checkCompileMatchesBlocks({mixedParagraphBlock(0, 1)}, "0.4 / 中英文混排段");

    std::printf("  --- 0.5：长度类型与 EMU 定点表示 ---\n");
    checkUnitRoundTrip();
    checkLengthRoundTrip();
    checkLengthShape();
    checkLengthParseRejects();
    checkCompileRejectsRelativeLength();

    std::printf("\n共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
