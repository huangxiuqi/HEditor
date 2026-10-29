/**
 * 命令层的内部工具：删除步骤的起点、整块覆盖的控件、控件属性的读取与几条造步助手。
 *
 * 只给 edit_command*.cpp 用。它们不碰 EditCommand 的成员，所以放在外面 —— 一拆文件就
 *   必须有一份共享的实现，各域各抄一份会让"怎么算一步"分叉。
 */
#pragma once

#include "core/engine/edit_command.h"

#include "core/doc/subtree.h"
#include "core/doc/table_grid.h"
#include "core/engine/control_spans.h"
#include "core/engine/structure_index.h"
#include "core/utils/utf8.h"

#include <algorithm>
#include <functional>
#include <variant>

namespace heditor::detail {

/**
 * 一步删除的起点（把整批删除步骤从后往前排时用）。
 * 只认两种删除步骤 —— 这个序列里不会有别的。
 */
inline FlowPos stepStartFlow(const Step& s) {
    if (const auto* d = std::get_if<DeleteRange>(&s)) return d->begin;
    if (const auto* r = std::get_if<RemoveParaMark>(&s)) return r->at;
    if (const auto* n = std::get_if<RemoveNode>(&s)) return n->at_flow;
    return -1;
}

/**
 * 区间里整块被覆盖的控件：摘掉这棵子树就等于删掉它占的全部流位置（边界与内容）。
 *
 * 控件在流里占两位边界加内容，而文本删除步只能覆盖内容 —— 混着文字一起选时，
 * 边界那两位没有对应步骤，覆盖校验必然对不上，整条编辑会被拒。所以整块覆盖的控件
 * 单独走 RemoveNode，其余区间才按文本 / 段落标记 / 原子发步。
 */
struct WholeControl {
    /**
     * 控件节点身份。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 它在流里的起点（含开括号）。
     */
    FlowPos lo = 0;

    /**
     * 它在流里的终点（含闭括号）。
     */
    FlowPos hi = 0;
};

/**
 * 读控件的运行时属性。四个分居两处：hidden 在节点上（呈现态的流位置换算要看它），
 * 其余三个在控件数据里。
 */
inline bool readPropValue(const doc::Node* n, ControlProp p, bool* out) {
    if (n == nullptr || n->kind != NodeKind::ContentControl || out == nullptr) return false;
    const auto* spec = std::get_if<ControlSpec>(&n->payload);
    if (spec == nullptr) return false;
    switch (p) {
        case ControlProp::Hidden:
            *out = n->hidden;
            return true;
        case ControlProp::Readonly:
            *out = spec->readonly;
            return true;
        case ControlProp::Required:
            *out = spec->required;
            return true;
        case ControlProp::StructureLock:
            *out = spec->structureLock;
            return true;
    }
    return false;
}

/**
 * 一枚原子整枚删掉：按流位置寻址。原子没有可寻址的内部，它在流里只占自己那一格。
 */
inline NodeBuild removeAtomStep(const doc::Doc& doc, const doc::FlowIndex& idx, FlowPos at) {
    NodeBuild out;
    if (at < 0 || at >= idx.size()) {
        out.error = "删除原子：流位置越界";
        return out;
    }
    const Pos p = idx.posAt(at);
    const doc::Node* n = doc::findNode(doc, p.container);
    if (n == nullptr || !isAtom(n->kind) || p.offset != 0) {
        out.error = "删除原子：该位置上没有原子";
        return out;
    }
    RemoveNode r;
    r.at_flow = at;
    out.step = Step{r};
    out.ok = true;
    return out;
}

/**
 * 删一枚段落标记。格式与样式引用从文档里读出来带上 —— 撤销要靠它还原，手填一份等于把
 * "这一段长什么样"记两遍。
 */
inline ParaMarkBuild removeMarkStep(const doc::Doc& doc, const doc::FlowIndex& idx, FlowPos at) {
    ParaMarkBuild out;
    if (at < 0 || at >= idx.size()) {
        out.error = "删除段落标记：流位置越界";
        return out;
    }
    const Pos p = idx.posAt(at);
    const doc::Node* n = doc::findNode(doc, p.container);
    if (n == nullptr || !isParagraphMark(n->kind) || p.offset != 0) {
        out.error = "删除段落标记：该位置上没有段落标记";
        return out;
    }
    const auto* mp = std::get_if<doc::MarkPayload>(&n->payload);
    if (mp == nullptr) {
        out.error = "删除段落标记：载荷不是段落标记";
        return out;
    }
    RemoveParaMark s;
    s.at = at;
    s.para = mp->para;
    s.styleRef = mp->style;
    out.step = Step{s};
    out.ok = true;
    return out;
}

/**
 * 删一个流区间：同层且只含文本与段落标记时一条 DeleteRange 就够。
 *
 * 区间里出现整块元素或跨层时返回空，由调用方另走结构步骤（删原子、删整表）。
 */
inline std::vector<Step> deleteWholeRange(const doc::Doc& doc, const doc::FlowIndex& idx,
                                          FlowPos from, FlowPos to) {
    if (from >= to) return {};
    if (from < 0 || to > idx.size()) return {};
    const Pos b = idx.posAt(from);
    const doc::Node* bn = doc::findNode(doc, b.container);
    if (bn == nullptr) return {};
    const doc::Node* parent = isContainer(bn->kind) ? bn : doc::parentOf(doc, bn->id);
    if (parent == nullptr) return {};

    // 覆盖总和必须恰好等于区间长：跨层（区间伸出了这一层的孩子表）或中间有洞时
    //   不满足，宁可返回空让调用方另走结构步骤，也不要发出半截区间 —— 那一类症状是
    //   "选中了整张表，只删掉第一格的字"。
    int covered = 0;
    for (const doc::Ref& c : parent->children) {
        int f = 0;
        int e = 0;
        if (!idx.intervalOf(c->id, &f, &e)) continue;
        if (e <= from || f >= to) continue;
        if (!isText(c->kind) && !isParagraphMark(c->kind)) return {};
        covered += (e < to ? e : to) - (f > from ? f : from);
    }
    if (covered != to - from) return {};

    DeleteRange s;
    s.begin = from;
    s.end = to;
    return {Step{s}};
}

/**
 * 按文本节点切分的删除步骤：每个被区间盖到的 run 各发一条，原子与段落标记不产生步骤
 * （它们由调用方另发结构步骤）。从后往前发 —— 删右边不会移动左边的坐标。
 *
 * 逐节点跳跃而不是逐位问 posAt：后者每次都要从根下降、每层线性扫孩子，整篇删除是亿级。
 */
inline std::vector<Step> deleteTextSteps(const doc::Doc& doc, const doc::FlowIndex& idx,
                                         FlowPos from, FlowPos to) {
    std::vector<Step> out;
    if (from < 0 || to <= from || to > idx.size()) return out;
    std::vector<std::pair<FlowPos, FlowPos>> segs;
    FlowPos p = from;
    while (p < to) {
        const Pos np = idx.posAt(p);
        const doc::Node* n = doc::findNode(doc, np.container);
        int nf = 0;
        int ne = 0;
        if (n == nullptr || !isText(n->kind) || !idx.intervalOf(n->id, &nf, &ne) || ne <= p) {
            ++p;  // 容器边界、段落标记、原子：这一位不产生步骤
            continue;
        }
        const int seg_to = (ne < to) ? ne : to;
        if (seg_to > p) segs.push_back({p, seg_to});
        p = ne;
    }
    out.reserve(segs.size());
    for (size_t i = segs.size(); i-- > 0;) {
        DeleteRange s;
        s.begin = segs[i].first;
        s.end = segs[i].second;
        out.push_back(Step{s});
    }
    return out;
}

}  // namespace heditor::detail
