/**
 * 不可变修订的骨架：节点、路径复制、内容哈希。
 *
 * 判据只有一条：一次编辑之后，旧值仍然可用、且没有被改动。做到这一点之后，"改了什么"
 * 就不再需要事后推导 —— 两份修订的哈希一比，不同的子树就是损伤，而且精确到节点。
 *
 * 切片范围：文字、段落标记、内容控件、表格（表 / 行 / 格）、图片、公式、域、节的页面设置。
 * 页眉页脚的内容（HeaderFooter 子树）按容器下钻，本层不解释它。
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
#include "core/doc/field_spec.h"
#include "core/doc/formula_spec.h"
#include "core/doc/image_spec.h"
#include "core/doc/node.h"
#include "core/doc/para_format.h"
#include "core/doc/section_spec.h"

namespace heditor::rev {
/**
 * 文本 run 的载荷：码点、字符属性、字符样式引用。
 */
struct RunPayload {
    /**
     * 码点序列（偏移就是下标）。
     */
    std::vector<uint32_t> text;

    /**
     * 字符属性（标记）。
     */
    CharFormat marks;

    /**
     * 字符样式引用；空表示不引用。
     */
    std::string style;
};

/**
 * 段落标记的载荷：段落属性的内联覆盖与段落样式引用（第三节：段落属性挂在结束标记上）。
 */
struct MarkPayload {
    /**
     * 段落属性的内联覆盖（只存与样式不同的那些）。
     */
    ParaFormatPatch para;

    /**
     * 段落样式引用；空表示不引用。
     */
    std::string style;
};

struct Node;
using Ref = std::shared_ptr<const Node>;

/**
 * 节点载荷：种类决定哪一支（文本带字符属性、段落标记带段落覆盖、控件带控件记录、表格三种
 * 节点各带自己的记录、图片与公式各带自己的记录、域带域记录、节带页面设置，其余不带载荷）。
 *
 * 一支一种记录，不做"表格载荷里再套三种"：种类与载荷的对应是一条查表就能写出来的规则，
 * 而多一层包装会让 hashNode 与落盘都要再分一次支。
 */
using Payload = std::variant<std::monostate, RunPayload, MarkPayload, ControlSpec, TableSpec,
                             RowSpec, CellSpec, ImageSpec, FormulaSpec, FieldSpec, SectionSpec>;

/**
 * 一个不可变节点。构造之后不再改动，编辑一律走"换一条路径"（见 replaceNode）。
 *
 * hidden 是作者设定的默认可见性（只在呈现态生效）：它在哈希里，因为呈现态下它决定的
 * 不只是"画不画"，还有位置空间 —— 摘掉一棵子树，它之后的所有位置都前移。
 *
 * paras 是这棵子树里的段落标记数（顺序统计量）。它由内容唯一确定，所以不进哈希，但必须
 * 在构造时算好：比较两份修订时要靠它把"哪一棵子树变了"直接换成一端块区间，否则每层都得
 * 重新数一遍子树。这是顺序统计量树的标量版，只到"多少块"这一档，不做到"第几块"。
 *
 * 块数随可见性变，所以统计量有两个（paras / paras_display）：呈现态下隐藏子树不产生块。
 * 一个数走不到两处 —— 谁要块数，谁就得说清按哪种可见性数。
 */
struct Node {
    /**
     * 节点身份。不进哈希 —— 它表达跨修订的同一性，不表达内容。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 结构种类，决定载荷是哪一支。
     */
    NodeKind kind = NodeKind::TextRun;

    /**
     * 作者设定的默认可见性（只在呈现态生效）。它在哈希里。
     */
    bool hidden = false;

    /**
     * 这棵子树里的段落标记数（编辑态）。派生量，不进哈希。
     */
    int paras = 0;

    /**
     * 呈现态（Display）下这棵子树里有几个段落标记 —— 与 paras 是两个数，不是一个。
     *
     * 隐藏的子树整棵不在呈现态的流里，它里面的段落标记也就不产生块。损伤判定按"哪种可见性"
     * 数块，必须读对应的那一个：两者混用会让 after_blocks 与投影的块数不等，按区间搬块时
     * shift 算错，表现是段落重复（见 TRAPS 124）。它同样不进哈希 —— 由 hidden 与内容唯一
     * 确定，不是新的作者意图。
     */
    int paras_display = 0;

    /**
     * 这一支种类自己的记录。
     */
    Payload payload;

    /**
     * 子节点（共享引用，不可变）。
     */
    std::vector<Ref> children;

    /**
     * 自身哈希的低 64 位 —— 种类、隐藏、载荷。
     */
    uint64_t hash_lo = 0;

    /**
     * 自身哈希的高 64 位。
     */
    uint64_t hash_hi = 0;
};

/**
 * 一份修订：根节点、全部内容的哈希、下一个可用的节点号。
 *
 * 根是 Document，其下是一个 Section（本切片不分节），Section 的子序列是 run、段落标记与
 * 控件交替排列的流 —— 与模型层的节点树同一个形状，将来换到真正的树上时不用再翻译一次。
 */
struct Doc {
    /**
     * 根节点（Document）。
     */
    Ref root;

    /**
     * 全文内容哈希的低 64 位。
     */
    uint64_t lo = 0;

    /**
     * 全文内容哈希的高 64 位。
     */
    uint64_t hi = 0;

    /**
     * 下一个可用的节点号。
     */
    NodeId next_id = 1;

    /**
     * 这份修订有效吗（根非空）。
     * @return true = 有根
     */
    explicit operator bool() const { return root != nullptr; }
};

/**
 * 一次编辑的结果。失败时带原因，不返回一个"看起来没变"的文档 —— 调用方分不清
 * "编辑被拒绝"与"编辑没产生变化"。
 */
struct EditResult {
    /**
     * 成了没有。
     */
    bool ok = false;

    /**
     * 新的修订；失败时是原样返回的旧值（不要用）。
     */
    Doc doc;

    /**
     * 失败原因。
     */
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

    /**
     * 混进一个有符号整数（按位当无符号）。
     * @param v 要混进去的值
     */
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }

    /**
     * 混进一个字节（种类、枚举这类）。
     * @param v 要混进去的值
     */
    void u8(uint8_t v) { u64(v); }

    /**
     * 混进一个浮点数。按位混，避免 -0.0 与 0.0 混成同一个。
     * @param v 要混进去的值
     */
    void f32(float v);

    /**
     * 混进一个字符串（长度与字节一起混）。
     * @param s 要混进去的串
     */
    void str(const std::string& s);

    /**
     * 低 64 位。
     * @return 累计值
     */
    uint64_t lo() const { return lo_; }

    /**
     * 高 64 位。
     * @return 累计值
     */
    uint64_t hi() const { return hi_; }

private:

    uint64_t lo_ = 1469598103934665603ull;

    /**
     * 高位的累加器（初始值与 lo_ 不同，两条通道不同源）。
     */
    uint64_t hi_ = 14695981039346656037ull;
};

/**
 * 一个节点自身的哈希：种类、隐藏标记、载荷，不含孩子。
 *
 * 完整节点哈希是"自身哈希 + 孩子哈希"，所以两者之间不需要另一份定义。损伤判定要区分
 * "容器自己的属性变了"与"只是里面某处变了"：前者要把它覆盖的块整段算进损伤（列宽变了，
 * 同一张表每一行的行高都可能变），后者只算变的那一块。
 * @param h 累加器
 * @param node 节点
 */
void hashSelf(Hasher& h, const Node& node);

/**
 * 正文那一节的子序列（本切片不分节）。没有正文时返回 nullptr。
 * @param doc 修订
 * @return 正文那一节的孩子表；没有正文时为 nullptr
 */
const std::vector<Ref>* bodyOf(const Doc& doc);

/**
 * 按节点号找节点（只走正文，线性扫描）。本切片没有索引，用途是读属性。
 * @param doc 修订
 * @param id 节点号
 * @return 节点；找不到为 nullptr
 */
const Node* findNode(const Doc& doc, NodeId id);

/**
 * 父节点。修订没有向上的指针（它是不可变树，加一条反向链就要在每次路径复制时维护两处），
 * 所以按节点号现找。命令层用它把"某个节点"换成"某个孩子槽位"。
 * @param doc 修订
 * @param id 节点号
 * @return 父节点；找不到或它是根时为 nullptr
 */
const Node* parentOf(const Doc& doc, NodeId id);

/**
 * 反向桥：修订 → 模型树。节点号不保留（模型树的节点由它自己分配），内容逐字段带过去。
 *
 * 它只有一个用途 —— 审计：引擎产物与另一条独立路径（compileDocument）比较时，参照侧需要
 * 一棵模型树。编辑路径不用它，否则就又回到"每条编辑造一棵树"。
 * @param doc 修订
 * @param out 写出的模型树（先清空）
 */
void toTree(const Doc& doc, NodeTree* out);

/**
 * 建一个节点：哈希与段落数由孩子算出来，调用方给的是内容与身份，不是派生量。
 *
 * 派生量不从外面填，是不给"第二个可写真相"留口子：节点若能带着别人算好的哈希建出来，
 * 就多了一处能对不上的地方。反序列化与"从模型导出修订"用它。
 * @param id 节点号
 * @param kind 节点种类
 * @param payload 这一支种类自己的记录
 * @param children 孩子（共享引用）
 * @param hidden 作者设定的默认可见性
 * @return 新节点（哈希与段落数已由孩子算好）
 */
Ref makeNode(NodeId id, NodeKind kind, Payload payload, std::vector<Ref> children,
             bool hidden = false);

/**
 * 用一棵树与分配器水位装出一份修订；全文哈希取自根。
 *
 * 水位决定后续分配的起点。反序列化按文件里的值恢复它，否则新建的节点会与文件里引用过的
 * 身份撞上 —— 那种撞车是静默的：改一个节点会连带改到另一个同号节点。
 * @param root 根节点
 * @param next_id 下一个可用的节点号（反序列化按文件里的水位给）
 * @return 一份修订
 */
Doc adopt(Ref root, NodeId next_id);

/**
 * 从模型树导出一份修订：节点号原样带过来（身份跨模型同源），载荷按种类一一对应，
 * hidden 与分配器水位一起带。
 *
 * 这是迁移路径，也是等价判据的桥：project(fromTree(tree)) 与 compileDocument(tree)
 * 逐字段相同，才能说两条路是同一件事。
 *
 * 表示不了的形状直接拒绝、并说明原因，不做"能搬多少搬多少"：搬一半的结果是一份看起来
 * 完整的修订 —— 它与模型不是同一份文档，而调用方没有别的办法知道。本切片拒绝不认识的根
 * 与"非文档 / 非节"的子节点。
 * @param tree 模型树
 * @param out 写出的修订（成功时才动）
 * @param error 写回失败原因
 * @return true = 表示得了；false 时 out 不动
 */
bool fromTree(const NodeTree& tree, Doc* out, std::string* error);

/**
 * 用若干段纯文本建一份修订。空段落被保留：它由一个段落标记单独表示。
 * @param paragraphs 每段一份码点序列
 * @return 一份修订
 */
Doc makeTextDoc(const std::vector<std::u32string>& paragraphs);

/**
 * 段落数（等于流里段落标记的个数，含控件内部的标记）。
 *
 * 直接读根的 paras。现数一遍的写法在 locate 的循环里就是 O(段数 × 每段节点数)——
 * "定位一段"与整篇规模挂钩，而它每敲一个字都要跑一次。
 * @param doc 修订
 * @return 段落数（含控件内部的标记）
 */
int paraCount(const Doc& doc);

/**
 * 每段的文本。只拼 run 的码点，不还原字符属性与段落格式；控件里的文字按流顺序并入。
 * @param doc 修订
 * @return 每段的文本（只拼 run 的码点）
 */
std::vector<std::u32string> paraTexts(const Doc& doc);

/**
 * 在第 para 段的第 cp 个码点之前插入文本。
 *
 * cp 的取址空间是这一段自己的码点序列（含控件内部的文字），[0, 段长] 都合法。插入的文本
 * 继承插入点的字符属性（打在加粗的 run 里就该是加粗的），所以并进那个 run、不新起一个。
 * 位置在控件内部时，改的是控件那一层的孩子表，容器之外的孩子一个都不复制。
 * @param doc 修订
 * @param para 段号
 * @param cp 段内码点位置
 * @param text 要插入的文本
 * @return 结果（失败时带原因，修订不动）
 */
EditResult insertText(const Doc& doc, int para, int cp, const std::u32string& text);

/**
 * 删掉第 para 段从第 cp 个码点起的 count 个码点。
 *
 * 只在本段内删：够不到就删到段末为止，不跨过段落标记。跨段删除是"删一段"的另一种语义，
 * 由调用方拆成"删本段余下"加"删下一段整段"，不该由这里替它猜。
 * @param doc 修订
 * @param para 段号
 * @param cp 起始码点位置
 * @param count 删几个码点
 * @return 结果（失败时带原因，修订不动）
 */
EditResult eraseText(const Doc& doc, int para, int cp, int count);

/**
 * 在第 para 段的第 cp 个码点处断开，插入一个段落标记。
 *
 * cp 取 0 或段长都合法，结果是多出一个空段。新段落标记默认没有内联覆盖与样式引用。
 * 位置在控件内部时那枚标记也落在控件里 —— 控件因此跨段（投影里它的起止在不同块）。
 * @param doc 修订
 * @param para 段号
 * @param cp 断开处的段内码点位置
 * @return 结果（失败时带原因，修订不动）
 */
EditResult splitPara(const Doc& doc, int para, int cp);

/**
 * 把第 para 段与下一段合并：删掉它们之间的段落标记。
 *
 * 两段各自的 run 保持原样（不合并同属性的相邻 run），文本拼接的结果与逐段读出再相加相同。
 * @param doc 修订
 * @param para 要并入下一段的段号
 * @return 结果（失败时带原因，修订不动）
 */
EditResult mergePara(const Doc& doc, int para);

/**
 * 在第 para 段的第 cp 个码点之前插入一个内容控件，内容是一段文本。
 *
 * 括号由投影按边界产生，不占文本位置（见 compile.cpp 里控件那一段的次序说明）。
 * label 为空时控件里没有内容，投影出来是一对相邻的边界。created 非空时写回新控件的节点号 ——
 * 紧接着要设隐藏或回填值的调用方不该再去树里按形状把它找一遍。
 * @param doc 修订
 * @param para 段号
 * @param cp 插入处的段内码点位置
 * @param spec 控件记录
 * @param label 控件里的内容
 * @param created 写回新控件的节点号（可为 nullptr）
 * @return 结果（失败时带原因，修订不动）
 */
EditResult insertControl(const Doc& doc, int para, int cp, const ControlSpec& spec,
                         const std::u32string& label, NodeId* created = nullptr);

/**
 * 设置一个节点的隐藏标记（作者意图，只在呈现态生效）。节点不存在时返回失败。
 * @param doc 修订
 * @param id 节点号
 * @param hidden true = 隐藏
 * @return 结果（节点不存在时失败）
 */
EditResult setHidden(const Doc& doc, NodeId id, bool hidden);

}  // namespace heditor::rev
