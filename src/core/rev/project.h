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
 * 支持范围：文字、段落标记、控件、表格（含嵌套）、图片与公式；节与页眉页脚只提供层级。
 * 尚未进入投影的元素（目前是域）记一条诊断并让 ok() 变假，容器仍下钻取文本 —— 诊断不静默：
 * 调用方拿到的产物若 ok() 为假，就不是完整的排版输入。
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
    /**
     * 块坐标的可见性口径（呈现态下隐藏子树不产生块）。
     */
    FlowVisibility vis = FlowVisibility::Design;
};

/**
 * 一个块的内容哈希（128 位）。同一块内容算出同一个数，改一个码点就换一个数。
 */
struct BlockHash {
    /**
     * 内容哈希的低 64 位。
     */
    uint64_t lo = 0;

    /**
     * 内容哈希的高 64 位。
     */
    uint64_t hi = 0;

    bool operator==(const BlockHash& o) const = default;
};

/**
 * 一个投影出来的块：排版输入、来源身份、内容哈希。只在投影内部当临时载体 ——
 * 产物里的块流由调用方持有（见 project 的 blocks 参数），不再拷进一个 parallel 的数组。
 */
struct ProjectedBlock {
    /**
     * 排版输入。
     */
    BlockSpec spec;

    /**
     * 来源身份（哪棵子树里的那一段）。
     */
    BlockSource source;

    /**
     * 内容哈希。
     */
    BlockHash hash;
};

/**
 * 一块的身份：内容哈希与来源。与块流同长同序。
 *
 * 它是"下一轮能否复用"的全部依据，只有 32 字节 —— 块本身（BlockSpec）留在调用方持有的
 * 块流里，不跟着复制一份。
 */
struct BlockKey {
    /**
     * 内容哈希。
     */
    BlockHash hash;

    /**
     * 来源身份。
     */
    BlockSource source;
};

/**
 * 一块在流里的局部尺寸：本块内容占几位，它前面那道边界占几位。
 *
 * 两个都是局部的 —— 内容长短只由这一块自己决定，边界宽窄只由"上一块与本块之间那道坎"决定
 * （普通段界一位，表格格边界两位）。绝对位置由它们累加，所以编辑之后没有谁需要平移后缀：
 * 上游变长，下游的起点自然跟着累加值走。
 *
 * 单位与流位置同一个：容器两端各一位，文本按码点，原子与段落标记各一位。
 */
struct BlockExtent {
    /**
     * 本块内容占几个流位置。
     */
    int span = 0;

    /**
     * 本块之前那道边界占几位（段界 1，表格格边界 2）。
     */
    int gap = 0;
};

/**
 * 上一轮块流的身份账：版本号 + 每块的 Key。
 *
 * 版本号必须一起给：块哈希只算作者内容，而解析出来的格式还依赖样式表 —— 只比哈希的话，
 * 换了样式之后搬过来的就是旧格式，而哈希一模一样，任何内容判据都看不出来。
 */
struct BlockHistory {
    /**
     * 记这份账时的样式表版本。
     */
    int style_version = 0;

    /**
     * 每块的身份，与块流同长同序。
     */
    std::vector<BlockKey> keys;
};

/**
 * 一个控件在块坐标里的跨度。block / cp 是起始边界的位置，end_block / end_cp 是结束边界 ——
 * 跨段控件的两者不在同一块里。
 *
 * 产出顺序是文档顺序（进入控件时占槽，离开时填末尾）：对外下标是"第几个字段"，而对象表
 * 里 ControlStart 的出现顺序也是文档顺序 —— 两者必须是同一个序，否则同一个字段在产物里是
 * 第 1 个、在控件表里是第 2 个。离开时才登记会让嵌套时内层排到外层前面。
 */
struct ControlSpan {
    /**
     * 控件节点（共享引用）。
     */
    Ref node;

    /**
     * 起始边界所在的块。
     */
    int block = -1;

    /**
     * 起始边的段内码点位置。
     */
    int cp = 0;

    /**
     * 结束边界所在的块（跨段控件与起始不同块）。
     */
    int end_block = -1;

    /**
     * 结束边界的段内码点位置。
     */
    int end_cp = 0;
};

/**
 * 一次投影的全部产物。
 *
 * 块流不在里面：它由调用方持有并原地更新（见 project 的 blocks 参数）。这里只留"与块同长
 * 同序的身份账"以及控件、表格、诊断 —— 这样"投影产物"与"引擎的块流"不必是同一种表示，
 * 也就不存在两份拷贝。
 */
struct Projection {
    /**
     * 每块的身份，与块流同长同序。
     */
    std::vector<BlockKey> keys;

    /**
     * 每块的局部尺寸，与 keys 同长同序。绝对流位置由它累加出来 —— 投影走树时一并数出来的，
     * 不再事后拿节点去查流索引（见 BlockExtent 的说明）。
     *
     * 复用块也在里面：走树走的是内容，复用省掉的是"物化块"，不是遍历。所以这一列每一轮
     * 都是全的，不随复用跳格。
     */
    std::vector<BlockExtent> extents;

    /**
     * 块流。调用方给了 blocks 参数时这里为空 —— 块流在调用方手里，两边是同一份；
     * 没给时才装在这里（测试与审计的便捷入口）。
     */
    std::vector<BlockSpec> blocks;

    /**
     * 本轮从上一轮搬过来的块数（没有给 prev 时恒 0）。
     *
     * 它是"增量真的发生了"的判据：只报耗时的话，机器变快就能让一条本该红的判据变绿。
     * 复用是按"同一枚段落标记 + 同一份内容哈希"认的，所以这个数还顺带说明"哪一段的
     * 身份没换"。
     */
    int reused = 0;

    /**
     * 控件在块坐标里的跨度，按文档顺序。
     */
    std::vector<ControlSpan> controls;

    /**
     * 表格结构（出现顺序：外层在前，嵌套的内层在后）。与编译产物同一种类型：排版层认的是
     * 它，不是修订层的节点。
     */
    std::vector<LayoutTable> tables;

    /**
     * 浮动图的排版输入，按 (para, cp) 升序。与编译产物同一种类型（LayoutFloat）——
     * 投影与编译是两条产出路，两边的浮动画法必须一致。
     */
    std::vector<LayoutFloat> floats;

    /**
     * 投影过程中的诊断（遇到尚未支持的元素）。空 = 产物完整。
     */
    std::vector<CompileDiagnostic> diagnostics;

    /**
     * 本次投影用的样式表版本（缓存键的另一半）。
     */
    int style_version = 0;

    /**
     * 产物完整吗（没有诊断）。
     * @return true = 没有诊断
     */
    bool ok() const { return diagnostics.empty(); }
};

/**
 * 一次投影里的损伤区间：[lo, hi) 按 after 的块序。
 *
 * 做法是掐掉相同的头与相同的尾，中间那段全算损伤。它是"以多报换简单"：散落的多次编辑会让
 * 中间一段里未变的块也被重排一次，但绝不会漏报 —— 漏报才是会让画面停在旧内容上的那种错。
 * 未变而被重排的块命中排版缓存（键是块哈希），代价是一次查表。
 */
struct BlockDamage {
    /**
     * 损伤区间的起点（按 after 的块序）。
     */
    int lo = 0;

    /**
     * 损伤区间的终点（不含）。
     */
    int hi = 0;

    /**
     * 两次投影是否有任何不同。区间空不等于"没变"：整篇删空时区间长度是 0，但页表必须清掉 ——
     * 只按区间长度判断的话，那种情形会被当成"什么都不用做"。
     */
    bool changed = false;

    /**
     * after 的块数，也是区间坐标的分母：区间之后的第 k 块，在上一轮里是第 k - (m - n) 块。
     *
     * 判据 [22] 把它与投影出来的块数逐例比对：两者一旦不等，按区间搬块就会错位，
     * 而错位只表现为"画面停在旧内容上"。
     */
    int after_blocks = 0;

    /**
     * 两次投影有没有不同。
     * @return true = 没有变化
     */
    bool empty() const { return !changed; }

    /**
     * 损伤区间的长度。
     * @return 块数
     */
    int count() const { return hi - lo; }
};

/**
 * 两份块哈希列表相比，掐掉相同的头与尾，中间那段算损伤。
 * @param before 上一轮的块哈希
 * @param after 这一轮的块哈希
 * @return 损伤区间（按 after 的块序）
 */
BlockDamage diffBlocks(const std::vector<BlockHash>& before, const std::vector<BlockHash>& after);

/**
 * 把一份修订投影成引擎输入。sheet 提供样式链（空表等于"全部用默认值"）。
 *
 * blocks 既是上一轮的块流（就地保留区间之外的块），也是这一轮的输出：中段 [lo, hi)
 * 被替换成新物化的块，其余原地不动。所以这里没有"产出一份新的块流"，也就没有整份
 * 拷贝 —— 调用方（引擎）拿到的就是它自己的那一份。
 *
 * prev 是上一轮的身份账（与 blocks 同长同序）与样式表版本；dmg 给出"哪一段要重算"。
 * 三条作废条件：样式表版本变了（块哈希只算内容，格式却依赖样式表）、dmg 为空、
 * prev 与 blocks 长度不符。作废时整篇物化，reused 为 0。省下来的是"每块算哈希 +
 * 物化"，不是整棵树的遍历：孩子表是 vector，找到损伤位置仍要扫过前面的孩子
 * （damage 的 scanned 就是这个下限的计数）。
 * @param doc 修订
 * @param sheet 样式表
 * @param params 投影参数
 * @param blocks 块流（既作上一轮的输入，也作这一轮的输出）
 * @param prev 上一轮的身份账（可为 nullptr）
 * @param dmg 损伤区间（可为 nullptr，表示整篇重算）
 * @param doc 修订
 * @param sheet 样式表
 * @param params 投影参数
 * @param blocks 块流（既作上一轮的输入，也作这一轮的输出）
 * @param prev 上一轮的身份账（可为 nullptr）
 * @param dmg 损伤区间（可为 nullptr，表示整篇重算）
 * @return 身份账、局部尺寸、控件跨度与诊断
 */
Projection project(const Doc& doc, const StyleSheet& sheet, const ViewParams& params,
                   std::vector<BlockSpec>* blocks, const BlockHistory* prev,
                   const BlockDamage* dmg);

/**
 * 只要块流、不要控件与表格的便捷入口（测试与审计用）。块流从空开始，整篇物化。
 * @param doc 修订
 * @param sheet 样式表
 * @param params 投影参数
 * @param blocks 块流（可为 nullptr；给了就写进去）
 * @return 身份账、局部尺寸、控件跨度与诊断
 */
Projection project(const Doc& doc, const StyleSheet& sheet, const ViewParams& params = {},
                   std::vector<BlockSpec>* blocks = nullptr);

/**
 * 只要每块的内容哈希，不要排版输入。开销与块数成正比、与文本长度无关 —— 排版层每轮先用
 * 它算出"哪几块变了"（见 diffBlocks），再决定重排哪几段。
 * @param doc 修订
 * @param vis 块坐标的可见性口径
 * @return 每块的内容哈希
 */
std::vector<BlockHash> blockHashes(const Doc& doc, FlowVisibility vis);

/**
 * 两份修订之间哪几块要重排，由哈希下降直接算出。
 *
 * vis 是块坐标的可见性口径：呈现态下隐藏子树不产生块，同一个下标在两种可见性下指向不同的
 * 块。它与投影的 ViewParams::vis 必须是同一个值（TRAPS 124）。
 *
 * 与 diffBlocks 的分工：那个比两条算好的块哈希，这个从根往下走、哈希同的子树一步剪掉。
 * 坐标按 after 的块序：尾部删段时 lo == hi == 块数。
 *
 * 局部性：孩子表是 vector，每层对齐都要扫一遍孩子 —— 拿到的是"不做物化、与不相关的容器
 * 无关"，不是"与段数无关"，scanned 就是给这件事留的计数。
 */
struct Damage {
    /**
     * 损伤区间本身。
     */
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

/**
 * 两份修订之间哪几块要重排：从根往下走，哈希相同的子树一步剪掉。
 * @param before 旧修订
 * @param after 新修订
 * @param vis 块坐标的可见性口径
 * @return 损伤区间与比较的代价读数
 */
Damage damage(const Doc& before, const Doc& after, FlowVisibility vis);

}  // namespace heditor::rev
