#pragma once
/**
 * 控件区间的唯一查询面。
 *
 * 为什么单列一份：控件在模型里不是容器节点，只是一段文字区间加流里的两枚括号占位，
 *   于是"区间在哪、这一格归谁"引擎、编辑命令与光标解析都要问。各写一套必然在边界上分家
 *   （同一个偏移压着六枚括号时，按括号排布反推的那一套会把三层内容区间缩成一个点）。
 *   区间只从流索引取（FlowIndex::intervalOf），它本来就算好了每枚括号的位置。
 *
 * 这里的函数都是纯查询、没有状态：调用方把自己 view 里的 flow 与控件节点表递进来。
 *
 * 下标约定：控件下标 = 文档顺序 = control_node_ids 的下标 = 控件集合的下标。三者由同一次
 *   投影产出，调用方要保证它们同步（引擎侧见 syncControls）。
 */
#include <vector>

#include "core/doc/flow.h"
#include "core/doc/node_id.h"
#include "core/rev/flow.h"

namespace heditor {

/**
 * 控件在流里的区间（含两枚括号，半开）。
 *
 * 宽 2 = 空控件（两枚括号相邻）。不在流里（隐藏、摘除、未装载）返回假。
 *
 * @param flow 流索引
 * @param id 控件节点
 * @param lo 写回区间起点（开括号那一格）
 * @param hi 写回区间终点（闭括号之后，不含）
 * @return true = 这个控件在流里
 */
inline bool controlSpanOf(const rev::FlowIndex& flow, NodeId id, FlowPos* lo, FlowPos* hi) {
    FlowPos first = 0;
    FlowPos end = 0;
    if (!flow.intervalOf(id, &first, &end)) return false;
    // 一个控件至少有两枚括号。少于此说明索引给的不是控件（节点被摘掉了一半）。
    if (end - first < 2) return false;
    if (lo != nullptr) *lo = first;
    if (hi != nullptr) *hi = end;
    return true;
}

/**
 * 控件的内容区间（插入点范围，两端都含）。两枚括号各占一格，所以是 span 退掉两头。
 *
 * 空控件退化成一点（lo == hi）：那个点是"开括号之后、闭括号之前"，本来就是唯一的插入点。
 *
 * @param flow 流索引
 * @param id 控件节点
 * @param lo 写回内容起点（开括号之后）
 * @param hi 写回内容末尾（闭括号之前）
 * @return true = 这个控件在流里
 */
inline bool controlContentSpanOf(const rev::FlowIndex& flow, NodeId id, FlowPos* lo, FlowPos* hi) {
    FlowPos first = 0;
    FlowPos end = 0;
    if (!controlSpanOf(flow, id, &first, &end)) return false;
    if (lo != nullptr) *lo = first + 1;
    if (hi != nullptr) *hi = end - 1;
    return true;
}

/**
 * 某个流位置落在哪个控件的内容区间里。嵌套时取最内层 —— 起点最大者，同起点取跨度短者。
 *
 * 取外层会让"这一格归谁"与编辑归属分家：光标在一个字段里、编辑却按另一个字段的规则判。
 *   按遍历顺序取第一个命中的更糟：外层可编辑会替内层只读作证。
 *
 * @param flow 流索引
 * @param ids 控件节点表（文档顺序）
 * @param f 流位置
 * @param out_lo 写回它的内容起点（可为空）
 * @param out_hi 写回它的内容末尾（可为空）
 * @return 控件下标；-1 = 不在任何控件的内容里
 */
inline int innermostControlAtFlow(const rev::FlowIndex& flow, const std::vector<NodeId>& ids,
                                  FlowPos f, FlowPos* out_lo = nullptr, FlowPos* out_hi = nullptr) {
    int best = -1;
    FlowPos best_lo = 0;
    FlowPos best_hi = 0;
    for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!controlContentSpanOf(flow, ids[static_cast<size_t>(i)], &lo, &hi)) continue;
        if (f < lo || f > hi) continue;
        if (best < 0 || lo > best_lo || (lo == best_lo && (hi - lo) < (best_hi - best_lo))) {
            best = i;
            best_lo = lo;
            best_hi = hi;
        }
    }
    if (best >= 0) {
        if (out_lo != nullptr) *out_lo = best_lo;
        if (out_hi != nullptr) *out_hi = best_hi;
    }
    return best;
}

}  // namespace heditor
