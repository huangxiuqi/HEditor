/**
 * 不可变修订的骨架：节点、路径复制、内容哈希。
 *
 * 判据只有一条：一次编辑之后，旧值仍然可用、且没有被改动。做到这一点之后，"改了什么"
 * 就不再需要事后推导 —— 两份修订的哈希一比，不同的子树就是损伤，而且精确到节点。
 *
 * 切片范围：文字、段落标记、内容控件。表格、域、图片、节属性留给后续切片。
 * 收窄不是留占位 —— 这一层表示不了的形状根本构造不出来，makeTextDoc 只产出前两类，
 * 控件只能经 insertControl 建出来。加新节点种类时 payload 的 variant 多一支，
 * 哈希用 std::visit 且不写默认分支，漏写一支直接编不过，不会静默算出一个不完整的哈希。
 *
 * 编辑入口认的是流位置，不看层级：控件内部的文字与顶层文字是同一段文字的两部分，所以把字打在
 * 控件里与打在同一段的别处走同一条路 —— 落点带一条从节往下的路径，改动沿路径换回去。
 * 读（paraTexts / paraCount）同样按流顺序下钻，与编译同一口径。
 *
 * 身份（NodeId）不进哈希：哈希表达内容，身份表达"跨修订的同一性"。撤销回到旧修订
 * 时内容哈希必须相等，而节点身份可以一路往前走。
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "core/doc/char_format.h"
#include "core/doc/control_spec.h"
#include "core/doc/node.h"
#include "core/doc/para_format.h"

namespace heditor::rev {

/**
 * 文本 run 的载荷：码点、字符属性、字符样式引用。
 */
struct RunPayload {
    std::vector<uint32_t> text;
    CharFormat marks;
    std::string style;
};

/**
 * 段落标记的载荷：段落属性的内联覆盖与段落样式引用（第三节：段落属性挂在结束标记上）。
 */
struct MarkPayload {
    ParaFormatPatch para;
    std::string style;
};

struct Node;
using Ref = std::shared_ptr<const Node>;

/**
 * 一个不可变节点。构造之后不再改动，编辑一律走"换一条路径"（见 replaceNode）。
 *
 * hidden 是作者设定的默认可见性（只在呈现态生效）：它在哈希里，因为呈现态下它决定的
 * 不只是"画不画"，还有位置空间 —— 摘掉一棵子树，它之后的所有位置都前移。
 *
 * paras 是这棵子树里的段落标记数（顺序统计量）。它由内容唯一确定，所以不进哈希，但必须
 * 在构造时算好：比较两份修订时要靠它把"哪一棵子树变了"直接换成一端块区间，否则每层都得
 * 重新数一遍子树。这是顺序统计量树的标量版，只到"多少块"这一档，不做到"第几块"。
 */
struct Node {
    NodeId id = kInvalidNodeId;
    NodeKind kind = NodeKind::TextRun;
    bool hidden = false;
    int paras = 0;
    std::variant<std::monostate, RunPayload, MarkPayload, ControlSpec> payload;
    std::vector<Ref> children;
    uint64_t hash_lo = 0;
    uint64_t hash_hi = 0;
};

/**
 * 一份修订：根节点、全部内容的哈希、下一个可用的节点号。
 *
 * 根是 Document，其下是一个 Section（本切片不分节），Section 的子序列是 run、段落标记与
 * 控件交替排列的流 —— 与模型层的节点树同一个形状，将来换到真正的树上时不用再翻译一次。
 */
struct Doc {
    Ref root;
    uint64_t lo = 0;
    uint64_t hi = 0;
    NodeId next_id = 1;

    explicit operator bool() const { return root != nullptr; }
};

/**
 * 一次编辑的结果。失败时带原因，不返回一个"看起来没变"的文档 —— 调用方分不清
 * "编辑被拒绝"与"编辑没产生变化"。
 */
struct EditResult {
    bool ok = false;
    Doc doc;
    std::string error;
};

/**
 * 128 位内容哈希的累加器。
 *
 * 节点哈希与块哈希共用它 —— 两处必须用同一套混合，否则"块哈希变了"与"节点哈希变了"
 * 不可比，按哈希做的复用也就无从判断。两条通道真的不同源（乘子与混合方式都不同），
 * 否则 128 位只是同一个 64 位写了两遍。
 */
class Hasher {
public:
    void u64(uint64_t v) {
        lo_ = (lo_ ^ v) * 1099511628211ull;
        hi_ = (hi_ ^ (v + 0x9E3779B97F4A7C15ull)) * 14029467366897019727ull;
        hi_ ^= hi_ >> 31;
    }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void u8(uint8_t v) { u64(v); }
    void f32(float v);
    void str(const std::string& s);

    uint64_t lo() const { return lo_; }
    uint64_t hi() const { return hi_; }

private:
    uint64_t lo_ = 1469598103934665603ull;
    uint64_t hi_ = 14695981039346656037ull;
};

/**
 * 正文那一节的子序列（本切片不分节）。没有正文时返回 nullptr。
 */
const std::vector<Ref>* bodyOf(const Doc& doc);

/**
 * 按节点号找节点（只走正文，线性扫描）。本切片没有索引，用途是读属性。
 */
const Node* findNode(const Doc& doc, NodeId id);

/**
 * 建一个节点：哈希与段落数由孩子算出来，调用方给的是内容与身份，不是派生量。
 *
 * 派生量不从外面填，是不给"第二个可写真相"留口子：节点若能带着别人算好的哈希建出来，
 * 就多了一处能对不上的地方。反序列化与"从模型导出修订"用它。
 */
Ref makeNode(NodeId id, NodeKind kind,
             std::variant<std::monostate, RunPayload, MarkPayload, ControlSpec> payload,
             std::vector<Ref> children, bool hidden = false);

/**
 * 用一棵树与分配器水位装出一份修订；全文哈希取自根。
 *
 * 水位决定后续分配的起点。反序列化按文件里的值恢复它，否则新建的节点会与文件里引用过的
 * 身份撞上 —— 那种撞车是静默的：改一个节点会连带改到另一个同号节点。
 */
Doc adopt(Ref root, NodeId next_id);

/**
 * 用若干段纯文本建一份修订。空段落被保留：它由一个段落标记单独表示。
 */
Doc makeTextDoc(const std::vector<std::u32string>& paragraphs);

/**
 * 段落数（等于流里段落标记的个数，含控件内部的标记）。
 *
 * 直接读根的 paras。现数一遍的写法在 locate 的循环里就是 O(段数 × 每段节点数)——
 * "定位一段"与整篇规模挂钩，而它每敲一个字都要跑一次。
 */
int paraCount(const Doc& doc);

/**
 * 每段的文本。只拼 run 的码点，不还原字符属性与段落格式；控件里的文字按流顺序并入。
 */
std::vector<std::u32string> paraTexts(const Doc& doc);

/**
 * 在第 para 段的第 cp 个码点之前插入文本。
 *
 * cp 的取址空间是这一段自己的码点序列（含控件内部的文字），[0, 段长] 都合法。插入的文本
 * 继承插入点的字符属性（打在加粗的 run 里就该是加粗的），所以并进那个 run、不新起一个。
 * 位置在控件内部时，改的是控件那一层的孩子表，容器之外的孩子一个都不复制。
 */
EditResult insertText(const Doc& doc, int para, int cp, const std::u32string& text);

/**
 * 删掉第 para 段从第 cp 个码点起的 count 个码点。
 *
 * 只在本段内删：够不到就删到段末为止，不跨过段落标记。跨段删除是"删一段"的另一种语义，
 * 由调用方拆成"删本段余下"加"删下一段整段"，不该由这里替它猜。
 */
EditResult eraseText(const Doc& doc, int para, int cp, int count);

/**
 * 在第 para 段的第 cp 个码点处断开，插入一个段落标记。
 *
 * cp 取 0 或段长都合法，结果是多出一个空段。新段落标记默认没有内联覆盖与样式引用。
 * 位置在控件内部时那枚标记也落在控件里 —— 控件因此跨段（投影里它的起止在不同块）。
 */
EditResult splitPara(const Doc& doc, int para, int cp);

/**
 * 把第 para 段与下一段合并：删掉它们之间的段落标记。
 *
 * 两段各自的 run 保持原样（不合并同属性的相邻 run），文本拼接的结果与逐段读出再相加相同。
 */
EditResult mergePara(const Doc& doc, int para);

/**
 * 在第 para 段的第 cp 个码点之前插入一个内容控件，内容是一段文本。
 *
 * 括号由投影按边界产生，不占文本位置（见 compile.cpp 里控件那一段的次序说明）。
 * label 为空时控件里没有内容，投影出来是一对相邻的边界。created 非空时写回新控件的节点号 ——
 * 紧接着要设隐藏或回填值的调用方不该再去树里按形状把它找一遍。
 */
EditResult insertControl(const Doc& doc, int para, int cp, const ControlSpec& spec,
                         const std::u32string& label, NodeId* created = nullptr);

/**
 * 设置一个节点的隐藏标记（作者意图，只在呈现态生效）。节点不存在时返回失败。
 */
EditResult setHidden(const Doc& doc, NodeId id, bool hidden);

}  // namespace heditor::rev
