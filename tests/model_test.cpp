/**
 * 文档模型 0.1 ~ 1.6 的判据。这一层靠"看起来对"验收不了（位置错一格，表象是光标跳、命中偏、
 * 撤销回错地方，全都要到界面才看得见），所以判据只落在数据层，每条都可证伪：
 *   0.1 流位置不丢（flowAt(posAt(f)) == f）与往返不改归属（同一流位置有多种 Pos 写法，不要求逐字段相等）。
 *   0.2 下标数组与全部段落查询跟暴力算法逐项比对，随机增删中每步与重建结果整体比对。
 *   0.3 不变量检查器：现有形状全绿，且逐条构造反例时必须报出对应的那条规则。
 *   0.4 编译：同一份内容走"现有 BlockSpec 路径"与"树 → 编译"两条路逐字段一致。
 *   0.5 长度：单位换算的两支往返、文本形式往返后类型与数值都不丢、畸形输入全被拒。
 *   1.1 规范化：幂等（第二遍零改动且快照逐条相同）、跨容器不合并、空容器不清除。
 *   1.2 步骤：应用加逆操作回到原样、取删插等于原状、映射往返一致、整棵子树增删后带身份的快照相同。
 *   1.3 撤销：连撤逐次回到对应步骤的状态、重做逐次回到编辑后、连续输入并成一个撤销单位。
 *   1.4 落点判定：四个边界位置互不相同且 posAt 单射；可编辑集合取交集；方向键不跳出集合；
 *       Tab 只落可编辑控件、无下一个时放行；越界编辑被拒且给原因；结构操作默认被拒。
 *   1.5 控件与宿主 API：索引与重建逐项相同、批量设值一个撤销步骤、撤销后逐字段回到原样；
 *       属性侧的值与文本类共用一条 API 但落点不同；外观逐项读回且类型不丢；异步裁决门在
 *       等待期间不动文档与撤销栈、Allow 才应用、Deny 与超时保持原值并标 invalid。
 *   1.6 样式：三层解析与手算逐字段一致；成环与未知样式名被诊断且不把整篇拖死；缓存按元素加
 *       样式表版本命中，改版整体失效、单元素可单独失效；样式引用随子树与片段一起走。
 * 覆盖四层：写死的形状、穷举、随机、增删序列；前两层在两种可见性口径下各过一遍。
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "core/doc/compile.h"
#include "core/doc/control_api.h"
#include "core/doc/control_index.h"
#include "core/doc/edit_capability.h"
#include "core/doc/flow.h"
#include "core/doc/fragment.h"
#include "core/doc/invariants.h"
#include "core/doc/length.h"
#include "core/doc/node.h"
#include "core/doc/normalize.h"
#include "core/doc/para.h"
#include "core/doc/range.h"
#include "core/doc/steps.h"
#include "core/doc/style.h"
#include "core/doc/transaction.h"
#include "core/doc/validation.h"
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
 *
 * 两个 run 的标记刻意不同：同标记的相邻文本节点是未规范形状（1.1），而这份形状要能在
 * 不变量检查下全绿。
 */
NodeTree makeTwoRuns() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61, 0x62}));
    const NodeId b = tree.appendChild(sec, NodeKind::TextRun);
    tree.setText(b, cps({0x63, 0x64}));
    tree.find(b)->marks.bold = true;
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
    // 0.3："任何编辑操作之后都要重新满足"，而规范化在事务提交前跑一次（1.1）——
    // 所以先规范化，再按"已提交文档"的口径检查。
    const NormalizeResult norm = normalizeDocument(tree);
    expectClean(tree, pre + " 增删之后的不变量（规范化：并 " + std::to_string(norm.merged) +
                        "、清 " + std::to_string(norm.removed) + "）");
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
        "文本.文本节点非空",         "文本.相邻同标记的 run 必须合并",
        "容器.边界成对有序不交叉",   "容器.切段无缝隙",
        "身份.节点 ID 唯一且索引自洽",
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
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61}));
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x62}));
        expectTrue(reported(checkInvariants(tree), "文本.相邻同标记的 run 必须合并"),
                   "反例：相邻两个同标记的文本节点 → 报相邻同标记必须合并");
    }
    // 对照：不同标记的两个 run 加一个空 run，规范化之后这条规则必须安静 ——
    // 否则它就成了"永远报警"。
    {
        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61}));
        const NodeId b = tree.appendChild(sec, NodeKind::TextRun);
        tree.setText(b, cps({0x62}));
        tree.find(b)->marks.bold = true;
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), {});
        normalizeDocument(tree);
        expectTrue(checkInvariants(tree).ok() && tree.find(sec)->children.size() == 2,
                   "反例的对照：不同标记的两个 run 加一个空 run，规范化之后全绿且只剩两个");
    }
}

/**
 * 0.4：把一份 BlockSpec 列表搬进树 —— 每段一个 TextRun 加一个结束标记，段落格式挂在标记上。
 *
 * 这是 0.4 的对照构造：同一份内容用"现有 BlockSpec 路径"与"树"两条路表达，再断言编译结果
 * 与前者逐字段一致。字段逐个搬而不是整体塞进节点，因为段落格式本来就该由样式/控件设置。
 */
/**
 * 没有样式的样式表。它的文档默认值就是各字段的默认值，所以"三层解析"的结果与"完全没有样式"
 * 一致 —— 0.4 那条"两条路逐字段一致"的判据因此可以在不带样式的前提下继续成立。
 */
const StyleSheet& noStyles() {
    static const StyleSheet sheet;
    return sheet;
}

/**
 * 段落内联覆盖的文本形式，用于快照比对。未设置的字段写成 -：三态里的"没写"与"写成默认值"
 * 是两种形状，快照必须能分开，否则"覆盖被误删"这类改动会被判成相同。
 */
std::string paraKey(const ParaFormatPatch& p) {
    std::string s = p.font_index.has_value() ? std::to_string(*p.font_index) : "-";
    s += "," + (p.size.has_value() ? printLength(*p.size) : std::string("-"));
    s += "," + (p.wm.has_value() ? std::string("wm") : std::string("-"));
    s += "," + (p.line_factor.has_value() ? std::to_string(*p.line_factor) : std::string("-"));
    s += "," + (p.space_after.has_value() ? printLength(*p.space_after) : std::string("-"));
    s += "," + (p.page_break_before.has_value() ? (*p.page_break_before ? "1" : "0")
                                                : std::string("-"));
    s += "," + (p.language.has_value() ? std::string("lang") : std::string("-"));
    s += "," + (p.color.has_value() ? std::string("col") : std::string("-"));
    s += "," + (p.fallback_font_indices.has_value() ? std::string("fb") : std::string("-"));
    return s;
}

NodeTree treeFromBlocks(const std::vector<BlockSpec>& blocks) {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    for (const BlockSpec& b : blocks) {
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), b.cp_text);
        const NodeId mark = tree.appendChild(sec, NodeKind::ParagraphMark);
        ParaFormatPatch& f = tree.find(mark)->para;
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
    const CompileResult r = compileDocument(tree, FlowVisibility::Design, noStyles());
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
        const CompileResult r = compileDocument(tree, FlowVisibility::Design, noStyles());
        expectTrue(r.ok() && r.blocks.size() == 1 && r.blocks[0].cp_text.empty(),
                   "0.4 / 只有标记：编译出一段空段落");
    }
    // 空节：没有标记也没有内容，0 段。
    {
        NodeTree tree;
        const CompileResult r = compileDocument(tree, FlowVisibility::Design, noStyles());
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
        const CompileResult r = compileDocument(tree, FlowVisibility::Design, noStyles());
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
        const CompileResult r = compileDocument(tree, FlowVisibility::Design, noStyles());
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
        const CompileResult r = compileDocument(tree, FlowVisibility::Design, noStyles());
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
        const CompileResult design = compileDocument(tree, FlowVisibility::Design, noStyles());
        const CompileResult display = compileDocument(tree, FlowVisibility::Display, noStyles());
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

    const CompileResult r = compileDocument(tree, FlowVisibility::Design, noStyles());
    expectTrue(!r.ok() && r.diagnostics.size() == 1 &&
                   r.diagnostics[0].kind == NodeKind::ParagraphMark &&
                   r.diagnostics[0].node == mark,
               "0.5 / 编译：非绝对的段落字号被拒，记一条诊断");

    // 同一段的段后距用相对单位，也要各自记一条 —— 两个字段分头检查。
    tree.find(mark)->para.size = Length::abs(ptToEmu(12.0));
    tree.find(mark)->para.space_after = Length::percent(50, 100);
    const CompileResult r2 = compileDocument(tree, FlowVisibility::Design, noStyles());
    expectTrue(!r2.ok() && r2.diagnostics.size() == 1,
               "0.5 / 编译：非绝对的段后距被拒，且只报这一条");
}

/**
 * 1.1：把一份字符属性集合压成可比的字符串。
 *
 * 快照必须含"会被规范化改动的一切"，否则"跑两遍结果相同"可能因为改动被漏比而假绿。
 * 未设置报 0、显式设成假报 1、设成真报 2 —— 三种状态要能分开，否则"继承"与"显式关掉"
 * 会被当成同一件事。
 */
std::string marksKey(const CharFormat& m) {
    std::string s;
    s += std::to_string(m.bold.has_value() ? (m.bold.value() ? 2 : 1) : 0);
    s += std::to_string(m.italic.has_value() ? (m.italic.value() ? 2 : 1) : 0);
    s += std::to_string(m.underline.has_value() ? (m.underline.value() ? 2 : 1) : 0);
    s += "|" + std::to_string(m.font_index.value_or(-1));
    s += "|" + (m.size.has_value() ? printLength(*m.size) : std::string("-"));
    s += "|" + (m.color.has_value() ? std::to_string(m.color->r) + "," +
                                         std::to_string(m.color->g) + "," +
                                         std::to_string(m.color->b)
                                   : std::string("-"));
    return s;
}

/**
 * 结构化快照：前序走一遍，每个节点一条记录。拿它比"跑两遍规范化结果相同"、
 * 以及"步骤往返之后文档回到原样" —— 只比改动读数是不够的，读数漏报一类改动时判据就成了空话。
 *
 * 记录含码点内容本身，否则"文本被换成另一段同样长的文本"这种错比不出来。
 * 不含节点 ID：文本节点的身份不参与往返判据（区间边界落在文本中间时原节点被切开，
 * 插回时是新的节点）。
 */
void snapshotInto(const NodeTree& tree, NodeId id, std::vector<std::string>* out) {
    const Node* n = tree.find(id);
    std::string s = nodeKindName(n->kind);
    s += "|t=";
    for (uint32_t cp : n->text) s += std::to_string(cp) + ",";
    s += "|m=" + marksKey(n->marks);
    s += "|p=" + paraKey(n->para);
    s += "|s=" + n->styleRef;
    s += "|h=" + std::to_string(n->hidden ? 1 : 0);
    s += "|kids=" + std::to_string(n->children.size());
    out->push_back(std::move(s));
    for (const Node* c : n->children) snapshotInto(tree, c->id, out);
}

std::vector<std::string> shapeSnapshot(const NodeTree& tree) {
    std::vector<std::string> out;
    snapshotInto(tree, tree.rootId(), &out);
    return out;
}

/**
 * 两个快照的第一处不同；全同返回 -1。
 */
int firstSnapshotDiff(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return static_cast<int>(i);
    }
    return a.size() == b.size() ? -1 : static_cast<int>(n);
}

/**
 * 两个快照的差异说明：相同返回空串，不同返回"第几条、两边分别是什么"。
 */
std::string snapshotDiffText(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    const int i = firstSnapshotDiff(a, b);
    if (i < 0) return "";
    const size_t k = static_cast<size_t>(i);
    const std::string left = k < a.size() ? a[k] : std::string("（没有）");
    const std::string right = k < b.size() ? b[k] : std::string("（没有）");
    return "第 " + std::to_string(i) + " 条不同：" + left + " → " + right;
}

/**
 * 带身份的快照：每行以节点 id 开头。节点级步骤的往返判据要用它 —— 光比内容的话，
 * "撤销时重造了一棵内容一样的子树"也会被当成通过，而那个节点的身份已经换了。
 */
void snapshotIdsInto(const NodeTree& tree, NodeId id, std::vector<std::string>* out) {
    const Node* n = tree.find(id);
    std::string s = std::to_string(n->id) + "|" + nodeKindName(n->kind);
    s += "|t=" + std::to_string(n->text.size());
    s += "|m=" + marksKey(n->marks);
    s += "|p=" + paraKey(n->para);
    s += "|s=" + n->styleRef;
    s += "|c=" + n->control.key + "," + n->control.controlType + "," +
         (n->control.readonly ? "ro" : "-") + "," + (n->control.structureLock ? "lock" : "open");
    s += "|h=" + std::string(n->hidden ? "1" : "0");
    s += "|kids=" + std::to_string(n->children.size());
    out->push_back(std::move(s));
    for (const Node* c : n->children) snapshotIdsInto(tree, c->id, out);
}

std::vector<std::string> nodeIdsSnapshot(const NodeTree& tree) {
    std::vector<std::string> out;
    snapshotIdsInto(tree, tree.rootId(), &out);
    return out;
}

/**
 * 一棵新建的控件子树：控件（key=new_field）里一个码点的文本。
 */
NodeSpec makeFreshControlSpec() {
    NodeSpec cc;
    cc.kind = NodeKind::ContentControl;
    cc.control.key = "new_field";
    cc.control.controlType = "text";
    NodeSpec text;
    text.kind = NodeKind::TextRun;
    text.text = cps({0x41});
    cc.children.push_back(text);
    return cc;
}

/**
 * 刻意未规范的树：相邻同标记、夹着空 run、标记不同的 run、嵌套容器、空容器、隐藏子树。
 */
NodeTree makeMessyTree() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61}));
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x62}));
    tree.appendChild(sec, NodeKind::TextRun);  // 空 run
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x63}));
    const NodeId italic = tree.appendChild(sec, NodeKind::TextRun);
    tree.setText(italic, cps({0x64}));
    tree.find(italic)->marks.italic = true;
    tree.appendChild(sec, NodeKind::ParagraphMark);

    const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
    tree.setText(tree.appendChild(cc, NodeKind::TextRun), cps({0x65}));
    const NodeId f = tree.appendChild(cc, NodeKind::TextRun);
    tree.setText(f, cps({0x66}));
    tree.find(f)->marks.bold = true;

    tree.appendChild(sec, NodeKind::ContentControl);  // 空容器
    tree.appendChild(sec, NodeKind::ParagraphMark);

    const NodeId hidden = tree.appendChild(sec, NodeKind::ContentControl);
    tree.find(hidden)->hidden = true;
    tree.setText(tree.appendChild(hidden, NodeKind::TextRun), cps({0x78}));
    tree.setText(tree.appendChild(hidden, NodeKind::TextRun), cps({0x79}));
    return tree;
}

/**
 * 1.1：规范化的形状判据。两条边界刻意不越界 —— 跨容器不合并、空容器不清除。
 */
void checkNormalizeShapes() {
    NodeTree tree = makeMessyTree();
    const NodeId sec = tree.firstSectionId();
    const NormalizeResult r = normalizeDocument(tree);
    const Node* s = tree.find(sec);

    expectTrue(r.merged == 3 && r.removed == 1,
               "1.1 / 形状：同标记三连并成一个、跨容器的一对并成一个（并 " +
                   std::to_string(r.merged) + "）、清掉一个空 run（清 " +
                   std::to_string(r.removed) + "）");
    expectTrue(s->children.size() == 7, "1.1 / 形状：Section 下剩 7 个孩子（原 10 个）");
    expectTrue(s->children.size() == 7 && isText(s->children[0]->kind) &&
                   s->children[0]->text == cps({0x61, 0x62, 0x63}) &&
                   s->children[0]->marks.empty(),
               "1.1 / 形状：第一个孩子是合并后的 abc，且标记仍是空的");
    expectTrue(s->children.size() == 7 && s->children[1]->marks.italic.value_or(false),
               "1.1 / 形状：标记不同的 run 不被合并（第二个仍是 italic 的那一个）");
    // 空容器不清除：它可能是"作者删空了内容的控件"，有身份、有属性，与空 run 不是一类。
    expectTrue(s->children.size() == 7 && isContainer(s->children[4]->kind) &&
                   s->children[4]->children.empty(),
               "1.1 / 形状：空容器保留（与空 run 相反）");
    expectTrue(s->children.size() == 7 && s->children[3]->children.size() == 2,
               "1.1 / 形状：控件内部标记不同的一对不合并");
    // 隐藏子树照样规范化：规范化是文档自身的性质，与呈现口径无关。
    expectTrue(s->children.size() == 7 && s->children[6]->hidden &&
                   s->children[6]->children.size() == 1 &&
                   s->children[6]->children[0]->text == cps({0x78, 0x79}),
               "1.1 / 形状：隐藏子树内部也合并（xy），节点本身保留");
    expectClean(tree, "1.1 / 规范化之后的树");
}

/**
 * 1.1 的核心判据：幂等。
 *
 * 两支都要验：跑第二遍的改动读数为 0（它自己认为无事可做），以及结构化快照逐条相同
 * （它确实什么都没改）。前者漏掉"改了却没计数"，后者漏不掉，但读数更能定位问题。
 */
void checkNormalizeIdempotent() {
    NodeTree tree = makeMessyTree();
    const NormalizeResult first = normalizeDocument(tree);
    const std::vector<std::string> snapshot1 = shapeSnapshot(tree);
    const NormalizeResult second = normalizeDocument(tree);
    const std::vector<std::string> snapshot2 = shapeSnapshot(tree);

    expectTrue(first.changed() && first.merged == 3 && first.removed == 1,
               "1.1 / 幂等：第一遍确实有改动（并 " + std::to_string(first.merged) + "、清 " +
                   std::to_string(first.removed) + "）");
    expectTrue(!second.changed(), "1.1 / 幂等：第二遍改动读数为 0（并 " +
                                      std::to_string(second.merged) + "、清 " +
                                      std::to_string(second.removed) + "）");
    const int diff = firstSnapshotDiff(snapshot1, snapshot2);
    expectTrue(diff < 0, "1.1 / 幂等：结构化快照逐条相同（" + std::to_string(snapshot1.size()) +
                             " 个节点" +
                             (diff < 0 ? "" : "，首处第 " + std::to_string(diff) + " 条") + "）");
    expectTrue(shapeSnapshot(tree).size() == snapshot1.size(),
               "1.1 / 幂等：规范形状是唯一的（节点数不再变）");
}

/**
 * 1.1：规范化只改形状，不改内容。判据落在编译产物上 —— 它是"文本 + 段落格式"的完整取值，
 * 比逐段核对码点更全面，也正好复用 0.4 的逐字段比较。
 */
void checkNormalizeKeepsContent() {
    NodeTree tree = makeMessyTree();
    const CompileResult before = compileDocument(tree, FlowVisibility::Design, noStyles());
    const NormalizeResult r = normalizeDocument(tree);
    const CompileResult after = compileDocument(tree, FlowVisibility::Design, noStyles());

    int diff = 0;
    std::string first;
    if (before.blocks.size() != after.blocks.size()) {
        diff = 1;
    } else {
        for (size_t i = 0; i < before.blocks.size(); ++i) {
            const std::string d = diffBlock(before.blocks[i], after.blocks[i]);
            if (!d.empty()) {
                ++diff;
                if (first.empty()) first = "第 " + std::to_string(i + 1) + " 段差在 " + d;
            }
        }
    }
    std::string msg = "1.1 / 规范化不改内容：编译产物逐字段不变（" +
                      std::to_string(after.blocks.size()) + " 段，不一致 " + std::to_string(diff) +
                      "）";
    if (!first.empty()) msg += "，首处 " + first;
    expectTrue(r.changed() && diff == 0, msg);
}

/**
 * 1.2 的编辑用文档：两段，第二段里嵌着一个控件（用来验端点外扩）。
 *
 * Section → TextRun("abcdef"), ParagraphMark, TextRun("xy"),
 *           ContentControl[TextRun("cd", italic)], ParagraphMark
 *
 * 流坐标：TextRun("abcdef") 占 [1,7)、标记 [7,8)、TextRun("xy") 占 [8,10)、
 * 控件占 [10,14)（起始边界 10、内容 [11,13)、结束边界 13）、标记 [14,15)，总数 16。
 */
NodeTree makeEditTree() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61, 0x62, 0x63, 0x64, 0x65, 0x66}));
    const NodeId pm = tree.appendChild(sec, NodeKind::ParagraphMark);
    // 段落格式刻意不给默认值：这样"段落标记把格式一起带出来"才是一条真的断言。
    tree.find(pm)->para.font_index = 3;
    tree.find(pm)->para.size = Length::abs(ptToEmu(14.0));
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x78, 0x79}));
    const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
    const NodeId inner = tree.appendChild(cc, NodeKind::TextRun);
    tree.setText(inner, cps({0x63, 0x64}));
    tree.find(inner)->marks.italic = true;
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

/**
 * 1.2：端点规范化。落在不可半分单位内部的一端向外扩到那个单位的边界。
 */
void checkRangeNormalize() {
    const NodeTree tree = makeEditTree();

    // 两端都落在控件内部（[11,13) 是控件里的那两个码点）→ 各自扩到控件的边界。
    expectTrue(normalizeRange(tree, Range{11, 13}) == Range{10, 14},
               "1.2 / 端点外扩：两端都在控件内部时各扩到控件边界");
    // 只有起点在里面：起点收到控件起点，终点不动。
    expectTrue(normalizeRange(tree, Range{11, 15}) == Range{10, 15},
               "1.2 / 端点外扩：只有起点在控件内部时只收起点");
    // 反向的区间（anchor 在右）：扩的是对应那一端，方向不变。
    expectTrue(normalizeRange(tree, Range{13, 11}) == Range{14, 10},
               "1.2 / 端点外扩：反向区间同样按角色扩，方向保留");
    // 不在任何单位内部的位置一个都不动（文本里的位置本来就合法）。
    expectTrue(normalizeRange(tree, Range{2, 4}) == Range{2, 4},
               "1.2 / 端点外扩：文本内部的位置不动");
    // 空区间（光标）不扩：扩了会把"点在控件里"变成"选中整个控件"。
    expectTrue(normalizeRange(tree, Range{12, 12}) == Range{12, 12},
               "1.2 / 端点外扩：空区间（光标）保持不动");
    expectTrue(normalizeRange(tree, Range{8, 15}) == Range{8, 15},
               "1.2 / 端点外扩：跨过控件的区间不动（两端本来就在外面）");
}

/**
 * 1.2：取区间（纯读）。含控件的区间被拒并给原因 —— 不静默截断。
 */
void checkExtract() {
    const NodeTree tree = makeEditTree();

    // 流坐标：TextRun("abcdef") 占 [1,7)，所以位置 3..5 就是 "cd"。
    const ExtractResult one = extractRange(tree, Range{3, 5});
    expectTrue(one.ok && one.fragment.pieces.size() == 1 &&
                   one.fragment.pieces[0].cps == cps({0x63, 0x64}) && one.fragment.span() == 2,
               "1.2 / 提取：一段文本，片段长度等于区间长度");

    // 跨段落标记：取出文本 + 段落标记 + 文本三段，片段长度 = 2 + 1 + 1。
    const ExtractResult multi = extractRange(tree, Range{5, 9});
    expectTrue(multi.ok && multi.fragment.pieces.size() == 3 &&
                   multi.fragment.pieces[0].cps == cps({0x65, 0x66}) &&
                   multi.fragment.pieces[1].kind == PieceKind::ParaMark &&
                   multi.fragment.pieces[2].cps == cps({0x78}) && multi.fragment.span() == 4,
               "1.2 / 提取：跨段落标记取出三段（文本 / 标记 / 文本）");
    expectTrue(multi.ok && multi.fragment.pieces[1].para.font_index == 3 &&
                   multi.fragment.pieces[1].para.size == Length::abs(ptToEmu(14.0)),
               "1.2 / 提取：段落标记把段落格式一起带出来");

    const ExtractResult bad = extractRange(tree, Range{10, 14});
    expectTrue(!bad.ok && bad.error.find("ContentControl") != std::string::npos,
               "1.2 / 提取：区间含控件时被拒，原因里点出是哪种元素（" + bad.error + "）");

    // 直接给一个落在控件内部的区间也一样：先外扩，再因为含控件而被拒。
    const ExtractResult inside = extractRange(tree, Range{11, 13});
    expectTrue(!inside.ok, "1.2 / 提取：落在控件内部的区间先外扩、再被判为含控件");

    const ExtractResult empty = extractRange(tree, Range{3, 3});
    expectTrue(empty.ok && empty.fragment.empty(), "1.2 / 提取：空区间取出空片段");
}

/**
 * 1.2 的核心判据：步骤往返。
 *
 * 应用步骤、规范化、应用逆步骤、再规范化 —— 结构化快照必须与开始时逐条相同。规范化夹在中间
 * 是刻意的：步骤只做最小改动，"合法形状"的定义在规范化那一侧（1.1），真实调用也是这个次序
 * （事务提交前跑一次规范化）。
 */
std::string roundTrip(NodeTree& tree, const Step& step) {
    normalizeDocument(tree);
    const std::vector<std::string> before = shapeSnapshot(tree);

    const StepResult applied = applyStep(tree, step);
    if (!applied.ok) return "应用失败：" + applied.error;
    normalizeDocument(tree);

    const StepResult undone = applyStep(tree, invertStep(step));
    if (!undone.ok) return "逆操作失败：" + undone.error;
    normalizeDocument(tree);

    return snapshotDiffText(before, shapeSnapshot(tree));
}

/**
 * 1.2：每个步骤类型的往返判据，参数覆盖头、尾、中间与跨段落。
 */
void checkStepRoundTrip() {
    {
        NodeTree tree = makeEditTree();
        InsertText s;
        s.at = 3;
        s.cps = cps({0x41, 0x42});
        expectTrue(roundTrip(tree, Step{s}).empty(), "1.2 / 往返：InsertText 插在文本中间（同标记并入）");
    }
    {
        NodeTree tree = makeEditTree();
        InsertText s;
        s.at = 3;
        s.cps = cps({0x41});
        s.marks.bold = true;
        expectTrue(roundTrip(tree, Step{s}).empty(), "1.2 / 往返：InsertText 标记不同（切开原节点）");
    }
    {
        NodeTree tree = makeEditTree();
        InsertText s;
        s.at = 1;
        s.cps = cps({0x41});
        expectTrue(roundTrip(tree, Step{s}).empty(), "1.2 / 往返：InsertText 插在文档最前");
    }
    {
        NodeTree tree = makeEditTree();
        InsertText s;
        s.at = 7;
        s.cps = cps({0x41});
        expectTrue(roundTrip(tree, Step{s}).empty(), "1.2 / 往返：InsertText 插在段落标记之前");
    }
    {
        NodeTree tree = makeEditTree();
        InsertParaMark s;
        s.at = 4;
        expectTrue(roundTrip(tree, Step{s}).empty(), "1.2 / 往返：InsertParaMark 分段");
    }
    {
        NodeTree tree = makeEditTree();
        // 格式与样式引用从文档读出来带上（手填一份会让撤销还原出另一个格式）。
        const ParaMarkBuild b = makeRemoveParaMark(tree, 7);
        expectTrue(b.ok && roundTrip(tree, b.step).empty(),
                   "1.2 / 往返：RemoveParaMark 合并两段" + (b.ok ? "" : "（" + b.error + "）"));
    }
    {
        NodeTree tree = makeEditTree();
        const DeleteBuild b = makeDeleteRange(tree, Range{2, 4});
        expectTrue(b.ok && roundTrip(tree, b.step).empty(), "1.2 / 往返：DeleteRange 同段内");
    }
    {
        NodeTree tree = makeEditTree();
        const DeleteBuild b = makeDeleteRange(tree, Range{5, 9});
        expectTrue(b.ok && roundTrip(tree, b.step).empty(), "1.2 / 往返：DeleteRange 跨段落标记");
    }
    {
        NodeTree tree = makeEditTree();
        const DeleteBuild b = makeDeleteRange(tree, Range{1, 10});
        expectTrue(b.ok && roundTrip(tree, b.step).empty(),
                   "1.2 / 往返：DeleteRange 从文档开头取到控件之前");
    }
    {
        NodeTree tree = makeEditTree();
        const ExtractResult ex = extractRange(tree, Range{5, 9});
        InsertFragment f;
        f.at = 5;
        f.content = ex.fragment;
        expectTrue(ex.ok && roundTrip(tree, Step{f}).empty(), "1.2 / 往返：InsertFragment 插回");
    }
}

/**
 * 1.2：取区间 → 删除 → 插回，等于原状。
 */
void checkExtractDeleteInsertBack() {
    NodeTree tree = makeEditTree();
    normalizeDocument(tree);
    const std::vector<std::string> before = shapeSnapshot(tree);

    const Range r{5, 9};
    const ExtractResult ex = extractRange(tree, r);
    expectTrue(ex.ok && ex.fragment.span() == 4, "1.2 / 取删插：片段长度为 4");

    const DeleteBuild b = makeDeleteRange(tree, r);
    const StepResult del = applyStep(tree, b.step);
    normalizeDocument(tree);
    expectTrue(del.ok && snapshotDiffText(before, shapeSnapshot(tree)) != "",
               "1.2 / 取删插：删除之后文档确实变了");

    InsertFragment back;
    back.at = r.begin();
    back.content = ex.fragment;
    const StepResult ins = applyStep(tree, Step{back});
    normalizeDocument(tree);

    const std::string diff = snapshotDiffText(before, shapeSnapshot(tree));
    expectTrue(ins.ok && diff.empty(), "1.2 / 取删插：插回之后等于原状" +
                                           (diff.empty() ? "" : "，差异 " + diff));
    expectClean(tree, "1.2 / 取删插之后的树");
}

/**
 * 1.2：位置映射与逆映射往返一致。
 *
 * 对区间之外的每个位置 p：映射到编辑后，再用逆步骤映射回来，必须回到 p；两个方向用同一个
 * assoc。位置正好落在区间端点上时，贴左与贴右各归各的一边 —— 这正是 assoc 存在的理由。
 * 被删区间内部的位置没有原像，判据不覆盖它们。
 */
void checkMapRoundTrip() {
    struct Case {
        Step step;
        FlowPos lo;  // 被影响区间的起点
        FlowPos hi;  // 终点（插入时为 lo）
        const char* label;
    };

    const NodeTree probe = makeEditTree();
    const int size = FlowIndex(probe, FlowVisibility::Design).size();
    const DeleteBuild del = makeDeleteRange(probe, Range{5, 9});

    std::vector<Case> cases;
    {
        InsertText s;
        s.at = 3;
        s.cps = cps({0x41, 0x42});
        cases.push_back(Case{Step{s}, 3, 3, "InsertText"});
    }
    if (del.ok) cases.push_back(Case{del.step, 5, 9, "DeleteRange"});
    {
        InsertParaMark s;
        s.at = 4;
        cases.push_back(Case{Step{s}, 4, 4, "InsertParaMark"});
    }
    {
        RemoveParaMark s;
        s.at = 7;
        cases.push_back(Case{Step{s}, 7, 8, "RemoveParaMark"});
    }

    for (const Case& c : cases) {
        int checked = 0;
        int bad = 0;
        FlowPos worst = -1;
        for (FlowPos p = 0; p <= size; ++p) {
            if (p > c.lo && p < c.hi) continue;
            const Assoc a = p <= c.lo ? Assoc::Before : Assoc::After;
            const FlowPos q = mapPos(c.step, p, a);
            const FlowPos back = mapPos(invertStep(c.step), q, a);
            ++checked;
            if (back != p) {
                ++bad;
                if (worst < 0) worst = p;
            }
        }
        expectTrue(bad == 0, std::string("1.2 / 位置映射：") + c.label + " 与逆映射往返一致（" +
                                 std::to_string(checked) + " 个位置，不一致 " + std::to_string(bad) +
                                 (worst < 0 ? "" : "，首个是 " + std::to_string(worst)) + "）");
    }

    // 被删区间内部的位置一律塌到区间起点 —— 这一段是单向的，没有原像。
    {
        const InsertText s{5, cps({0x41})};
        expectTrue(mapPos(Step{s}, 5, Assoc::Before) == 5 &&
                       mapPos(Step{s}, 5, Assoc::After) == 6,
                   "1.2 / 位置映射：插入点上的位置由 assoc 决定贴左还是贴右");
        const DeleteBuild d = makeDeleteRange(probe, Range{5, 9});
        expectTrue(d.ok && mapPos(d.step, 6, Assoc::Before) == 5 &&
                       mapPos(d.step, 9, Assoc::After) == 5,
                   "1.2 / 位置映射：被删区间内部与终点都塌到区间起点");
    }
}

/**
 * 1.3：事务与撤销。连撤 N 次必须逐次回到对应步骤的状态，重做再逐次回到编辑后。
 */
void checkUndoRedo() {
    NodeTree tree = makeEditTree();
    normalizeDocument(tree);
    UndoStack stack(tree);

    std::vector<std::vector<std::string>> states;
    std::vector<Range> sel_before;
    std::vector<Range> sel_after;
    states.push_back(shapeSnapshot(tree));

    // 事务一：插入一个粗体字（标记与邻居不同，会切开原节点）。
    {
        InsertText s;
        s.at = 3;
        s.cps = cps({0x41});
        s.marks.bold = true;
        const StepResult r =
            stack.commit(Transaction{{Step{s}}, Range{3, 3}, Range{4, 4}, "输入"});
        expectTrue(r.ok, "1.3 / 提交：插入成功（" + r.error + "）");
    }
    states.push_back(shapeSnapshot(tree));
    sel_before.push_back(Range{3, 3});
    sel_after.push_back(Range{4, 4});

    // 事务二：分段。
    {
        InsertParaMark s;
        s.at = 5;
        const StepResult r =
            stack.commit(Transaction{{Step{s}}, Range{5, 5}, Range{6, 6}, "分段"});
        expectTrue(r.ok, "1.3 / 提交：分段成功（" + r.error + "）");
    }
    states.push_back(shapeSnapshot(tree));
    sel_before.push_back(Range{5, 5});
    sel_after.push_back(Range{6, 6});

    // 事务三：删除一段（跨过刚插入的段落标记）。
    {
        const DeleteBuild b = makeDeleteRange(tree, Range{3, 7});
        const StepResult r =
            stack.commit(Transaction{{b.step}, Range{3, 7}, Range{3, 3}, "删除"});
        expectTrue(b.ok && r.ok, "1.3 / 提交：删除成功（" + b.error + r.error + "）");
    }
    states.push_back(shapeSnapshot(tree));
    sel_before.push_back(Range{3, 7});
    sel_after.push_back(Range{3, 3});

    expectTrue(stack.undoDepth() == 3 && !stack.canRedo(),
               "1.3 / 栈：三次提交压出三个撤销单位，重做栈是空的");

    int bad_state = 0;
    int bad_sel = 0;
    for (int i = 2; i >= 0; --i) {
        Range restore{};
        const StepResult r = stack.undo(&restore);
        const std::string diff = snapshotDiffText(states[static_cast<size_t>(i)], shapeSnapshot(tree));
        if (!r.ok || !diff.empty()) ++bad_state;
        if (restore != sel_before[static_cast<size_t>(i)]) ++bad_sel;
    }
    expectTrue(bad_state == 0, "1.3 / 撤销：连撤三次，每次都与对应步骤的状态逐条相同");
    expectTrue(bad_sel == 0, "1.3 / 撤销：每次恢复的是该事务的编辑前选区");
    expectTrue(!stack.canUndo() && stack.redoDepth() == 3,
               "1.3 / 撤销到底：没有可撤的事务，重做栈有三项");

    // 帧仍与全量一致，在模型层的落点：撤到底之后重新编译，产物必须与起始状态逐字段相同。
    {
        const NodeTree fresh = makeEditTree();
        const CompileResult ref = compileDocument(fresh, FlowVisibility::Design, noStyles());
        const CompileResult now = compileDocument(tree, FlowVisibility::Design, noStyles());
        int diff = 0;
        if (ref.blocks.size() != now.blocks.size()) {
            diff = 1;
        } else {
            for (size_t i = 0; i < ref.blocks.size(); ++i) {
                if (!diffBlock(ref.blocks[i], now.blocks[i]).empty()) ++diff;
            }
        }
        expectTrue(diff == 0, "1.3 / 帧：撤到底之后重新编译，产物与起始的文档逐字段相同（" +
                                  std::to_string(now.blocks.size()) + " 段，不一致 " +
                                  std::to_string(diff) + "）");
    }

    int bad_redo = 0;
    int bad_redo_sel = 0;
    for (int i = 0; i < 3; ++i) {
        Range restore{};
        const StepResult r = stack.redo(&restore);
        const std::string diff =
            snapshotDiffText(states[static_cast<size_t>(i) + 1], shapeSnapshot(tree));
        if (!r.ok || !diff.empty()) ++bad_redo;
        if (restore != sel_after[static_cast<size_t>(i)]) ++bad_redo_sel;
    }
    expectTrue(bad_redo == 0, "1.3 / 重做：连做三次，每次都与对应步骤的状态逐条相同");
    expectTrue(bad_redo_sel == 0, "1.3 / 重做：每次回到该事务的编辑后选区");
    expectTrue(stack.undoDepth() == 3 && !stack.canRedo(), "1.3 / 重做到底：栈回到撤销前的样子");
    expectClean(tree, "1.3 / 撤销重做之后的树");
}

/**
 * 1.3：撤销分组的几条边界 —— 什么并、什么不并，以及提交失败要整条作废。
 */
void checkUndoGrouping() {
    // 连续输入并成一个撤销单位；一次撤销回退整段输入。
    {
        NodeTree tree = makeEditTree();
        normalizeDocument(tree);
        UndoStack stack(tree);
        for (uint32_t i = 0; i < 5; ++i) {
            InsertText s;
            s.at = 3 + static_cast<FlowPos>(i);
            s.cps = cps({0x41 + i});
            stack.commit(Transaction{{Step{s}}, Range{s.at, s.at},
                                     Range{s.at + 1, s.at + 1}, "输入"});
        }
        expectTrue(stack.undoDepth() == 1, "1.3 / 分组：五次连续输入并成一个撤销单位");

        Range restore{};
        const StepResult r = stack.undo(&restore);
        const std::string diff = snapshotDiffText(shapeSnapshot(makeEditTree()), shapeSnapshot(tree));
        expectTrue(r.ok && diff.empty() && restore == Range{3, 3},
                   "1.3 / 分组：一次撤销回退整段输入，并回到第一笔的编辑前选区" + diff);
    }

    // 位置不接着的一笔另起一个撤销单位。
    {
        NodeTree tree = makeEditTree();
        normalizeDocument(tree);
        UndoStack stack(tree);
        InsertText a;
        a.at = 3;
        a.cps = cps({0x41});
        stack.commit(Transaction{{Step{a}}, Range{3, 3}, Range{4, 4}, "输入"});
        InsertText b;
        b.at = 4;
        b.cps = cps({0x42});
        stack.commit(Transaction{{Step{b}}, Range{4, 4}, Range{5, 5}, "输入"});
        InsertText far;
        far.at = 9;  // 隔开了（中间隔着段落标记）
        far.cps = cps({0x43});
        stack.commit(Transaction{{Step{far}}, Range{9, 9}, Range{10, 10}, "输入"});
        expectTrue(stack.undoDepth() == 2, "1.3 / 分组：位置不接着的两笔分属两个撤销单位");
    }

    // 标记不同的一笔另起一个撤销单位。
    {
        NodeTree tree = makeEditTree();
        normalizeDocument(tree);
        UndoStack stack(tree);
        InsertText a;
        a.at = 3;
        a.cps = cps({0x41});
        stack.commit(Transaction{{Step{a}}, Range{3, 3}, Range{4, 4}, "输入"});
        InsertText b;
        b.at = 4;
        b.cps = cps({0x42});
        b.marks.bold = true;
        stack.commit(Transaction{{Step{b}}, Range{4, 4}, Range{5, 5}, "加粗输入"});
        expectTrue(stack.undoDepth() == 2, "1.3 / 分组：标记不同的一笔另起一个撤销单位");
    }

    // 撤销之后再提交：重做栈被清空。
    {
        NodeTree tree = makeEditTree();
        normalizeDocument(tree);
        UndoStack stack(tree);
        InsertText a;
        a.at = 3;
        a.cps = cps({0x41});
        stack.commit(Transaction{{Step{a}}, Range{3, 3}, Range{4, 4}, "输入"});
        stack.undo(nullptr);
        expectTrue(stack.canRedo() && stack.redoDepth() == 1, "1.3 / 重做栈：撤销之后可以重做");
        InsertText b;
        b.at = 3;
        b.cps = cps({0x42});
        stack.commit(Transaction{{Step{b}}, Range{3, 3}, Range{4, 4}, "输入"});
        expectTrue(!stack.canRedo() && stack.redoDepth() == 0,
                   "1.3 / 重做栈：撤销之后再提交会清空重做栈");
    }

    // 提交中途失败：整条事务作废，文档与栈都回到提交前。
    {
        NodeTree tree = makeEditTree();
        normalizeDocument(tree);
        UndoStack stack(tree);
        const std::vector<std::string> before = shapeSnapshot(tree);
        InsertText good;
        good.at = 3;
        good.cps = cps({0x41});
        InsertText bad;
        bad.at = 9999;
        bad.cps = cps({0x5A});
        const StepResult r =
            stack.commit(Transaction{{Step{good}, Step{bad}}, Range{3, 3}, Range{4, 4}, "两条"});
        expectTrue(!r.ok && stack.undoDepth() == 0 && !stack.canRedo() &&
                       snapshotDiffText(before, shapeSnapshot(tree)).empty(),
                   "1.3 / 提交失败：整条事务作废，文档与栈都回到提交前（" + r.error + "）");
    }

    // 规范化不改流位置总数 —— 撤销能在规范化之后的文档上按原坐标工作，靠的就是这一条。
    {
        NodeTree tree = makeMessyTree();
        const int before = FlowIndex(tree, FlowVisibility::Design).size();
        normalizeDocument(tree);
        const int after = FlowIndex(tree, FlowVisibility::Design).size();
        expectTrue(before == after, "1.3 / 坐标：规范化前后流位置总数不变（" +
                                        std::to_string(before) + " → " + std::to_string(after) + "）");
    }
}

/**
 * 表单形状：静态文字、一个可编辑控件、一个只读控件、只读外层套可编辑内层。流坐标（size 23）：
 *   主诉[1,3)  chief（可编辑）[3,7)：起始 3、内容 4..5、结束 6  PM[7,8)
 *   姓名[8,10)  name（只读）[10,14)：起始 10、内容 11..12、结束 13  PM[14,15)
 *   outer（只读）[15,21)：起始 15、内容 16..19、结束 20；内含 inner（可编辑）[17,20)：内容 18
 *   PM[21,22)
 * 只读外层套可编辑内层是判据的关键：可编辑集合取交集，所以 inner 的内容仍不可编辑。
 */
NodeTree makeFormTree() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x4e3b, 0x8bc9}));
    const NodeId chief = tree.appendChild(sec, NodeKind::ContentControl);
    tree.find(chief)->control.key = "chief";
    tree.setText(tree.appendChild(chief, NodeKind::TextRun), cps({0x5934, 0x75db}));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x59d3, 0x540d}));
    const NodeId name = tree.appendChild(sec, NodeKind::ContentControl);
    tree.find(name)->control.key = "name";
    tree.find(name)->control.readonly = true;
    tree.setText(tree.appendChild(name, NodeKind::TextRun), cps({0x5f20, 0x4e09}));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    const NodeId outer = tree.appendChild(sec, NodeKind::ContentControl);
    tree.find(outer)->control.key = "outer";
    tree.find(outer)->control.readonly = true;
    tree.setText(tree.appendChild(outer, NodeKind::TextRun), cps({0x58f9}));
    const NodeId inner = tree.appendChild(outer, NodeKind::ContentControl);
    tree.find(inner)->control.key = "inner";
    tree.setText(tree.appendChild(inner, NodeKind::TextRun), cps({0x8d30}));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

/**
 * 可编辑的外层套一个只读内层，用来验"区间整块包住只读控件"这条闸门。
 *
 * 流坐标：CC host（可编辑）[1,8)，内容 2..7；其中 CC locked（只读）[3,6)，内容 4..5。
 * 于是 3 与 6 在 host 里可编辑，而中间的 4、5 被 locked 挡住。
 */
NodeTree makeFormNestedTree() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    const NodeId host = tree.appendChild(sec, NodeKind::ContentControl);
    tree.find(host)->control.key = "host";
    tree.setText(tree.appendChild(host, NodeKind::TextRun), cps({0x4e00}));
    const NodeId locked = tree.appendChild(host, NodeKind::ContentControl);
    tree.find(locked)->control.key = "locked";
    tree.find(locked)->control.readonly = true;
    tree.setText(tree.appendChild(locked, NodeKind::TextRun), cps({0x4e8c}));
    tree.setText(tree.appendChild(host, NodeKind::TextRun), cps({0x4e09}));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

/**
 * 两个可编辑控件被一个只读控件隔开：CC a [1,4)、只读 CC b [6,9)、CC c [11,14)。
 * 用来验 Tab 跳的是"可编辑控件"，而不是"下一个控件"。
 */
NodeTree makeFormTabTree() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    const NodeId a = tree.appendChild(sec, NodeKind::ContentControl);
    tree.find(a)->control.key = "a";
    tree.setText(tree.appendChild(a, NodeKind::TextRun), cps({0x4e00}));
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x4e8c, 0x4e8c}));
    const NodeId b = tree.appendChild(sec, NodeKind::ContentControl);
    tree.find(b)->control.key = "b";
    tree.find(b)->control.readonly = true;
    tree.setText(tree.appendChild(b, NodeKind::TextRun), cps({0x4e09}));
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x56db, 0x56db}));
    const NodeId c = tree.appendChild(sec, NodeKind::ContentControl);
    tree.find(c)->control.key = "c";
    tree.setText(tree.appendChild(c, NodeKind::TextRun), cps({0x4e94}));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

/**
 * posAt 在这个形状上是否两两不同：没有两个流位置归到同一格 Pos。
 *
 * 少一个位置可寻址时这里会失败（某个位置被就近归给了邻居），而那种失败正是四个边界缺陷的
 * 成因。它比"往返"更强地钉住"归属互不相同"这句话。
 */
bool posAtInjective(const NodeTree& tree) {
    const FlowIndex idx(tree, FlowVisibility::Design);
    std::vector<std::string> seen;
    for (int f = 0; f <= idx.size(); ++f) {
        const Pos p = idx.posAt(f);
        const std::string key = std::to_string(p.container) + ":" + std::to_string(p.offset);
        for (const std::string& s : seen) {
            if (s == key) return false;
        }
        seen.push_back(key);
    }
    return true;
}

/**
 * 1.4：控件的边界位置。一个行内控件把同段内的位置切成四段 —— 控件之前 / 内容开头 / 内容末尾 /
 * 控件之后，四个流位置互不相同，且每一个都有自己的一格 Pos。
 *
 * 这是上一轮四个边界缺陷的正面判据：那时"在 [ 与 ] 之间"在整数偏移里没有编码，于是
 * "控件之前"与"控件内部"会撞在同一个位置上。现在它们各自有位置，靠的是边界是流里的元素。
 */
void checkBoundaryPositions() {
    const NodeTree tree = makeFormTree();
    const NodeId sec = tree.firstSectionId();
    const NodeId chief = tree.find(sec)->children[1]->id;
    const FlowIndex idx(tree, FlowVisibility::Design);

    const FlowPos before = idx.flowAt(Pos{sec, 1});
    const FlowPos inside = idx.flowAt(Pos{chief, 0});
    const FlowPos content_end = idx.flowAt(Pos{chief, 1});
    const FlowPos after = idx.flowAt(Pos{sec, 2});
    expectTrue(before == 3 && inside == 4 && content_end == 6 && after == 7,
               "1.4 / 边界：控件之前 3、内容开头 4、内容末尾 6、控件之后 7");
    expectTrue(before != inside && inside != content_end && content_end != after &&
                   before != content_end && before != after && inside != after,
               "1.4 / 边界：四个流位置两两不同");
    expectTrue(posAtInjective(makeFormTree()) && posAtInjective(makeEmptyControl()) &&
                   posAtInjective(makeNestedControls()) && posAtInjective(makeControlAcrossMark()),
               "1.4 / 边界：四种控件形状下 posAt 都单射（归属互不相同）");
}

/**
 * 1.4：可编辑集合。表单模式下它是交集 —— 包住这个位置的每一层内容控件都允许，位置才可编辑。
 */
void checkEditableSet() {
    const NodeTree tree = makeFormTree();
    const FlowIndex idx(tree, FlowVisibility::Design);
    const int last = idx.size();

    bool all = true;
    for (int f = 0; f <= last; ++f) {
        if (!canEditAt(tree, EditMode::Document, f).allowed) all = false;
    }
    expectTrue(all, "1.4 / 可编辑集合：文档模式全部位置可编辑");

    bool none = true;
    for (int f = 0; f <= last; ++f) {
        if (canEditAt(tree, EditMode::ReadOnly, f).allowed) none = false;
    }
    expectTrue(none, "1.4 / 可编辑集合：只读模式全部位置不可编辑");

    std::string got;
    for (int f = 0; f <= last; ++f) {
        if (canEditAt(tree, EditMode::Form, f).allowed) got += std::to_string(f) + " ";
    }
    expectTrue(got == "4 5 6 ",
               "1.4 / 可编辑集合：表单模式只有可编辑控件的内容可编辑（实得 " + got + "）");

    const NodeId sec = tree.firstSectionId();
    const NodeId name = tree.find(sec)->children[4]->id;
    const NodeId outer = tree.find(sec)->children[6]->id;
    const NodeId inner = tree.find(outer)->children[1]->id;
    expectTrue(tree.find(name)->control.readonly, "1.4 / 可编辑集合：只读控件自身标着只读（对照）");
    expectTrue(!tree.find(inner)->control.readonly,
               "1.4 / 可编辑集合（交集）：内层控件自身是可编辑的（只看它会判成可编辑）");
    expectTrue(!canEditAt(tree, EditMode::Form, 18).allowed &&
                   !canEditAt(tree, EditMode::Form, 19).allowed,
               "1.4 / 可编辑集合（交集）：只读外层里的可编辑控件位置仍不可编辑");
    expectTrue(!canEditAt(tree, EditMode::Form, 2).allowed,
               "1.4 / 可编辑集合：控件之外的静态文字不可编辑");
    expectTrue(!canEditAt(tree, EditMode::Form, 3).allowed,
               "1.4 / 可编辑集合：控件起始边界（控件之前）不可编辑");
    expectTrue(!canEditAt(tree, EditMode::Form, 7).allowed, "1.4 / 可编辑集合：段落标记处不可编辑");

    // 隐藏的控件在表单模式下整棵摘除，位置根本不在流里，也就谈不上可编辑。
    {
        NodeTree t = makeFormTree();
        const NodeId s = t.firstSectionId();
        const NodeId chief = t.find(s)->children[1]->id;
        t.find(chief)->hidden = true;
        const FlowIndex disp(t, FlowVisibility::Display);
        expectTrue(disp.flowAt(Pos{chief, 0}) < 0,
                   "1.4 / 可编辑集合：隐藏控件在表单模式下滑除，位置不可寻址");
        bool any = false;
        for (int f = 0; f <= disp.size(); ++f) {
            if (canEditAt(t, EditMode::Form, f).allowed) any = true;
        }
        expectTrue(!any, "1.4 / 可编辑集合：隐藏控件摘除后，表单模式无任何可编辑位置");
    }
}

/**
 * 1.4：方向键。表单模式下目标位置必须在集合内；集合外的方向就沿该方向找最近的可编辑位置，
 * 找不到则原地不动。选择"跳过"而不是"顶住边界"，是为了不把键盘用户困在单个控件里。
 */
void checkFormNavigation() {
    const NodeTree tree = makeFormTree();

    expectTrue(stepEditable(tree, EditMode::Form, 2, +1) == 4,
               "1.4 / 方向键：控件之外往右跳到最近的可编辑位置（跳过控件起始边界）");
    expectTrue(stepEditable(tree, EditMode::Form, 4, +1) == 5 &&
                   stepEditable(tree, EditMode::Form, 5, +1) == 6,
               "1.4 / 方向键：控件内容里逐位前进");
    expectTrue(stepEditable(tree, EditMode::Form, 6, +1) == 6,
               "1.4 / 方向键：控件末尾往右停在原地，不跳出可编辑集合");
    expectTrue(stepEditable(tree, EditMode::Form, 6, -1) == 5, "1.4 / 方向键：反向同理");
    expectTrue(stepEditable(tree, EditMode::Form, 20, -1) == 6,
               "1.4 / 方向键：从只读区往左跳回最近的可编辑位置");

    std::vector<int> reached{4};
    FlowPos cur = 4;
    for (int i = 0; i < 8; ++i) {
        const FlowPos next = stepEditable(tree, EditMode::Form, cur, +1);
        if (next == cur) break;
        cur = next;
        reached.push_back(cur);
    }
    expectTrue((reached == std::vector<int>{4, 5, 6}),
               "1.4 / 方向键：可编辑集合的每个位置都能到达");

    expectTrue(stepEditable(tree, EditMode::Document, 2, +1) == 3 &&
                   stepEditable(tree, EditMode::ReadOnly, 2, +1) == 3,
               "1.4 / 方向键：文档与只读模式是相邻一步（落点不受限）");
}

/**
 * 1.4：Tab。表单模式在可编辑控件之间走；文档模式（Tab 是制表符）与只读模式（放行）都不消费。
 */
void checkTabStops() {
    const NodeTree tree = makeFormTabTree();

    expectTrue(tabFrom(tree, EditMode::Form, 0, false).handled &&
                   tabFrom(tree, EditMode::Form, 0, false).target == 2,
               "1.4 / Tab：表单模式落到第一个可编辑控件的起点");
    expectTrue(tabFrom(tree, EditMode::Form, 3, false).handled &&
                   tabFrom(tree, EditMode::Form, 3, false).target == 12,
               "1.4 / Tab：跳过中间的只读控件，落到下一个可编辑控件");
    expectTrue(!tabFrom(tree, EditMode::Form, 12, false).handled,
               "1.4 / Tab：末个可编辑控件之后放行给宿主");
    expectTrue(tabFrom(tree, EditMode::Form, 13, true).handled &&
                   tabFrom(tree, EditMode::Form, 13, true).target == 2,
               "1.4 / Tab：Shift+Tab 退回上一个可编辑控件");
    expectTrue(!tabFrom(tree, EditMode::Form, 2, true).handled,
               "1.4 / Tab：没有上一个可编辑控件时放行");
    expectTrue(!tabFrom(tree, EditMode::Document, 0, false).handled,
               "1.4 / Tab：文档模式不消费 —— Tab 在那里是制表符");
    expectTrue(!tabFrom(tree, EditMode::ReadOnly, 0, false).handled,
               "1.4 / Tab：只读模式放行，让焦点落到编辑器之外");
}

/**
 * 1.4：编辑动作的越界闸门。越界一律拒绝并给原因，不静默把范围裁到可编辑部分。
 */
void checkEditRangeGate() {
    const NodeTree tree = makeFormTree();

    expectTrue(canEditRange(tree, EditMode::Form, Range{4, 6}).allowed,
               "1.4 / 编辑区间：整段落在可编辑控件内容里，允许");

    const EditCheck out = canEditRange(tree, EditMode::Form, Range{2, 5});
    expectTrue(!out.allowed && !out.reason.empty(),
               "1.4 / 编辑区间：起点在控件之外被拒并给原因（" + out.reason + "）");

    const EditCheck into_readonly = canEditRange(tree, EditMode::Form, Range{5, 11});
    expectTrue(!into_readonly.allowed && into_readonly.reason.find("只读") != std::string::npos,
               "1.4 / 编辑区间：跨进只读控件被拒，原因点名只读（" + into_readonly.reason + "）");

    expectTrue(!canEditRange(tree, EditMode::Form, Range{3, 7}).allowed,
               "1.4 / 编辑区间：整块覆盖控件（含它的一对边界）被拒");

    const EditCheck inner = canEditRange(tree, EditMode::Form, Range{18, 20});
    expectTrue(!inner.allowed && inner.reason.find("只读") != std::string::npos,
               "1.4 / 编辑区间（交集）：只读外层内的可编辑控件区间被拒（" + inner.reason + "）");

    expectTrue(canEditRange(tree, EditMode::Document, Range{2, 20}).allowed,
               "1.4 / 编辑区间：文档模式放行");
    expectTrue(!canEditRange(tree, EditMode::ReadOnly, Range{4, 6}).allowed,
               "1.4 / 编辑区间：只读模式一律拒绝");
    expectTrue(canEditRange(tree, EditMode::Form, Range{5, 5}).allowed &&
                   !canEditRange(tree, EditMode::Form, Range{12, 12}).allowed,
               "1.4 / 编辑区间：空区间（光标）等同落点判定");

    // 区间两端都落在可编辑的外层控件里，但中间整块包住了只读内层 —— 这条闸门堵的是
    // "跨过只读控件就能删掉它的内容"这个绕开只读的口子。
    const NodeTree nested = makeFormNestedTree();
    const EditCheck wrap = canEditRange(nested, EditMode::Form, Range{3, 6});
    expectTrue(!wrap.allowed && wrap.reason.find("包住") != std::string::npos,
               "1.4 / 编辑区间：整块包住只读控件被拒（" + wrap.reason + "）");
    expectTrue(canEditRange(nested, EditMode::Form, Range{2, 3}).allowed,
               "1.4 / 编辑区间：只覆盖只读控件之前的那一段，允许（对照）");
}

/**
 * 1.4：结构操作。控件属性、默认关闭，作者显式开放后才允许。
 */
void checkStructureOps() {
    const NodeTree tree = makeFormTree();
    const NodeId sec = tree.firstSectionId();
    const NodeId text = tree.find(sec)->children[0]->id;
    const NodeId chief = tree.find(sec)->children[1]->id;

    const EditCheck locked = canDoStructureOp(tree, EditMode::Form, chief);
    expectTrue(!locked.allowed && locked.reason.find("锁") != std::string::npos,
               "1.4 / 结构操作：表单模式默认被拒（" + locked.reason + "）");

    NodeTree open = makeFormTree();
    const NodeId sec2 = open.firstSectionId();
    const NodeId chief2 = open.find(sec2)->children[1]->id;
    open.find(chief2)->control.structureLock = false;
    expectTrue(canDoStructureOp(open, EditMode::Form, chief2).allowed,
               "1.4 / 结构操作：控件声明可删（structureLock = false）后才允许");

    expectTrue(canDoStructureOp(tree, EditMode::Document, chief).allowed,
               "1.4 / 结构操作：文档模式放行（作者在设计表单）");
    expectTrue(!canDoStructureOp(tree, EditMode::ReadOnly, chief).allowed,
               "1.4 / 结构操作：只读模式一律拒绝");
    const EditCheck not_ctrl = canDoStructureOp(tree, EditMode::Form, text);
    expectTrue(!not_ctrl.allowed && not_ctrl.reason.find("控件") != std::string::npos,
               "1.4 / 结构操作：目标不是控件时被拒（" + not_ctrl.reason + "）");
}

/**
 * 1.2 的剩余一半：节点级步骤（InsertNode / RemoveNode）。控件这类整块元素不按区间增删，
 * 而是整棵子树挂上、拿下，所以不会出现"半个控件"的中间态。
 *
 * 判据是两条：流位置的总数正好增减该子树的跨度；撤销之后"带身份的快照"逐条相同 ——
 * 只比内容的话，"重造一棵内容一样的子树"也会被当成通过，而那个节点的身份已经换了。
 */
void checkNodeStepRoundTrip() {
    // 删掉一个控件再撤销：文档逐字段回到原样，那个节点的 id 不变。
    {
        NodeTree tree = makeEditTree();
        const NodeId sec = tree.firstSectionId();
        const NodeId cc = tree.find(sec)->children[3]->id;
        normalizeDocument(tree);
        const std::vector<std::string> before = nodeIdsSnapshot(tree);

        const NodeBuild b = makeRemoveNode(tree, Pos{sec, 3});
        const auto* rm = std::get_if<RemoveNode>(&b.step);
        expectTrue(b.ok && rm != nullptr,
                   "1.2 / 节点级：删除控件的步骤构造成功" + (b.ok ? "" : "（" + b.error + "）"));
        expectTrue(rm != nullptr && rm->at_flow == 10 && rm->detached == cc &&
                       rm->removed.kind == NodeKind::ContentControl &&
                       rm->removed.children.size() == 1,
                   "1.2 / 节点级：记下的正是那个控件（流位置 10、身份与内容都在）");

        expectTrue(applyStep(tree, b.step).ok, "1.2 / 节点级：删除可应用");
        expectTrue(tree.find(cc) != nullptr && tree.find(cc)->parent == nullptr,
                   "1.2 / 节点级：控件从树上摘下，但没被销毁（节点还在池里）");
        expectTrue(FlowIndex(tree, FlowVisibility::Design).size() == 12,
                   "1.2 / 节点级：流位置总数 16 → 12（正好是那个控件的跨度 4）");
        expectClean(tree, "1.2 / 节点级：删除之后不变量全绿");

        const StepResult u = applyStep(tree, invertStep(b.step));
        expectTrue(u.ok && nodeIdsSnapshot(tree) == before,
                   "1.2 / 节点级：撤销之后带身份的快照逐条相同（id 没有变）" +
                       (u.ok ? "" : "（" + u.error + "）"));
    }

    // 插入一个新建的控件：构造时身份就定下来，撤销与重做挂的是同一棵树。
    {
        NodeTree tree = makeEditTree();
        const NodeId sec = tree.firstSectionId();
        normalizeDocument(tree);

        NodeBuild b = makeInsertNode(tree, Pos{sec, 1}, makeFreshControlSpec());
        const auto* ins = std::get_if<InsertNode>(&b.step);
        const NodeId planted = ins != nullptr ? ins->detached : kInvalidNodeId;
        expectTrue(b.ok && ins != nullptr && ins->at_flow == 7,
                   "1.2 / 节点级：插入控件的步骤构造成功，插入点流位置 7" +
                       (b.ok ? "" : "（" + b.error + "）"));
        expectTrue(planted != kInvalidNodeId && tree.find(planted) != nullptr &&
                       tree.find(planted)->parent == nullptr,
                   "1.2 / 节点级：构造时子树已经物化成摘下的一棵（身份就此定下）");

        const int size_before = FlowIndex(tree, FlowVisibility::Design).size();
        expectTrue(applyStep(tree, b.step).ok && tree.find(planted)->parent != nullptr,
                   "1.2 / 节点级：插入可应用，挂上去的是那棵树");
        expectTrue(FlowIndex(tree, FlowVisibility::Design).size() == size_before + 3,
                   "1.2 / 节点级：插入内容 1 码点的控件让流位置 +3（起始 + 内容 + 结束）");
        expectClean(tree, "1.2 / 节点级：插入之后不变量全绿");

        const std::vector<std::string> with_control = nodeIdsSnapshot(tree);
        expectTrue(applyStep(tree, invertStep(b.step)).ok && tree.find(planted)->parent == nullptr,
                   "1.2 / 节点级：撤销把它拿下来（没有销毁，身份还在）");
        expectTrue(applyStep(tree, b.step).ok && tree.find(planted)->parent != nullptr &&
                       nodeIdsSnapshot(tree) == with_control,
                   "1.2 / 节点级：重做挂回同一棵树，快照与第一次插入后逐条相同");
        expectClean(tree, "1.2 / 节点级：撤销重做一轮之后不变量全绿");
    }

    // 嵌套子树往返：控件里含段落标记与嵌套控件，各字段都要跟着走。
    {
        NodeTree tree = makeEditTree();
        const NodeId sec = tree.firstSectionId();
        normalizeDocument(tree);

        NodeSpec outer;
        outer.kind = NodeKind::ContentControl;
        outer.control.key = "outer";
        outer.control.readonly = true;
        outer.hidden = true;
        outer.para.size = Length::abs(ptToEmu(14.0));
        NodeSpec inner_mark;
        inner_mark.kind = NodeKind::ParagraphMark;
        inner_mark.para.font_index = 3;
        outer.children.push_back(inner_mark);
        NodeSpec inner;
        inner.kind = NodeKind::ContentControl;
        inner.control.key = "inner";
        inner.control.structureLock = false;
        NodeSpec text;
        text.kind = NodeKind::TextRun;
        text.text = cps({0x41, 0x42});
        text.marks.bold = true;
        inner.children.push_back(text);
        outer.children.push_back(inner);

        const NodeBuild b = makeInsertNode(tree, Pos{sec, 3}, outer);
        expectTrue(b.ok, "1.2 / 节点级：嵌套子树构造成功" + (b.ok ? "" : "（" + b.error + "）"));
        const int size_before = FlowIndex(tree, FlowVisibility::Design).size();
        expectTrue(applyStep(tree, b.step).ok &&
                       FlowIndex(tree, FlowVisibility::Design).size() == size_before + outer.span(),
                   "1.2 / 节点级：嵌套子树的跨度与 NodeSpec::span 一致（" +
                       std::to_string(outer.span()) + "）");
        expectClean(tree, "1.2 / 节点级：嵌套子树插入后不变量全绿");
        const std::vector<std::string> after = nodeIdsSnapshot(tree);
        expectTrue(
            applyStep(tree, invertStep(b.step)).ok && applyStep(tree, b.step).ok &&
                nodeIdsSnapshot(tree) == after,
            "1.2 / 节点级：嵌套子树撤销重做后逐字段相同（含 hidden / para / marks / 控件字段）");
    }
}

/**
 * 1.2：节点级步骤的位置映射。子树整体后移或前移它的跨度，被摘掉的区间没有原像。
 */
void checkNodeStepMap() {
    NodeTree tree = makeEditTree();
    const NodeId sec = tree.firstSectionId();
    normalizeDocument(tree);

    const NodeBuild rm = makeRemoveNode(tree, Pos{sec, 3});  // 流区间 [10, 14)
    expectTrue(mapPos(rm.step, 5, Assoc::After) == 5 && mapPos(rm.step, 9, Assoc::After) == 9,
               "1.2 / 节点级映射：删除区间之前的位置不动");
    expectTrue(mapPos(rm.step, 10, Assoc::Before) == 10 && mapPos(rm.step, 10, Assoc::After) == 10,
               "1.2 / 节点级映射：删除区间起点两侧都留在起点");
    expectTrue(mapPos(rm.step, 11, Assoc::After) == 10 && mapPos(rm.step, 13, Assoc::After) == 10,
               "1.2 / 节点级映射：被摘子树内部的位置塌到它的起点（单向）");
    expectTrue(mapPos(rm.step, 14, Assoc::Before) == 10 && mapPos(rm.step, 16, Assoc::After) == 12,
               "1.2 / 节点级映射：之后的位置整体前移跨度 4");

    const NodeBuild in =
        makeInsertNode(tree, Pos{sec, 1}, makeFreshControlSpec());  // 插入点 7、跨度 3
    expectTrue(mapPos(in.step, 3, Assoc::After) == 3, "1.2 / 节点级映射：插入点之前的位置不动");
    expectTrue(mapPos(in.step, 7, Assoc::Before) == 7 && mapPos(in.step, 7, Assoc::After) == 10,
               "1.2 / 节点级映射：插入点上按 assoc 分到子树之前 / 之后");
    expectTrue(mapPos(in.step, 9, Assoc::Before) == 12,
               "1.2 / 节点级映射：插入点之后的位置整体后移跨度 3");

    // 插入一个区间之外的位置，映射过去再用逆映射搬回来必须回到原处。
    int bad = 0;
    for (int p = 0; p <= 16; ++p) {
        const FlowPos mid = mapPos(in.step, p, Assoc::After);
        if (mapPos(invertStep(in.step), mid, Assoc::After) != p) ++bad;
    }
    expectTrue(bad == 0, "1.2 / 节点级映射：插入步骤与它的逆映射往返一致（不一致 " +
                             std::to_string(bad) + " 个）");
}

/**
 * 1.2：节点级步骤的拒绝路径。构造与校验都要给出原因，不静默纠正。
 */
void checkNodeStepRejects() {
    {
        const NodeTree tree = makeEditTree();
        const NodeId sec = tree.firstSectionId();
        const NodeId text = tree.find(sec)->children[0]->id;
        const NodeBuild b = makeRemoveNode(tree, Pos{text, 0});
        expectTrue(!b.ok && b.error.find("不是容器") != std::string::npos,
                   "1.2 / 节点级：目标不是容器时构造被拒（" + b.error + "）");
    }
    {
        const NodeTree tree = makeEditTree();
        const NodeId sec = tree.firstSectionId();
        const NodeBuild b = makeRemoveNode(tree, Pos{sec, 5});
        expectTrue(!b.ok && b.error.find("没有孩子") != std::string::npos,
                   "1.2 / 节点级：槽位没有孩子时被拒（" + b.error + "）");
    }
    {
        NodeTree tree = makeEditTree();
        NodeSpec t;
        t.kind = NodeKind::TextRun;
        t.text = cps({0x61});
        const NodeBuild b = makeInsertNode(tree, Pos{tree.rootId(), 1}, t);
        expectTrue(!b.ok && b.error.find("不接受") != std::string::npos,
                   "1.2 / 节点级：根只收节，插入文本被拒（" + b.error + "）");
    }
    {
        NodeTree tree = makeEditTree();
        const NodeId sec = tree.firstSectionId();
        const NodeBuild b = makeInsertNode(tree, Pos{sec, 99}, makeFreshControlSpec());
        expectTrue(!b.ok && b.error.find("越界") != std::string::npos,
                   "1.2 / 节点级：槽位越界时被拒（" + b.error + "）");
    }
    {
        // 步骤过期：流位置与文档对不上时必须拒，不能照 at 硬插。
        NodeTree tree = makeEditTree();
        const NodeId sec = tree.firstSectionId();
        NodeBuild b = makeRemoveNode(tree, Pos{sec, 3});
        std::get<RemoveNode>(b.step).at_flow = 999;
        const StepResult r = applyStep(tree, b.step);
        expectTrue(!r.ok && r.error.find("对不上") != std::string::npos,
                   "1.2 / 节点级：流位置与文档对不上的步骤被拒（" + r.error + "）");
    }
    {
        // 同一步骤重复应用：子树已经挂在树上，再挂一次会拆散父子链。
        NodeTree tree = makeEditTree();
        const NodeId sec = tree.firstSectionId();
        const NodeBuild b = makeInsertNode(tree, Pos{sec, 1}, makeFreshControlSpec());
        expectTrue(applyStep(tree, b.step).ok, "1.2 / 节点级：插入先应用成功");
        const StepResult again = applyStep(tree, b.step);
        expectTrue(!again.ok && again.error.find("已经挂在树上") != std::string::npos,
                   "1.2 / 节点级：同一步骤重复应用被拒（" + again.error + "）");
    }
    {
        // 已知边界：含整棵节点的区间提取还没做（控件走节点级步骤），所以"选一段含控件的内容
        // 再复制"仍会被拒。写成断言是为了让"还没做"可发现，而不是让人以为已经支持。
        const NodeTree tree = makeEditTree();
        const ExtractResult ex = extractRange(tree, Range{5, 12});
        expectTrue(!ex.ok && ex.error.find("区间含") != std::string::npos,
                   "1.2 / 节点级：含控件的区间提取仍被拒（" + ex.error + "）");
    }
}

/**
 * 1.2：节点级步骤的权限。步骤本身只答"结构上能不能应用"，权限另算一层。
 */
void checkNodeStepGate() {
    NodeTree tree = makeEditTree();
    const NodeId sec = tree.firstSectionId();
    const NodeBuild b = makeRemoveNode(tree, Pos{sec, 3});  // 控件默认 structureLock = true

    const EditCheck form = canEditStep(tree, EditMode::Form, b.step);
    expectTrue(!form.allowed && form.reason.find("锁") != std::string::npos,
               "1.2 / 节点级权限：表单模式默认拒绝删除控件（" + form.reason + "）");
    expectTrue(canEditStep(tree, EditMode::Document, b.step).allowed,
               "1.2 / 节点级权限：文档模式放行");
    expectTrue(!canEditStep(tree, EditMode::ReadOnly, b.step).allowed,
               "1.2 / 节点级权限：只读模式拒绝");

    tree.find(tree.find(sec)->children[3]->id)->control.structureLock = false;
    expectTrue(canEditStep(tree, EditMode::Form, b.step).allowed,
               "1.2 / 节点级权限：控件声明可删之后表单模式放行");

    NodeSpec img;
    img.kind = NodeKind::Image;
    const NodeBuild ib = makeInsertNode(tree, Pos{sec, 1}, img);
    const EditCheck bad = canEditStep(tree, EditMode::Form, ib.step);
    expectTrue(!bad.allowed && bad.reason.find("只允许插入控件") != std::string::npos,
               "1.2 / 节点级权限：表单模式不接受插入图片（" + bad.reason + "）");

    InsertText it;
    it.at = 3;  // 落在控件之外的文本里
    it.cps = cps({0x41});
    const EditCheck content = canEditStep(tree, EditMode::Form, Step{it});
    expectTrue(!content.allowed && content.reason.find("控件") != std::string::npos,
               "1.2 / 节点级权限：内容级步骤仍按落点判，控件之外被拒（" + content.reason + "）");
}

/**
 * 1.5 的表单文书：静态文字、一个可编辑文本控件、同一个 key 的两处、外层含内层控件、
 * 一个跨段落标记的控件、一个选择类控件。流坐标与取值期望都在下面的断言里。
 */
struct FormDoc {
    NodeTree tree;
    NodeId chief = kInvalidNodeId;     // key=chief，内容 "AB"
    NodeId name1 = kInvalidNodeId;     // key=patient_name，内容 "C"
    NodeId name2 = kInvalidNodeId;     // 同 key 的第二处
    NodeId outer = kInvalidNodeId;     // key=outer，内部 "D" + 内层控件
    NodeId duration = kInvalidNodeId;  // key=duration，内容 "EF"（outer 的内层）
    NodeId multi = kInvalidNodeId;     // key=multi，内容 "G" + 段落标记 + "H"
    NodeId allergy = kInvalidNodeId;   // key=allergy，Bool 形态
};

FormDoc makeFormDoc() {
    FormDoc d;
    const NodeId sec = d.tree.firstSectionId();
    d.tree.setText(d.tree.appendChild(sec, NodeKind::TextRun), cps({0x4e3b, 0x8bc9}));

    d.chief = d.tree.appendChild(sec, NodeKind::ContentControl);
    d.tree.find(d.chief)->control.key = "chief";
    d.tree.setText(d.tree.appendChild(d.chief, NodeKind::TextRun), cps({0x41, 0x42}));
    d.tree.appendChild(sec, NodeKind::ParagraphMark);

    d.name1 = d.tree.appendChild(sec, NodeKind::ContentControl);
    d.tree.find(d.name1)->control.key = "patient_name";
    d.tree.setText(d.tree.appendChild(d.name1, NodeKind::TextRun), cps({0x43}));
    d.tree.appendChild(sec, NodeKind::ParagraphMark);

    d.name2 = d.tree.appendChild(sec, NodeKind::ContentControl);
    d.tree.find(d.name2)->control.key = "patient_name";
    d.tree.setText(d.tree.appendChild(d.name2, NodeKind::TextRun), cps({0x43}));
    d.tree.appendChild(sec, NodeKind::ParagraphMark);

    d.outer = d.tree.appendChild(sec, NodeKind::ContentControl);
    d.tree.find(d.outer)->control.key = "outer";
    d.tree.setText(d.tree.appendChild(d.outer, NodeKind::TextRun), cps({0x44}));
    d.duration = d.tree.appendChild(d.outer, NodeKind::ContentControl);
    d.tree.find(d.duration)->control.key = "duration";
    d.tree.setText(d.tree.appendChild(d.duration, NodeKind::TextRun), cps({0x45, 0x46}));
    d.tree.appendChild(sec, NodeKind::ParagraphMark);

    d.multi = d.tree.appendChild(sec, NodeKind::ContentControl);
    d.tree.find(d.multi)->control.key = "multi";
    d.tree.setText(d.tree.appendChild(d.multi, NodeKind::TextRun), cps({0x47}));
    d.tree.appendChild(d.multi, NodeKind::ParagraphMark);
    d.tree.setText(d.tree.appendChild(d.multi, NodeKind::TextRun), cps({0x48}));
    d.tree.appendChild(sec, NodeKind::ParagraphMark);

    d.allergy = d.tree.appendChild(sec, NodeKind::ContentControl);
    d.tree.find(d.allergy)->control.key = "allergy";
    d.tree.find(d.allergy)->control.controlType = "checkBox";
    d.tree.find(d.allergy)->control.valueShape = ValueShape::Bool;
    d.tree.appendChild(sec, NodeKind::ParagraphMark);
    return d;
}

/**
 * 1.5：控件索引。两张表要与重建逐项相同，且文本级步骤不为它付代价。
 */
void checkControlIndex() {
    FormDoc d = makeFormDoc();
    const NodeId sec = d.tree.firstSectionId();
    ControlIndex index;
    index.rebuild(d.tree);

    expectTrue(index.size() == 7,
               "1.5 / 索引：七个控件（实得 " + std::to_string(index.size()) + "）");
    expectTrue(index.has(d.chief) && !index.has(sec) && index.keyOf(sec) == nullptr,
               "1.5 / 索引：表一回答\"这个 id 是不是控件\"");
    expectTrue(index.byKey("patient_name").size() == 2 && index.byKey("chief").size() == 1 &&
                   index.byKey("没有这个 key").empty(),
               "1.5 / 索引：表二是一对多（同 key 两个控件），没有的 key 返回空表");
    expectTrue(controlIds(d.tree) == std::vector<NodeId>{d.chief, d.name1, d.name2, d.outer,
                                                         d.duration, d.multi, d.allergy},
               "1.5 / 索引：控件的枚举顺序是文档顺序（外层在它的内层之前）");

    // 文本级步骤：索引什么都不用做，且结果与重建一致。
    InsertText ins;
    ins.at = 1;
    ins.cps = cps({0x5a});
    expectTrue(index.onStep(d.tree, Step{ins}) && index.size() == 7,
               "1.5 / 索引：文本级步骤不为索引付代价");

    // 插入一个控件：增量跟上之后与重建逐项相同。
    NodeSpec spec;
    spec.kind = NodeKind::ContentControl;
    spec.control.key = "patient_name";  // 故意用已有 key：表二是一对多
    const NodeBuild add = makeInsertNode(d.tree, Pos{sec, 1}, spec);
    expectTrue(add.ok && applyStep(d.tree, add.step).ok && index.onStep(d.tree, add.step),
               "1.5 / 索引：插入控件后增量跟得上" + (add.ok ? "" : "（" + add.error + "）"));
    expectTrue(index.size() == 8 && index.byKey("patient_name").size() == 3,
               "1.5 / 索引：新控件按它的 key 进了表二（同 key 三处）");

    ControlIndex fresh;
    fresh.rebuild(d.tree);
    bool same = fresh.size() == index.size();
    for (NodeId id : controlIds(d.tree)) {
        const std::string* a = index.keyOf(id);
        const std::string* b = fresh.keyOf(id);
        same = same && a != nullptr && b != nullptr && *a == *b;
    }
    expectTrue(same && index.byKey("patient_name") == fresh.byKey("patient_name"),
               "1.5 / 索引：增量维护的结果与重建逐项相同");

    // 删掉一个不是控件的孩子（段末的段落标记）：表不动。
    const int last_slot = static_cast<int>(d.tree.find(sec)->children.size()) - 1;
    const NodeBuild drop_mark = makeRemoveNode(d.tree, Pos{sec, last_slot});
    expectTrue(drop_mark.ok && applyStep(d.tree, drop_mark.step).ok &&
                   index.onStep(d.tree, drop_mark.step) && index.size() == 8,
               "1.5 / 索引：删掉一个不是控件的孩子，表不变");

    // 删掉刚插入的那个控件：表二少一项，仍与重建一致。
    const NodeBuild drop_cc = makeRemoveNode(d.tree, Pos{sec, 1});
    expectTrue(
        drop_cc.ok && applyStep(d.tree, drop_cc.step).ok && index.onStep(d.tree, drop_cc.step),
        "1.5 / 索引：删除控件后增量跟得上");
    ControlIndex fresh2;
    fresh2.rebuild(d.tree);
    expectTrue(index.size() == 7 && index.byKey("patient_name").size() == 2 &&
                   index.byKey("patient_name") == fresh2.byKey("patient_name"),
               "1.5 / 索引：删除之后与重建一致（同 key 回到两处）");

    // 步骤里没有子树身份时（序列化还原的形状）跟不上，调用方必须重建。
    InsertNode no_identity;
    no_identity.at = Pos{sec, 0};
    no_identity.subtree.kind = NodeKind::ContentControl;
    expectTrue(!index.onStep(d.tree, Step{no_identity}),
               "1.5 / 索引：步骤没记下子树身份时返回\"跟不上\"，由调用方重建");
}

/**
 * 1.5：取值。文本类递归拼接（含嵌套控件）、段落标记拼成换行、隐藏的子树不算可见文字、
 * 选择类读属性上的值。
 */
void checkReadValues() {
    FormDoc d = makeFormDoc();
    ControlIndex index;
    index.rebuild(d.tree);

    expectTrue(getValue(d.tree, d.chief).ok && getValue(d.tree, d.chief).text == "AB",
               "1.5 / 取值：文本控件的值就是它的内容");
    expectTrue(getValue(d.tree, d.outer).text == "DEF",
               "1.5 / 取值：外层取的是整棵子树的文字，含内层控件里的（DEF）");
    expectTrue(getValue(d.tree, d.duration).text == "EF", "1.5 / 取值：内层自己取是 EF");
    expectTrue(getValue(d.tree, d.multi).text == "G\nH",
               "1.5 / 取值：段落标记拼成一个换行，多段控件不会粘成一行");

    const Reading flag = getValue(d.tree, d.allergy);
    expectTrue(flag.ok && flag.shape == ValueShape::Bool && !flag.flag,
               "1.5 / 取值：选择类控件的值在属性上（shape=bool，值为假）");

    // 隐藏的子树不算"可见文字"：取值取的是用户在表单与打印里能看到的那份。
    d.tree.find(d.duration)->hidden = true;
    expectTrue(getValue(d.tree, d.outer).text == "D",
               "1.5 / 取值：隐藏的子树不计入（外层只剩它自己的静态文字）");
    d.tree.find(d.duration)->hidden = false;

    // 按 key：同 key 值一致时不算不一致；改了一处之后要能发现。
    const Reading one = getValueByKey(d.tree, index, "patient_name");
    expectTrue(one.ok && one.text == "C" && !one.inconsistent,
               "1.5 / 取值：按 key 取到值，同 key 一致时不报警");
    expectTrue(!getValueByKey(d.tree, index, "没有这个 key").ok,
               "1.5 / 取值：没有这个 key 时显式失败（不静默返回空值）");

    d.tree.setText(d.tree.find(d.name2)->children[0]->id, cps({0x44}));
    const Reading two = getValueByKey(d.tree, index, "patient_name");
    expectTrue(two.inconsistent, "1.5 / 取值：同 key 值不一致时标出来（不静默取一个）");
    const std::vector<std::string> bad = inconsistentKeys(d.tree, index);
    expectTrue(bad.size() == 1 && bad[0] == "patient_name",
               "1.5 / 取值：不一致清单按 key 去重（" + std::to_string(bad.size()) + " 条）");

    const std::vector<ControlInfo> infos = listControls(d.tree);
    expectTrue(infos.size() == 7 && infos[0].id == d.chief && infos[0].value.text == "AB" &&
                   infos[0].key == "chief",
               "1.5 / 取值：listControls 给出轻量描述（含值），不含节点指针");
    expectTrue(infos[6].shape == ValueShape::Bool && !infos[6].required && infos[6].structureLock,
               "1.5 / 取值：描述里带值形态与四个属性");
}

/**
 * 1.5：批量设值。一次调用合成一个事务 ⇒ 一个撤销步骤；撤销之后逐字段回到原样（含身份）。
 */
void checkSetValues() {
    FormDoc d = makeFormDoc();
    UndoStack undo(d.tree);
    ControlIndex index;
    index.rebuild(d.tree);
    ControlApi api(d.tree, undo, index);

    const std::vector<std::string> before = nodeIdsSnapshot(d.tree);
    const int depth = undo.undoDepth();

    ValueEdit e;
    e.id = d.chief;
    e.value.text = "XY";
    const BatchResult one = api.setValues({e});
    expectTrue(one.ok && one.changed == 1,
               "1.5 / 设值：设一个控件成功" + (one.ok ? "" : "（" + one.error + "）"));
    expectTrue(getValue(d.tree, d.chief).text == "XY", "1.5 / 设值：取回来是新值");
    expectTrue(undo.undoDepth() == depth + 1, "1.5 / 设值：一次调用只产生一个撤销步骤");
    expectClean(d.tree, "1.5 / 设值：设完之后不变量全绿");

    undo.undo(nullptr);
    expectTrue(nodeIdsSnapshot(d.tree) == before,
               "1.5 / 设值：撤销之后逐字段回到原样，且节点身份不变");

    // 批量：三个控件一次设，仍然只多一个撤销步骤。
    const int depth2 = undo.undoDepth();
    ValueEdit a;
    a.id = d.chief;
    a.value.text = "1";
    ValueEdit b;
    b.id = d.name1;
    b.value.text = "2";
    ValueEdit c;
    c.id = d.multi;
    c.value.text = "3";
    const BatchResult three = api.setValues({a, b, c});
    expectTrue(three.ok && three.changed == 3 && undo.undoDepth() == depth2 + 1,
               "1.5 / 设值：三个控件一次设，只多一个撤销步骤（changed " +
                   std::to_string(three.changed) + "，深度 " +
                   std::to_string(undo.undoDepth() - depth2) + "）" +
                   (three.ok ? "" : "（" + three.error + "）"));
    expectTrue(getValue(d.tree, d.chief).text == "1" && getValue(d.tree, d.name1).text == "2" &&
                   getValue(d.tree, d.multi).text == "3",
               "1.5 / 设值：三个都改了");
    undo.undo(nullptr);
    expectTrue(getValue(d.tree, d.chief).text == "AB" && getValue(d.tree, d.multi).text == "G\nH",
               "1.5 / 设值：一次撤销回退整批");

    // 同一次调用里同一个目标出现多次：后者覆盖前者，仍算改了一个。
    ValueEdit first;
    first.id = d.chief;
    first.value.text = "先";
    ValueEdit second;
    second.id = d.chief;
    second.value.text = "后";
    const int depth3 = undo.undoDepth();
    const BatchResult twice = api.setValues({first, second});
    expectTrue(twice.ok && twice.changed == 1 && getValue(d.tree, d.chief).text == "后" &&
                   undo.undoDepth() == depth3 + 1,
               "1.5 / 设值：同一目标重复时后者覆盖前者，仍是一个撤销步骤");

    // 按 key：一次改掉同 key 的全部（正文与页脚镜像同一字段的做法）。
    const int depth4 = undo.undoDepth();
    ValueEdit by_key;
    by_key.key = "patient_name";
    by_key.value.text = "李四";
    const BatchResult keyed = api.setValuesByKey({by_key});
    expectTrue(keyed.ok && keyed.changed == 2 && undo.undoDepth() == depth4 + 1,
               "1.5 / 设值：按 key 一次改掉同 key 的两处，仍是一个撤销步骤（changed " +
                   std::to_string(keyed.changed) + "）" +
                   (keyed.ok ? "" : "（" + keyed.error + "）"));
    expectTrue(getValue(d.tree, d.name1).text == "李四" &&
                   getValue(d.tree, d.name2).text == "李四" &&
                   !getValueByKey(d.tree, index, "patient_name").inconsistent,
               "1.5 / 设值：按 key 设值把两处置成同一个值（不一致消失）");

    // 空值：内容清空，控件留着（空控件有身份，与空 run 不同类）。
    ValueEdit clear;
    clear.id = d.chief;
    const BatchResult emptied = api.setValues({clear});
    expectTrue(emptied.ok && getValue(d.tree, d.chief).text.empty() &&
                   d.tree.find(d.chief)->children.empty(),
               "1.5 / 设值：设空串把内容清掉，控件自己留着");
    expectClean(d.tree, "1.5 / 设值：清空之后不变量全绿");
}

/**
 * 1.5：设值的拒绝路径。拒绝带原因、不产生任何编辑步骤、文档逐字段不变。
 */
void checkSetValuesRejects() {
    FormDoc d = makeFormDoc();
    UndoStack undo(d.tree);
    ControlIndex index;
    index.rebuild(d.tree);
    ControlApi api(d.tree, undo, index);

    const int depth = undo.undoDepth();

    // 内含嵌套控件：整体设值会把内层连同它的 key 与值一起删掉，一律拒绝。
    ValueEdit nested;
    nested.id = d.outer;
    nested.value.text = "x";
    const BatchResult r1 = api.setValues({nested});
    expectTrue(!r1.ok && r1.error.find("嵌套字段") != std::string::npos,
               "1.5 / 设值拒绝：内含嵌套字段时拒绝并说明（" + r1.error + "）");

    // 内含不可半分的原子：同一条判据。
    NodeSpec img;
    img.kind = NodeKind::Image;
    const NodeBuild add = makeInsertNode(d.tree, Pos{d.chief, 1}, img);
    std::vector<std::string> with_image;
    if (add.ok && applyStep(d.tree, add.step).ok) {
        with_image = nodeIdsSnapshot(d.tree);
        ValueEdit with_atom;
        with_atom.id = d.chief;
        with_atom.value.text = "x";
        const BatchResult r2 = api.setValues({with_atom});
        expectTrue(!r2.ok && r2.error.find("Image") != std::string::npos,
                   "1.5 / 设值拒绝：内含图片原子时拒绝（" + r2.error + "）");
    }

    // 目标不是控件（这里给的是一个节）：显式拒绝，不静默当成功。
    ValueEdit not_ctrl;
    not_ctrl.id = d.tree.firstSectionId();
    not_ctrl.value.text = "x";
    const BatchResult r3 = api.setValues({not_ctrl});
    expectTrue(!r3.ok && r3.error.find("不是内容控件") != std::string::npos,
               "1.5 / 设值拒绝：目标不是控件时拒绝（" + r3.error + "）");

    // 三条拒绝都不该动过文档，也不该产生撤销步骤。
    expectTrue(undo.undoDepth() == depth, "1.5 / 设值拒绝：一次撤销步骤都没产生");
    expectTrue(!with_image.empty() && nodeIdsSnapshot(d.tree) == with_image,
               "1.5 / 设值拒绝：文档逐字段不变（只多了上面显式插入的那个图片节点）");
}

/**
 * 1.5：批量设属性。一次一个撤销步骤；含 hidden 的批次要重排（呈现态流结构变了），
 * 只改 readonly 的不重排。
 */
void checkSetProperties() {
    FormDoc d = makeFormDoc();
    UndoStack undo(d.tree);
    ControlIndex index;
    index.rebuild(d.tree);
    ControlApi api(d.tree, undo, index);

    const int design_before = FlowIndex(d.tree, FlowVisibility::Design).size();
    const int display_before = FlowIndex(d.tree, FlowVisibility::Display).size();

    // 只改 readonly：不重排（三种口径的流位置都不变）。
    PropEdit ro;
    ro.id = d.chief;
    ro.readonly = true;
    const int depth = undo.undoDepth();
    const BatchResult r1 = api.setProperties({ro});
    expectTrue(r1.ok && r1.changed == 1 && !r1.needs_relayout && undo.undoDepth() == depth + 1,
               "1.5 / 设属性：改 readonly 成功、不重排、一个撤销步骤");
    expectTrue(getProperties(d.tree, {d.chief})[0].readonly, "1.5 / 设属性：读回来是改过的值");
    expectTrue(FlowIndex(d.tree, FlowVisibility::Design).size() == design_before &&
                   FlowIndex(d.tree, FlowVisibility::Display).size() == display_before,
               "1.5 / 设属性：只改 readonly 的批次不重排");
    undo.undo(nullptr);
    expectTrue(!getProperties(d.tree, {d.chief})[0].readonly, "1.5 / 设属性：撤销回退属性");

    // 改 hidden：要重排（呈现态少了那棵子树，编辑态不变）。
    PropEdit hid;
    hid.id = d.duration;
    hid.hidden = true;
    const BatchResult r2 = api.setProperties({hid});
    expectTrue(r2.ok && r2.needs_relayout, "1.5 / 设属性：含 hidden 的批次标成要重排");
    expectTrue(FlowIndex(d.tree, FlowVisibility::Design).size() == design_before &&
                   FlowIndex(d.tree, FlowVisibility::Display).size() < display_before,
               "1.5 / 设属性：hidden 改变呈现态的流结构，编辑态不变");
    undo.undo(nullptr);
    expectTrue(FlowIndex(d.tree, FlowVisibility::Display).size() == display_before,
               "1.5 / 设属性：撤销之后呈现态的流位置回到原数");

    // 按 key 批量改属性：同 key 两处一起改，仍是一个撤销步骤。
    PropEdit keyed;
    keyed.key = "patient_name";
    keyed.required = true;
    const int depth2 = undo.undoDepth();
    const BatchResult r3 = api.setPropertiesByKey({keyed});
    expectTrue(r3.ok && r3.changed == 2 && undo.undoDepth() == depth2 + 1,
               "1.5 / 设属性：按 key 一次改掉同 key 的两处，一个撤销步骤");
    expectTrue(getProperties(d.tree, {d.name1, d.name2})[0].required &&
                   getProperties(d.tree, {d.name1, d.name2})[1].required,
               "1.5 / 设属性：两处都改了");

    // 值与属性在同一批里设：仍然只有一个撤销步骤。
    const int depth3 = undo.undoDepth();
    api.beginBatch();
    ValueEdit v;
    v.id = d.chief;
    v.value.text = "批";
    const BatchResult bv = api.setValues({v});
    PropEdit p;
    p.id = d.chief;
    p.hidden = false;
    p.readonly = false;
    const BatchResult bp = api.setProperties({p});
    const BatchResult commit = api.commitBatch();
    expectTrue(bv.ok && bp.ok && commit.ok && commit.changed == 3 && undo.undoDepth() == depth3 + 1,
               "1.5 / 设属性：值与属性在同一批里设，仍然只有一个撤销步骤");
    expectTrue(
        getValue(d.tree, d.chief).text == "批" && !getProperties(d.tree, {d.chief})[0].readonly,
        "1.5 / 设属性：批里的值与属性都生效");
    expectClean(d.tree, "1.5 / 设属性：批量之后不变量全绿");
}

/**
 * 1.5：串起来跑一轮 —— 插入控件、设值、改属性、删控件，每一步之后索引都与重建一致。
 */
void checkControlApiSequence() {
    FormDoc d = makeFormDoc();
    UndoStack undo(d.tree);
    ControlIndex index;
    index.rebuild(d.tree);
    ControlApi api(d.tree, undo, index);
    const NodeId sec = d.tree.firstSectionId();

    int steps = 0;
    for (int round = 0; round < 3; ++round) {
        NodeSpec spec;
        spec.kind = NodeKind::ContentControl;
        spec.control.key = "round_" + std::to_string(round);
        const NodeBuild add = makeInsertNode(
            d.tree, Pos{sec, static_cast<int>(d.tree.find(sec)->children.size())}, spec);
        if (!add.ok) continue;
        // 插入也走事务层（与 API 内部同一条路），撤销才回得去。
        Transaction tx;
        tx.steps = {add.step};
        if (undo.commit(std::move(tx)).ok) {
            index.onStep(d.tree, add.step);
            ++steps;

            ValueEdit e;
            e.key = "round_" + std::to_string(round);
            e.value.text = "v";
            if (api.setValuesByKey({e}).ok) ++steps;

            PropEdit p;
            p.key = "round_" + std::to_string(round);
            p.required = true;
            if (api.setPropertiesByKey({p}).ok) ++steps;
        }
    }
    expectTrue(steps == 9, "1.5 / 串起来：插入 + 设值 + 设属性各三轮，共 9 次成功调用（实得 " +
                               std::to_string(steps) + "）");

    ControlIndex fresh;
    fresh.rebuild(d.tree);
    bool same = fresh.size() == index.size();
    for (NodeId id : controlIds(d.tree)) {
        const std::string* a = index.keyOf(id);
        const std::string* b = fresh.keyOf(id);
        same = same && a != nullptr && b != nullptr && *a == *b;
    }
    expectTrue(same && index.size() == 10,
               "1.5 / 串起来：一轮混合编辑之后，增量索引与重建逐项相同（10 个控件）");
    expectClean(d.tree, "1.5 / 串起来：不变量全绿");

    // 全部撤销：回到起点。
    while (undo.canUndo()) {
        undo.undo(nullptr);
    }
    ControlIndex after;
    after.rebuild(d.tree);
    expectTrue(after.size() == 7 && controlIds(d.tree).size() == 7,
               "1.5 / 串起来：全部撤销之后回到七个控件");
    expectTrue(nodeIdsSnapshot(d.tree) == nodeIdsSnapshot(makeFormDoc().tree),
               "1.5 / 串起来：全部撤销之后文档逐字段与初始相同（含身份）");
}

/**
 * 1.5 的后一半：外观属性。它是设计期属性（改它等于重新定义控件），不走属性入口，
 * 直接写在记录上；读回来要逐项相同，且 placeholder 不进内容、不被取值取出来。
 */
void checkAppearance() {
    FormDoc d = makeFormDoc();
    ControlIndex index;
    index.rebuild(d.tree);

    const Appearance a = getProperties(d.tree, {d.chief})[0].appearance;
    expectTrue(a.width.kind == LengthKind::Auto && a.overflow == Overflow::Expand &&
                   a.underline == Underline::None && a.align == Align::Start && !a.border.left.on &&
                   !a.border.top.on && !a.border.bottom.on && a.glyph.kind.empty() &&
                   a.placeholder.empty() && a.styleRef.empty(),
               "1.5b / 外观：默认是不限宽、撑开、不画线与图元、无提示文字");

    Node* n = d.tree.find(d.chief);
    n->control.appearance.width = Length::chars(6);
    n->control.appearance.overflow = Overflow::Clip;
    n->control.appearance.underline = Underline::BlankOnly;
    n->control.appearance.border.bottom.on = true;
    n->control.appearance.border.bottom.style = BorderStyle::Dotted;
    n->control.appearance.glyph.kind = "check";
    n->control.appearance.align = Align::Center;
    n->control.appearance.placeholder = "请填写主诉";
    n->control.appearance.styleRef = "field";
    n->control.appearance.invalidStyleRef = "field-invalid";
    n->control.validateRef = "idCard";

    const ControlInfo after = getProperties(d.tree, {d.chief})[0];
    const Appearance b = after.appearance;
    expectTrue(b.width.kind == LengthKind::Chars && b.width.num == 6 &&
                   b.overflow == Overflow::Clip && b.underline == Underline::BlankOnly &&
                   b.align == Align::Center && b.glyph.kind == "check",
               "1.5b / 外观：宽度按类型读回（chars 6）、三档与图元名逐项相同");
    expectTrue(b.border.bottom.on && b.border.bottom.style == BorderStyle::Dotted &&
                   !b.border.top.on && !b.border.left.on,
               "1.5b / 外观：四边各自独立（只画了下边，其余三边仍是关的）");
    expectTrue(after.validateRef == "idCard" && b.styleRef == "field" &&
                   b.invalidStyleRef == "field-invalid",
               "1.5b / 外观：校验规则与两条样式引用都是名字（绑定是数据，不是代码）");
    expectTrue(getValue(d.tree, d.chief).text == "AB",
               "1.5b / 外观：placeholder 是独立字段，不进内容、不被取值取出来");
    expectTrue(printLength(b.width) == "chars:6" &&
                   printLength(b.border.bottom.width) == "abs:" + std::to_string(ptToEmu(0.5)),
               "1.5b / 外观：宽度的文本形式不丢类型（" + printLength(b.width) + "）");
    expectTrue(std::string(overflowName(b.overflow)) == "clip" &&
                   std::string(underlineName(b.underline)) == "blankOnly" &&
                   std::string(borderStyleName(BorderStyle::Dotted)) == "dotted" &&
                   std::string(alignName(Align::Center)) == "center",
               "1.5b / 外观：档位的名字可读，用于诊断与报告");
}

/**
 * 1.5 的后一半：属性侧的值（Bool / Enum / List / Resource）。与文本类共用一条 API，
 * 但落点不同：这一半走 SetControlValue 步骤，文本类走内容级的节点步骤。
 */
void checkValueShapes() {
    FormDoc d = makeFormDoc();
    UndoStack undo(d.tree);
    ControlIndex index;
    index.rebuild(d.tree);
    ControlApi api(d.tree, undo, index);
    const NodeId sec = d.tree.firstSectionId();

    const std::vector<std::string> before = nodeIdsSnapshot(d.tree);
    const int depth = undo.undoDepth();

    ValueEdit flag;
    flag.id = d.allergy;
    flag.value.flag = true;
    const BatchResult r1 = api.setValues({flag});
    expectTrue(
        r1.ok && r1.changed == 1 && getValue(d.tree, d.allergy).flag &&
            undo.undoDepth() == depth + 1,
        "1.5b / 非文本设值：Bool 写在记录上，一个撤销步骤" + (r1.ok ? "" : "（" + r1.error + "）"));
    expectClean(d.tree, "1.5b / 非文本设值：设完之后不变量全绿");
    undo.undo(nullptr);
    expectTrue(nodeIdsSnapshot(d.tree) == before && !getValue(d.tree, d.allergy).flag,
               "1.5b / 非文本设值：撤销之后逐字段回到原样（含身份）");

    // 三种形态各建一个控件：枚举、多选集合、资源。
    NodeSpec spec;
    spec.kind = NodeKind::ContentControl;
    auto plant = [&](const char* type, ValueShape shape) -> NodeId {
        spec.control = ControlSpec{};
        spec.control.key = type;
        spec.control.controlType = type;
        spec.control.valueShape = shape;
        const NodeBuild b = makeInsertNode(d.tree, Pos{sec, 0}, spec);
        if (!b.ok) return kInvalidNodeId;
        Transaction tx;
        tx.steps = {b.step};
        if (!undo.commit(std::move(tx)).ok) return kInvalidNodeId;
        index.onStep(d.tree, b.step);
        return std::get<InsertNode>(b.step).detached;
    };
    const NodeId choice = plant("dropdownList", ValueShape::Enum);
    const NodeId tags = plant("checkBoxGroup", ValueShape::List);
    const NodeId photo = plant("picture", ValueShape::Resource);

    ValueEdit en;
    en.id = choice;
    en.value.text = "已婚";
    ValueEdit li;
    li.id = tags;
    li.value.items = {"高血压", "糖尿病"};
    ValueEdit re;
    re.id = photo;
    re.value.text = "res://x1";
    const int depth2 = undo.undoDepth();
    const BatchResult r2 = api.setValues({en, li, re});
    expectTrue(r2.ok && r2.changed == 3 && undo.undoDepth() == depth2 + 1,
               "1.5b / 非文本设值：三种形态一次设，仍只多一个撤销步骤" +
                   (r2.ok ? "" : "（" + r2.error + "）"));
    expectTrue(getValue(d.tree, choice).text == "已婚" &&
                   getValue(d.tree, tags).items.size() == 2 &&
                   getValue(d.tree, tags).items[1] == "糖尿病" &&
                   getValue(d.tree, photo).text == "res://x1",
               "1.5b / 非文本设值：三种形态都取得到");
    undo.undo(nullptr);
    expectTrue(getValue(d.tree, choice).text.empty() && getValue(d.tree, tags).items.empty(),
               "1.5b / 非文本设值：一次撤销回退整批");

    // 文本类仍走内容路径：同一条 API，落点不同。
    ValueEdit txt;
    txt.id = d.chief;
    txt.value.text = "赵";
    expectTrue(api.setValues({txt}).ok && getValue(d.tree, d.chief).text == "赵" &&
                   !d.tree.find(d.chief)->children.empty(),
               "1.5b / 非文本设值：文本类走内容路径，与非文本形态共用一条 API");

    // 步骤层的三条拒绝：原值不符（过期）、文本类走属性侧的值步骤、目标不在文档里。
    SetControlValue stale;
    stale.id = d.allergy;
    stale.value.flag = true;
    stale.before.flag = true;  // 当前是 false，对不上
    const StepResult sr = canApply(d.tree, Step{stale});
    expectTrue(!sr.ok && sr.error.find("原值不符") != std::string::npos,
               "1.5b / 非文本设值：原值不符的步骤被拒（" + sr.error + "）");
    SetControlValue on_text;
    on_text.id = d.chief;
    on_text.before.text = "赵";
    const StepResult sr2 = canApply(d.tree, Step{on_text});
    expectTrue(!sr2.ok && sr2.error.find("内容") != std::string::npos,
               "1.5b / 非文本设值：文本类不走属性侧的值步骤（" + sr2.error + "）");
}

/**
 * 1.5 的后一半：异步裁决门。判据是三条 —— 等待期间文档与撤销栈逐字段不变、
 * Allow 之后才应用（此时才产生撤销步骤）、Deny 之后值与交互前一致。
 */
void checkAsyncGate() {
    FormDoc d = makeFormDoc();
    UndoStack undo(d.tree);
    ControlIndex index;
    index.rebuild(d.tree);
    ControlApi api(d.tree, undo, index);
    ChangeGate gate(api);
    const NodeId sec = d.tree.firstSectionId();

    const std::vector<std::string> before = nodeIdsSnapshot(d.tree);
    const int depth = undo.undoDepth();

    ValueEdit e;
    e.id = d.allergy;
    e.value.flag = true;
    const BeginResult b = gate.begin(e);
    expectTrue(b.ok && b.token > 0, "1.5b / 等待：登记候选成功（" + b.error + "）");
    expectTrue(gate.isPending(d.allergy) && gate.pendingCount() == 1 &&
                   gate.stateOf(d.allergy).state == ValidationState::Pending,
               "1.5b / 等待：控件进 pending，只剩它一个等待");
    expectTrue(nodeIdsSnapshot(d.tree) == before && undo.undoDepth() == depth &&
                   !getValue(d.tree, d.allergy).flag,
               "1.5b / 等待：文档与撤销栈逐字段不变，也不显示新值");

    const GateResult ok = gate.allow(b.token);
    expectTrue(ok.outcome == GateOutcome::Applied && getValue(d.tree, d.allergy).flag &&
                   gate.pendingCount() == 0 &&
                   gate.stateOf(d.allergy).state == ValidationState::None,
               "1.5b / 裁决：Allow 之后值才变更、pending 清空");
    expectTrue(undo.undoDepth() == depth + 1, "1.5b / 裁决：Allow 之后才产生撤销步骤");
    undo.undo(nullptr);
    expectTrue(nodeIdsSnapshot(d.tree) == before, "1.5b / 裁决：撤销回到交互前，逐字段相同");

    const BeginResult b2 = gate.begin(e);
    const GateResult no = gate.deny(b2.token, "该项与既往史冲突");
    const Validation v = gate.stateOf(d.allergy);
    expectTrue(no.outcome == GateOutcome::Denied && v.state == ValidationState::Invalid &&
                   v.message == "该项与既往史冲突" && !getValue(d.tree, d.allergy).flag &&
                   undo.undoDepth() == depth && nodeIdsSnapshot(d.tree) == before,
               "1.5b / 裁决：Deny 丢弃候选、值不变、标 invalid 带原因，不多出撤销步骤");

    const GateResult to = gate.expire(gate.begin(e).token);
    expectTrue(to.outcome == GateOutcome::Denied && to.message.find("超时") != std::string::npos &&
                   !getValue(d.tree, d.allergy).flag,
               "1.5b / 裁决：超时按拒绝处理，不静默放行（" + to.message + "）");

    // 同一控件再次交互：前一次按取消丢弃，旧结果不应用也不误报失败。
    const BeginResult old_one = gate.begin(e);
    const BeginResult new_one = gate.begin(e);
    expectTrue(old_one.token != new_one.token && gate.pendingCount() == 1,
               "1.5b / 裁决：同控件再次交互只剩一个等待（最后交互为准）");
    const GateResult stale = gate.allow(old_one.token);
    expectTrue(stale.outcome == GateOutcome::Dropped && !getValue(d.tree, d.allergy).flag &&
                   undo.undoDepth() == depth,
               "1.5b / 裁决：旧结果到达时按已取消失效，不应用也不误报失败");
    expectTrue(gate.allow(new_one.token).outcome == GateOutcome::Applied &&
                   getValue(d.tree, d.allergy).flag,
               "1.5b / 裁决：后一次交互照常裁决");
    undo.undo(nullptr);

    // 锚点失效：等待期间控件被删掉，结果到达时丢弃、不做补偿。
    // 判"还在不在"不能只看 find 非空 —— 被摘下的子树留在池里，按 id 照样查得到。
    auto drop = [&](NodeId target) {
        int slot = -1;
        const Node* s = d.tree.find(sec);
        for (size_t i = 0; i < s->children.size(); ++i) {
            if (s->children[i]->id == target) slot = static_cast<int>(i);
        }
        if (slot < 0) return false;
        const NodeBuild del = makeRemoveNode(d.tree, Pos{sec, slot});
        if (!del.ok) return false;
        Transaction tx;
        tx.steps = {del.step};
        if (!undo.commit(std::move(tx)).ok) return false;
        index.onStep(d.tree, del.step);
        return true;
    };
    const BeginResult armed = gate.begin(e);
    expectTrue(armed.ok, "1.5b / 裁决：目标还在文档里时登记成功");
    expectTrue(drop(d.allergy), "1.5b / 裁决：等待期间删掉目标控件");
    const GateResult gone = gate.allow(armed.token);
    expectTrue(gone.outcome == GateOutcome::Dropped &&
                   gone.message.find("失效") != std::string::npos && gate.pendingCount() == 0,
               "1.5b / 裁决：目标已失效时丢弃，不做补偿动作（" + gone.message + "）");
    expectTrue(!isAttached(d.tree, d.allergy) && d.tree.find(d.allergy) != nullptr,
               "1.5b / 裁决：被删的控件留在池里所以查得到，但它已经不在文档上");
    undo.undo(nullptr);
    expectTrue(isAttached(d.tree, d.allergy), "1.5b / 裁决：撤销之后控件带着身份挂回来了");

    // sweepStale：目标不在文档里时，等待与校验状态一起清掉。
    expectTrue(gate.begin(e).ok, "1.5b / 清理：重新登记一次等待");
    expectTrue(drop(d.allergy) && gate.pendingCount() == 1,
               "1.5b / 清理：目标被删时等待还挂着（尚未清理）");
    gate.sweepStale();
    expectTrue(gate.pendingCount() == 0 && gate.stateOf(d.allergy).state == ValidationState::None,
               "1.5b / 清理：sweepStale 把失效目标的等待与状态一起清掉");
    undo.undo(nullptr);
    expectTrue(isAttached(d.tree, d.allergy), "1.5b / 清理：撤销之后控件挂回来了");

    // 主动置位：晚到的校验结果没有对应的事务，只能由宿主直接设状态。
    gate.setValidation(d.chief, ValidationState::Invalid, "格式不符");
    expectTrue(gate.stateOf(d.chief).state == ValidationState::Invalid &&
                   gate.stateOf(d.chief).message == "格式不符",
               "1.5b / 状态：宿主可主动置位（晚到的结果已经不属于任何一次交互）");

    // 只拦用户交互：编程设值不进门，也不取消等待。
    const BeginResult b5 = gate.begin(e);
    ValueEdit prog;
    prog.id = d.allergy;
    prog.value.flag = true;
    expectTrue(api.setValues({prog}).ok && getValue(d.tree, d.allergy).flag,
               "1.5b / 分工：编程设值不进门，照常立刻生效");
    expectTrue(gate.isPending(d.allergy), "1.5b / 分工：编程设值不取消等待");
    expectTrue(gate.deny(b5.token, "不通过").outcome == GateOutcome::Denied &&
                   getValue(d.tree, d.allergy).flag,
               "1.5b / 分工：随后的 Deny 只丢候选，不改已经生效的编程值");
}

/**
 * 两份完整段落格式逐字段比。返回空串表示一致，否则返回第一处不同的字段名。
 *
 * 显式列出每个字段而不是靠一个"相等"：漏比一个字段，判据就变成一句空话（与 0.4 的 diffBlock
 * 同一个理由）。
 */
std::string diffPara(const ParaFormat& a, const ParaFormat& b) {
    if (a.font_index != b.font_index) return "font_index";
    if (!(a.size == b.size)) return "size";
    if (a.wm.inline_axis != b.wm.inline_axis || a.wm.block_flow != b.wm.block_flow ||
        a.wm.orientation != b.wm.orientation) {
        return "wm";
    }
    if (a.line_factor != b.line_factor) return "line_factor";
    if (!(a.space_after == b.space_after)) return "space_after";
    if (a.page_break_before != b.page_break_before) return "page_break_before";
    const std::string la = a.language != nullptr ? a.language : "";
    const std::string lb = b.language != nullptr ? b.language : "";
    if (la != lb) return "language";
    if (!(a.color == b.color)) return "color";
    if (a.fallback_font_indices != b.fallback_font_indices) return "fallback_font_indices";
    return "";
}

/**
 * 1.6：三层解析。判据是"与手算一致"—— 期望值整份手写出来逐字段核，而不是只查其中一两个字段
 * （只查一个字段的话，"继承链搭错了但恰好那一个字段相同"会蒙混过去）。
 */
void checkStyleResolution() {
    // 没有样式表、没有内联覆盖：结果就是各字段的默认值。
    {
        const StyleSheet empty;
        const ParaFormat got = resolveParagraphFormat(empty, "", {}, nullptr);
        const std::string d = diffPara(got, ParaFormat{});
        expectTrue(d.empty(),
                   "1.6 / 三层：空表加空覆盖就是默认值" + (d.empty() ? "" : "（" + d + "）"));
    }
    // 文档默认值是最下面那一层。
    {
        StyleSheet sheet;
        ParaFormatPatch dd;
        dd.size = Length::abs(ptToEmu(14.0));
        dd.color = Color::rgb(200, 0, 0);
        sheet.setDocDefaults(dd, {});
        ParaFormat want;
        want.size = Length::abs(ptToEmu(14.0));
        want.color = Color::rgb(200, 0, 0);
        const std::string d = diffPara(resolveParagraphFormat(sheet, "", {}, nullptr), want);
        expectTrue(d.empty(), "1.6 / 三层：文档默认值改两个字段，其余仍是默认" +
                                  (d.empty() ? "" : "（" + d + "）"));
    }
    // 段落样式链：based_on 逐级继承，叶覆盖根。
    {
        StyleSheet sheet;
        StyleDef base;
        base.name = "正文";
        base.para.font_index = 3;
        base.para.size = Length::abs(ptToEmu(10.0));
        sheet.add(base);

        StyleDef leaf;
        leaf.name = "强调段";
        leaf.based_on = "正文";
        leaf.para.color = Color::rgb(0, 0, 255);
        leaf.para.line_factor = 2.0f;
        sheet.add(leaf);

        ParaFormat want;
        want.font_index = 3;                     // 继承自根
        want.size = Length::abs(ptToEmu(10.0));  // 继承自根
        want.color = Color::rgb(0, 0, 255);      // 叶
        want.line_factor = 2.0f;                 // 叶
        const std::string d = diffPara(resolveParagraphFormat(sheet, "强调段", {}, nullptr), want);
        expectTrue(d.empty(),
                   "1.6 / 三层：样式链逐属性合并（叶覆盖根）" + (d.empty() ? "" : "（" + d + "）"));

        // 内联覆盖压过整条链。
        ParaFormatPatch inline_p;
        inline_p.size = Length::abs(ptToEmu(20.0));
        want.size = Length::abs(ptToEmu(20.0));
        const std::string d2 =
            diffPara(resolveParagraphFormat(sheet, "强调段", inline_p, nullptr), want);
        expectTrue(d2.empty(),
                   "1.6 / 三层：内联覆盖压过样式链" + (d2.empty() ? "" : "（" + d2 + "）"));
    }
    // 字符四层：文档默认值 → 段落样式的字符部分 → 字符样式链 → 元素上的内联 marks。
    {
        StyleSheet sheet;
        CharFormat dd;
        dd.bold = true;
        sheet.setDocDefaults({}, dd);

        StyleDef para_style;
        para_style.name = "正文";
        para_style.run.font_index = 7;
        sheet.add(para_style);

        StyleDef term;
        term.name = "术语";
        term.kind = StyleKind::Character;
        term.run.size = Length::abs(ptToEmu(18.0));
        sheet.add(term);

        StyleDef strong;
        strong.name = "术语加强";
        strong.kind = StyleKind::Character;
        strong.based_on = "术语";
        strong.run.italic = true;
        sheet.add(strong);

        CharFormat inline_marks;
        inline_marks.color = Color::rgb(9, 9, 9);
        const CharFormat got = resolveRunFormat(sheet, "正文", "术语加强", inline_marks, nullptr);

        CharFormat want;
        want.bold = true;                        // 文档默认值
        want.font_index = 7;                     // 段落样式自带的字符属性
        want.size = Length::abs(ptToEmu(18.0));  // 字符样式链
        want.italic = true;                      // 字符样式链（叶）
        want.color = Color::rgb(9, 9, 9);        // 内联
        expectTrue(got == want,
                   "1.6 / 三层：字符格式四层合并（默认值 → 段落样式 → 字符样式链 → 内联）");
        expectTrue(!got.underline.has_value(),
                   "1.6 / 三层：四层都没设置的属性保持未设置（不是被写成了默认值）");
    }
}

/**
 * 1.6：链上的两种异常都要被诊断出来，而且不能把整篇拖死。
 */
void checkStyleChainEdges() {
    // 成环：诊断里有"成环"，链上已经解析到的部分仍然生效。
    {
        StyleSheet sheet;
        StyleDef a;
        a.name = "A";
        a.based_on = "B";
        a.para.font_index = 1;
        sheet.add(a);
        StyleDef b;
        b.name = "B";
        b.based_on = "A";
        b.para.size = Length::abs(ptToEmu(9.0));
        sheet.add(b);

        std::vector<std::string> why;
        const ParaFormat got = resolveParagraphFormat(sheet, "A", {}, &why);
        expectTrue(why.size() == 1 && why[0].find("成环") != std::string::npos,
                   "1.6 / 成环：记一条诊断（" + (why.empty() ? std::string("无") : why[0]) + "）");
        expectTrue(got.font_index == 1 && (got.size == Length::abs(ptToEmu(9.0))),
                   "1.6 / 成环：链上已经解析到的部分仍然生效，不转圈也不整篇算不出");
    }
    // 样式名不存在：降级到默认值并诊断，不静默。
    {
        const StyleSheet sheet;
        std::vector<std::string> why;
        const ParaFormat got = resolveParagraphFormat(sheet, "没有这个样式", {}, &why);
        expectTrue(
            why.size() == 1 && why[0].find("不存在") != std::string::npos,
            "1.6 / 未知样式：记一条诊断（" + (why.empty() ? std::string("无") : why[0]) + "）");
        expectTrue(diffPara(got, ParaFormat{}).empty(), "1.6 / 未知样式：降级到默认值");
    }
    // 上游不存在：本层已经设置的属性照旧生效。
    {
        StyleSheet sheet;
        StyleDef a;
        a.name = "孤儿";
        a.based_on = "不存在";
        a.para.font_index = 4;
        sheet.add(a);
        std::vector<std::string> why;
        const ParaFormat got = resolveParagraphFormat(sheet, "孤儿", {}, &why);
        expectTrue(why.size() == 1 && got.font_index == 4,
                   "1.6 / 未知上游：本层的属性照旧生效，缺的那一层记诊断");
    }
    // 名字为空：不算错误，也不产生诊断。
    {
        const StyleSheet sheet;
        std::vector<std::string> why;
        resolveParagraphFormat(sheet, "", {}, &why);
        resolveRunFormat(sheet, "", "", {}, &why);
        expectTrue(why.empty(), "1.6 / 空样式名：不引用样式，不产生诊断");
    }
}

/**
 * 1.6：解析结果的缓存。判据是"第二次查不重算" —— 只比结果相等证明不了这一点，重算也相等。
 */
void checkStyleCache() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    const NodeId t1 = tree.appendChild(sec, NodeKind::TextRun);
    tree.setText(t1, cps({0x61}));
    const NodeId m1 = tree.appendChild(sec, NodeKind::ParagraphMark);
    const NodeId m2 = tree.appendChild(sec, NodeKind::ParagraphMark);

    StyleSheet sheet;
    StyleDef style;
    style.name = "正文";
    style.para.font_index = 2;
    sheet.add(style);
    tree.find(m1)->styleRef = "正文";
    tree.find(m1)->para.size = Length::abs(ptToEmu(20.0));

    StyleCache cache(sheet);
    const ParaFormat first = cache.paragraph(tree, m1);
    expectTrue(cache.computations() == 1 && cache.hits() == 0 && first.font_index == 2 &&
                   (first.size == Length::abs(ptToEmu(20.0))),
               "1.6 / 缓存：第一次查要算，结果是样式加内联覆盖");

    cache.paragraph(tree, m1);
    expectTrue(cache.computations() == 1 && cache.hits() == 1,
               "1.6 / 缓存：同样的元素再查一次直接命中，不重算");
    expectTrue(cache.paragraph(tree, m1).font_index == 2, "1.6 / 缓存：命中返回的仍是同一份结果");

    cache.paragraph(tree, m2);
    expectTrue(cache.computations() == 2, "1.6 / 缓存：另一个元素要各算一份");

    // 样式表一改：整体失效（改一次样式影响的是全篇引用它的元素，逐个失效不如整体失效）。
    StyleDef another;
    another.name = "标题";
    sheet.add(another);
    cache.paragraph(tree, m1);
    expectTrue(cache.computations() == 3, "1.6 / 缓存：样式表改版之后整体失效，要重算");

    // 只有某一个元素自己改了时，只失效它一个：另一个元素仍然命中。
    const int hits_before = cache.hits();
    cache.invalidate(m2);
    cache.paragraph(tree, m1);
    expectTrue(cache.computations() == 3 && cache.hits() == hits_before + 1,
               "1.6 / 缓存：单个元素失效不影响别的条目");
    cache.paragraph(tree, m2);
    expectTrue(cache.computations() == 4, "1.6 / 缓存：被失效的那一个要重算");

    // run 的缓存键还带上所在段落的样式名：同一个 run 换了段落样式就是另一份结果。
    cache.run(tree, t1, "正文");
    const int before = cache.computations();
    cache.run(tree, t1, "正文");
    expectTrue(cache.computations() == before, "1.6 / 缓存：同一 run 同一段落样式命中");
    cache.run(tree, t1, "标题");
    expectTrue(cache.computations() == before + 1,
               "1.6 / 缓存：同一 run 换了段落样式不能复用旧结果");
}

/**
 * 1.6：样式经编译进引擎输入。段落格式是解析出来的，节点上只留差异。
 */
void checkStyleThroughCompile() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cps({0x61}));
    const NodeId mark = tree.appendChild(sec, NodeKind::ParagraphMark);
    tree.find(mark)->styleRef = "大标题";
    tree.find(mark)->para.size = Length::abs(ptToEmu(20.0));

    expectTrue(tree.find(mark)->para.size.has_value() &&
                   !tree.find(mark)->para.font_index.has_value() &&
                   !tree.find(mark)->para.color.has_value(),
               "1.6 / 编译：节点上只留与样式不同的属性（没有字号之外的覆盖）");

    StyleSheet sheet;
    StyleDef style;
    style.name = "大标题";
    style.para.font_index = 5;
    style.para.size = Length::abs(ptToEmu(16.0));
    style.para.color = Color::rgb(200, 0, 0);
    sheet.add(style);

    const CompileResult r = compileDocument(tree, FlowVisibility::Design, sheet);
    expectTrue(r.ok() && r.blocks.size() == 1 && r.blocks[0].font_index == 5 &&
                   r.blocks[0].size_pt == 20.0f && r.blocks[0].color == Color::rgb(200, 0, 0),
               "1.6 / 编译：字号取内联覆盖 20 磅，字体与颜色取自样式（5 号、红）");

    // 改样式表再编一次：产物跟着变（编译不做缓存，模型改了就重编）。
    style.para.font_index = 6;
    sheet.add(style);
    const CompileResult r2 = compileDocument(tree, FlowVisibility::Design, sheet);
    expectTrue(r2.ok() && r2.blocks.size() == 1 && r2.blocks[0].font_index == 6,
               "1.6 / 编译：样式表改过之后重新编译，产物跟着变");

    // 未知样式名：记诊断，不静默降级（ok() 变假，调用方不能当完整结果用）。
    tree.find(mark)->styleRef = "没有这个样式";
    const CompileResult r3 = compileDocument(tree, FlowVisibility::Design, sheet);
    expectTrue(!r3.ok() && r3.blocks.size() == 1 &&
                   r3.diagnostics[0].detail.find("不存在") != std::string::npos,
               "1.6 / 编译：未知样式名进诊断，产物仍编出一段");
}

/**
 * 1.6：样式引用是内容的一部分，捕获子树、复制片段、撤销都要把它带上。
 */
void checkStyleRefTravels() {
    NodeTree a;
    const NodeId sec = a.firstSectionId();
    const NodeId t = a.appendChild(sec, NodeKind::TextRun);
    a.setText(t, cps({0x78}));
    a.find(t)->styleRef = "术语";
    const NodeId m = a.appendChild(sec, NodeKind::ParagraphMark);
    a.find(m)->styleRef = "标题";
    a.find(m)->para.line_factor = 2.0f;

    // 节点级：捕获再物化到另一棵树里，样式引用与内联覆盖逐项相同。
    const NodeSpec spec = captureSubtree(a, sec);
    NodeTree b;
    const NodeId copied = materializeSubtree(b, b.rootId(), 0, spec);
    expectTrue(copied != kInvalidNodeId && b.find(copied) != nullptr &&
                   b.find(copied)->children.size() == 2,
               "1.6 / 子树：捕获再物化到另一棵树成功");
    if (copied != kInvalidNodeId && b.find(copied)->children.size() == 2) {
        const Node* bt = b.find(copied)->children[0];
        const Node* bm = b.find(copied)->children[1];
        expectTrue(bt->styleRef == "术语" && isParagraphMark(bm->kind) && bm->styleRef == "标题" &&
                       bm->para.line_factor.has_value(),
                   "1.6 / 子树：字符样式与段落样式随子树一起过去");
    }

    // 片段级：提取带上样式引用，插回去之后仍在（复制粘贴走的就是这条路）。
    const ExtractResult ex = extractRange(a, Range{1, 3});
    expectTrue(ex.ok && ex.fragment.pieces.size() == 2 &&
                   ex.fragment.pieces[0].styleRef == "术语" &&
                   ex.fragment.pieces[1].styleRef == "标题" &&
                   ex.fragment.pieces[1].para.line_factor.has_value(),
               "1.6 / 片段：提取带上字符样式与段落样式");

    NodeTree c;
    InsertFragment ins;
    ins.at = 1;
    ins.content = ex.fragment;
    expectTrue(applyStep(c, Step{ins}).ok, "1.6 / 片段：把提取出来的片段插进另一棵树");
    const Node* csec = c.find(c.firstSectionId());
    if (csec != nullptr && csec->children.size() == 2) {
        expectTrue(csec->children[0]->styleRef == "术语" &&
                       isParagraphMark(csec->children[1]->kind) &&
                       csec->children[1]->styleRef == "标题" &&
                       csec->children[1]->para.line_factor.has_value(),
                   "1.6 / 片段：粘贴之后样式引用与内联覆盖都在");
    } else {
        expectTrue(false, "1.6 / 片段：插入之后应该有一个文本节点与一个段落标记");
    }

    // 撤销：删掉段落标记再撤销，样式与格式都要回来。
    NodeTree d;
    const NodeId dsec = d.firstSectionId();
    d.setText(d.appendChild(dsec, NodeKind::TextRun), cps({0x79}));
    const NodeId dm = d.appendChild(dsec, NodeKind::ParagraphMark);
    d.find(dm)->styleRef = "标题";
    d.find(dm)->para.line_factor = 3.0f;
    const ParaMarkBuild built = makeRemoveParaMark(d, 2);
    expectTrue(built.ok, "1.6 / 撤销：构造删除段落标记的步骤（格式从文档读出）" +
                             (built.ok ? "" : "（" + built.error + "）"));
    const std::vector<std::string> before = shapeSnapshot(d);
    expectTrue(applyStep(d, built.step).ok, "1.6 / 撤销：应用删除");
    const StepResult back = applyStep(d, invertStep(built.step));
    // 比形状而不是比节点 ID：段落标记在模型里没有身份（它的身份是流位置），撤销是"造一个
    // 新的标记"，要求 ID 不变等于给模型加一条它本来没有的约束。
    expectTrue(back.ok && shapeSnapshot(d) == before,
               "1.6 / 撤销：撤销之后逐字段回到原样（含段落样式与内联覆盖）");
}

}  // namespace

int main() {
    // 不缓冲：崩溃时已经打印出来的断言行不会留在缓冲区里丢掉，定位崩溃点全靠它。
    // MSVC 把 _IOLBF 当成全缓冲，所以这里必须用 _IONBF。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf(
        "=== 文档模型 0.1 ~ 1.6：位置、分段、不变量、编译、长度、规范化、步骤、撤销、落点、"
        "控件、样式 ===\n");

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
        // 随机树是中间态形状（会造出相邻同标记的 run），先规范化再按已提交文档的口径检查。
        NodeTree t = makeRandomTree(seed, 40);
        const NormalizeResult n = normalizeDocument(t);
        expectClean(t, "0.3 / 随机树 #" + std::to_string(seed) + "（规范化：并 " +
                           std::to_string(n.merged) + "、清 " + std::to_string(n.removed) + "）");
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

    std::printf("  --- 1.1：文本节点与标记合并规范化 ---\n");
    checkNormalizeShapes();
    checkNormalizeIdempotent();
    checkNormalizeKeepsContent();

    std::printf("  --- 1.2：区间原语、编辑步骤（内容级与节点级）与位置映射 ---\n");
    checkRangeNormalize();
    checkExtract();
    checkStepRoundTrip();
    checkExtractDeleteInsertBack();
    checkMapRoundTrip();
    checkNodeStepRoundTrip();
    checkNodeStepMap();
    checkNodeStepRejects();
    checkNodeStepGate();

    std::printf("  --- 1.5：控件索引与宿主 API ---\n");
    checkControlIndex();
    checkReadValues();
    checkSetValues();
    checkSetValuesRejects();
    checkSetProperties();
    checkControlApiSequence();

    std::printf("  --- 1.5b：外观、属性侧的值与异步裁决门 ---\n");
    checkAppearance();
    checkValueShapes();
    checkAsyncGate();

    std::printf("  --- 1.6：样式表与三层解析、缓存 ---\n");
    checkStyleResolution();
    checkStyleChainEdges();
    checkStyleCache();
    checkStyleThroughCompile();
    checkStyleRefTravels();

    std::printf("  --- 1.3：事务与撤销分组 ---\n");
    checkUndoRedo();
    checkUndoGrouping();

    std::printf("  --- 1.4：控件边界位置与表单模式落点判定 ---\n");
    checkBoundaryPositions();
    checkEditableSet();
    checkFormNavigation();
    checkTabStops();
    checkEditRangeGate();
    checkStructureOps();

    std::printf("\n共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
