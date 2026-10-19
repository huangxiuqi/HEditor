/**
 * 投影：修订 → 引擎输入。纯函数。
 *
 * 它替掉的是编译（compileDocument）加流索引加控件导出那三趟：一次遍历同时产出块、块的内容
 * 哈希、控件在块坐标里的跨度。产物里没有"指针指回修订"的东西 —— 块持有一份码点副本（每段
 * 一次，不是整篇），其余全是值。
 *
 * 缓存怎么用：块哈希只从贡献这个块的节点算（节点哈希已经是内容哈希），于是两个修订的块哈希
 * 列表一比就知道哪几块要重排（见 diffBlocks），不需要任何人上报"我改了什么"。
 * 样式表的版本号单独一并给出：块哈希表达内容，解析出来的格式还依赖样式表，缓存键要两样都带。
 *
 * 切片范围：与修订层一致 —— 文字、段落标记、控件。表格、域、图片、节属性会记诊断并让 ok()
 * 变假，容器仍下钻取文本。诊断不静默：调用方拿到的产物若 ok() 为假，就不是完整的排版输入。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/compile.h"
#include "core/rev/rev.h"

namespace heditor::rev {

/**
 * 投影参数。它是参数，不是"改产物"：换模式、换页度量都重新投影一份，不存在"一半按新参数、
 * 一半按旧参数"的中间态。
 *
 * 括号显示范围（D3 里那一项）也属于这里，但它的两档语义要等表单模式落进来；现在投影恒按
 * 全部边界产出，先加一个没有人实现的枚举值只会让调用方以为可以选。
 */
struct ViewParams {
    FlowVisibility vis = FlowVisibility::Design;
};

/**
 * 一个块的内容哈希（128 位）。同一块内容算出同一个数，改一个码点就换一个数。
 */
struct BlockHash {
    uint64_t lo = 0;
    uint64_t hi = 0;

    bool operator==(const BlockHash& o) const = default;
};

/**
 * 一个投影出来的块：排版输入、来源身份、内容哈希。
 *
 * 三者放在一条记录里而不是三条平行的数组：平行数组的长短与次序靠调用方维持，那是"两个数组
 * 错位一格"这类缺陷的产地。
 */
struct ProjectedBlock {
    BlockSpec spec;
    BlockSource source;
    BlockHash hash;
};

/**
 * 一个控件在块坐标里的跨度。block / cp 是起始边界的位置，end_block / end_cp 是结束边界 ——
 * 跨段控件的两者不在同一块里。
 */
struct ControlSpan {
    Ref node;
    int block = -1;
    int cp = 0;
    int end_block = -1;
    int end_cp = 0;
};

/**
 * 一次投影的全部产物。
 */
struct Projection {
    std::vector<ProjectedBlock> blocks;
    std::vector<ControlSpan> controls;
    std::vector<CompileDiagnostic> diagnostics;
    int style_version = 0;

    bool ok() const { return diagnostics.empty(); }
};

/**
 * 把一份修订投影成引擎输入。sheet 提供样式链（空表等于"全部用默认值"）。
 */
Projection project(const Doc& doc, const StyleSheet& sheet, const ViewParams& params = {});

/**
 * 只要每块的内容哈希，不要排版输入。开销与块数成正比、与文本长度无关 —— 增量排版每轮先用
 * 它算出"哪几块变了"，再对变了的块调 project。
 */
std::vector<BlockHash> blockHashes(const Doc& doc, FlowVisibility vis);

/**
 * 两次投影之间需要重新布局的块区间 [lo, hi)。
 *
 * 做法是掐掉相同的头与相同的尾，中间那段全算损伤。它是"以多报换简单"：散落的多次编辑会让
 * 中间一段里未变的块也被重排一次，但绝不会漏报 —— 漏报才是会让画面停在旧内容上的那种错。
 * 未变而被重排的块命中排版缓存（键是块哈希），代价是一次查表。
 */
struct BlockDamage {
    int lo = 0;
    int hi = 0;

    /**
     * 两次投影是否有任何不同。区间空不等于"没变"：整篇删空时区间长度是 0，但页表必须清掉 ——
     * 只按区间长度判断的话，那种情形会被当成"什么都不用做"。
     */
    bool changed = false;

    bool empty() const { return !changed; }
    int count() const { return hi - lo; }
};

BlockDamage diffBlocks(const std::vector<BlockHash>& before, const std::vector<BlockHash>& after);

/**
 * 两份修订之间哪几块要重排，由哈希下降直接算出。
 *
 * 与 diffBlocks 的分工：那个比的是两条已经算好的块哈希 —— 要先把整篇投影一遍，代价 O(块数)；
 * 这个从根往下走，哈希相同的子树一步剪掉、连进都不进，也不需要物化任何 BlockSpec。
 *
 * 坐标按 after 的块序。after 尾部删段时 lo == hi == 块数："没有块要重排，但块数变了"，
 * 与 diffBlocks 同一口径（页表要跟着改）。
 *
 * 局部性的实情：孩子表是 vector，所以每一层对齐都要扫一遍孩子（O(孩子数)），
 *   "O(改动的块 × 深度)"要等孩子表分块（每块带聚合哈希）之后才成立。今天拿到的是
 *   "不做物化、与不相关的容器无关"，不是"与段数无关"——scanned 那个计数就是给这件事留的。
 */
struct Damage {
    BlockDamage range;
    /**
     * 真正比较过哈希的节点数。它是局部性的判据，不是性能数字。
     */
    int visited = 0;
    /**
     * 为对齐孩子表做过的身份比较次数。孩子表是 vector，所以这一项在每层是 O(孩子数)。
     */
    int scanned = 0;
};

Damage damage(const Doc& before, const Doc& after);

}  // namespace heditor::rev
