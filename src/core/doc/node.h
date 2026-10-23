/**
 * 文档模型的节点基类与节点树。
 *
 * 阶段 0 的地基：只有"节点是什么、怎么编号、谁是谁的孩子"，不含编辑语义
 * （段落标记进流在 0.2、增删步骤在 1.2、编译到引擎在 0.4）。先做这些是因为
 * 后面每一步都拿节点身份与树结构当输入 —— 位置、步骤、不变量、序列化都建立在它上面。
 *
 * 三类节点加一种流内分隔元素（见 docs/文档模型设计.md 第三节）：容器（有子节点）、
 * 原子（不可再分的叶子）、文本（TextRun），以及 ParagraphMark —— 它既不是容器也不是
 * 原子，是流里标记段落结束的分隔元素，也是段落属性的唯一宿主。
 * 刻意没有"行内 / 块级"二分：容器的差别只在接受哪些子类型，以及跨度是否跨过段落标记。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/doc/char_format.h"
#include "core/doc/control_spec.h"
#include "core/doc/field_spec.h"
#include "core/doc/formula_spec.h"
#include "core/doc/image_spec.h"
#include "core/doc/node_id.h"
#include "core/doc/para_format.h"
#include "core/doc/section_spec.h"
#include "core/doc/table_spec.h"

namespace heditor {
/**
 * 节点种类：只回答"结构上怎么处理"这一个问题。
 *
 * 不按控件的业务类型分：复选框、单选、下拉、日期都是 ContentControl，差别在
 * controlType 这个数据字段上（第七节）：业务类型会一直增加、宿主还能自带一种，
 * 而结构上只有"能不能装子节点"这一条差别。把业务类型写进枚举，等于每加一种控件都要同时改
 * 容器判定、不变量检查器、编译与序列化四处。控件的结构形态（容器型 / 原子型）也是
 * 控件类型声明里的一项数据，不是节点种类 —— 所以控件现在只有 ContentControl 一种种类。
 */
enum class NodeKind : uint8_t {
    /**
     * 容器：持有子节点序列，在流里表现为「起始边界 … 内容 … 结束边界」
     */
    Document = 0,
    Section,
    Table,
    TableRow,
    TableCell,
    ContentControl,
    Quote,
    CodeBlock,
    ListItem,
    Link,
    /**
     * 页眉页脚的内容片段根（第十节）。它装块级内容，但不在正文流里：位置换算与段落序号
     * 都看不到它，它由节的记录 HeaderFooter::root 引用，走自己的编译路径。单独一种容器，
     * 是因为"在哪条流里"是结构差别，不是属性差别。
     */
    HeaderFooter,
    /**
     * 原子：不可再分，在流里是一个带宽度、参与断行的叶子
     */
    Image,
    Formula,
    Break,
    PageBreak,
    Tab,
    Field,
    /**
     * 文本：一段共享同一套标记的连续文本，边界由标记集合的变化决定
     */
    TextRun,
    /**
     * 流内分隔元素：标记段落结束，段落属性挂在它身上
     */
    ParagraphMark,
};

/**
 * 是否持有子节点。只有容器能装东西，加子节点前必须过这一关。
 * @param kind 节点种类
 * @return true = 持有子节点
 */
bool isContainer(NodeKind kind);

/**
 * 是否为文本节点：内部按码点编址，在流里按码点展开。
 * @param kind 节点种类
 * @return true = 文本节点
 */
bool isText(NodeKind kind);

/**
 * 是否为流内分隔元素。单独判定是因为它既不是容器也不是原子，
 * 但流位置换算必须给它一个位置 —— 段落序号正是数它的个数得来的。
 * @param kind 节点种类
 * @return true = 段落标记
 */
bool isParagraphMark(NodeKind kind);

/**
 * 是否为原子：在流里整枚占一个位置、没有可寻址的内部（图片、公式、域、制表符、分页符、换行）。
 *
 * 单独判定是因为"删掉它"只有整枚移除这一种做法：文本按码点删、容器按边界进出，而原子既没有
 * 可寻址的内部、也不是结构边界。它同时也是排版层"不可断的盒子"那一类，但两者不是一回事 ——
 * 排版只认已经进了编译产物的那几种（见 InlineObjectKind::Atom）。
 * @param kind 节点种类
 * @return true = 原子（整枚占一个流位置）
 */
bool isAtom(NodeKind kind);

/**
 * 种类名，与枚举逐项对应。用于诊断与报告文本（拿数字去对照枚举太费事），
 * 也让"新加一个结构种类"这件事集中在一处 —— switch 不写 default，
 * 少写一种时 -Wswitch 会报出来。
 * @param kind 节点种类
 * @return 名字
 */
const char* nodeKindName(NodeKind kind);

/**
 * 种类名 → 种类。不认识的名字返回 false。
 *
 * 序列化（1.10）按名字落盘、按名字读回，而不是写枚举值：枚举值一旦重排或插入，旧文件里
 * 的数字就会静默变成另一个种类，而名字不会。这个名字来回的唯一真相就是 nodeKindName。
 * @param name 名字
 * @param out 写回种类
 * @return true = 名字认识
 */
bool nodeKindFromName(const std::string& name, NodeKind* out);

/**
 * 谁允许装谁。只登记第三节已经定下的三条强约束（根只收节、表格只收行、行只收单元格），
 * 其余容器不限制子节点 —— 子节点约束是一个位标志的并集，尚未逐种登记。
 *
 * 放在这一层而不是检查器内部：步骤构造也要用它（未通过就在构造时拒掉，而不是先造出非法文档
 * 再等检查器报）。两处各写一份，迟早会在"新加一种容器"时不一致。
 * @param parent 父节点种类
 * @param child 子节点种类
 * @return true = 允许这样嵌套
 */
bool acceptsChild(NodeKind parent, NodeKind child);

/**
 * 节点。字段公开：它是纯数据，封装留在树那一层（见 NodeTree）。
 * 给每个字段配一对访问器只会让"谁改了 parent"更难查。
 */
struct Node {
    /**
     * 节点身份。分配之后永不复用，编辑之后仍然有效。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 结构种类，决定这个节点怎么处理。
     */
    NodeKind kind = NodeKind::TextRun;

    /**
     * 父节点；根为 nullptr。
     */
    Node* parent = nullptr;

    /**
     * 子节点（仅容器非空），顺序就是流顺序。
     */
    std::vector<Node*> children;

    /**
     * 文本码点（仅文本节点非空），偏移就是下标。
     */
    std::vector<uint32_t> text;

    /**
     * 隐藏（第七节）：作者设定的默认可见性，只在非编辑模式下生效。
     * 放在节点上而不是另建一张表，是因为流位置换算要看它 —— 非编辑模式下隐藏的控件
     * 整棵子树从流里摘除，位置空间随之变化，所以 FlowIndex 必须带可见性口径。
     */
    bool hidden = false;

    /**
     * 段落格式的内联覆盖：只对 ParagraphMark 有意义（第三节：段落属性挂在结束标记上）。
     *
     * 它是 patch 而不是完整的 ParaFormat：元素只存与样式不同的那些属性（第六节），完整形式
     * 一挂上就"处处都是内联覆盖"，样式再也起不了作用。解析按"文档默认值 → 样式链 → 这里"
     * 三层做，编译时执行一次。
     *
     * 放在节点上而不是另建一张表，理由同 hidden —— 编译要按流顺序取它，而流顺序就是树的顺序。
     * 其余种类读它没有定义。
     */
    ParaFormatPatch para;

    /**
     * 样式引用：段落标记上指段落样式，文本节点上指字符样式（第六节）。空表示不引用。
     *
     * 一个字段兼两种含义，与 para / marks / control 一样按种类区别取用：段落样式与字符样式
     * 在解析里本来就是两段独立的链，节点上不需要再分两个字段。
     */
    std::string styleRef;

    /**
     * 字符属性：只对 TextRun 有意义（第二节：标记是文本节点上的属性集合）。
     * 与 para 同一个理由：规范化与编译都要按流顺序取它。其余种类读它没有定义。
     */
    CharFormat marks;

    /**
     * 控件记录：只对 ContentControl 有意义。取值、索引与表单模式的落点判定都读它，
     * 理由同 para —— 都按流顺序取。其余种类读它没有定义。
     */
    ControlSpec control;

    /**
     * 单元格记录（跨度与垂直对齐）：只对 TableCell 有意义（第九节）。被合并掉的格位在模型里
     * 不存在，所以跨度就是"这一格覆盖多少格位"的全部信息，网格由它推出来。其余种类读它没有定义。
     */
    CellSpec cell;

    /**
     * 行记录（跨页三个开关与行高）：只对 TableRow 有意义。与 cell 对 TableCell 的关系同构，
     * 理由也同 —— 编译与排版按流顺序取它。其余种类读它没有定义。
     */
    RowSpec row;

    /**
     * 表格自己的记录（列定义、显式列宽、宽度与对齐）：只对 Table 有意义。默认是空的普通表格，
     * 列由跨度派生。其余种类读它没有定义。
     */
    TableSpec table;

    /**
     * 域记录：只对 Field 有意义（第八节）。缓存值求值写入，其余字段是作者给的种类与属性名。
     * 理由同 para —— 求值按流顺序走一遍树。其余种类读它没有定义。
     */
    FieldSpec field;

    /**
     * 图片记录：只对 Image 有意义（第十一节）。资源标识、原始像素尺寸与显示尺寸都在这里，
     * 理由同 para —— 编译按流顺序取它，而流顺序就是树的顺序。其余种类读它没有定义。
     */
    ImageSpec image;

    /**
     * 公式记录：只对 Formula 有意义。表达式、排版后端与显示尺寸。其余种类读它没有定义。
     */
    FormulaSpec formula;

    /**
     * 节记录：只对 Section 有意义（第十节）。纸张、页边距、页眉页脚引用、页码，以及装未知
     * 页面设置的属性袋。其余种类读它没有定义。
     */
    SectionSpec section;
};

/**
 * 可见性口径：隐藏的控件在哪种模式下生效（第七节）。
 *
 * 编辑模式是设计态，隐藏的控件照常显示、照常参与排版，否则作者没法选中它改回来；
 * 表单与只读模式下它整棵子树从流里摘除，不显示、不占位置。所以位置空间与段落结构
 * 都随模式变 —— 两套派生索引（流位置、段落）都要带上这个参数。
 * 放在这里而不是各自定义，是因为它的成因是 Node.hidden 这一条属性。
 */
enum class FlowVisibility : uint8_t {
    Design,   // 编辑（设计）态：隐藏的控件照常参与流
    Display,  // 表单与只读：隐藏的控件整棵子树摘除，不占流位置
};

/**
 * 节点树：持有全部节点、分配 ID、维护父子链。
 *
 * 构造时建出 Document → Section 骨架（第二节：根节点唯一，其下是节序列）。
 *
 * 改结构只有两条路径：insertChild 与 eraseChild。刻意不提供第二种插入方式
 * （例如"插到某个兄节点之后"）—— 不变量检查器（0.3）与派生索引的增量维护都要挂在
 * 每一条改动路径上，多一条就多一处能漏掉的地方。编辑步骤的语义（逆操作、位置映射、
 * 撤销分组）是 1.2 的事，建在这两条原语之上，不在这层。
 */
class NodeTree {
public:

    NodeTree();

    /**
     * 根节点 id。
     * @return 根的 id
     */
    NodeId rootId() const { return root_->id; }

    /**
     * 节序列的第一节，根下没有节时返回 kInvalidNodeId。
     *
     * 它按根的孩子现算而不缓存一个指针：缓存会在"根的孩子被换掉"（序列化还原、未来的删节）
     * 之后指向一个已经摘下的节点，而那种失效是静默的。根的孩子本来就只有节、数量也小。
     * @return 第一节的 id；根下没有节时为 kInvalidNodeId
     */
    NodeId firstSectionId() const;

    /**
     * 节的个数与其第 i 节的 id（i 从 0 起，越界返回 kInvalidNodeId）。
     *
     * 分节（第十节）之后文档下有多个节，按序访问与逐节求值都需要它。顺序枚举走一遍根的孩子
     * 即可：根的合法孩子只有节，数量也小，不值得再维护一张顺序表。
     * @return 节的个数
     */
    int sectionCount() const;

    /**
     * 第 i 节的 id。
     * @param index 序号（0 起）
     * @return 节的 id；越界为 kInvalidNodeId
     */
    NodeId sectionAt(int index) const;

    /**
     * 找不到时返回 nullptr，不抛：拿 id 来查的多半是调用方自己算错了。
     *
     * 它也能查到已被摘下、留在池里的子树（撤销要把它们原样挂回，所以节点不销毁、id 不回收）。
     * 所以"这个 id 还在文档里吗"要另外问 isAttached，只看 find 非空会把删掉的东西当成活的。
     * @param id 节点 id
     * @return 节点；找不到为 nullptr
     */
    Node* find(NodeId id);

    /**
     * 按 id 查节点（只读）。
     * @param id 节点 id
     * @return 节点；找不到为 nullptr
     */
    const Node* find(NodeId id) const;

    /**
     * 在 parent 的第 at 个槽位插入一个 kind 类型的子节点，返回新 id。
     * at 取 [0, 子节点数]，等于子节点数就是追加。parent 不存在、kind 不是容器、
     * 或 at 越界时返回 kInvalidNodeId —— 不做隐式纠正。
     * @param parent 父节点 id
     * @param at 槽位（0..子节点数，等于子节点数就是追加）
     * @param kind 新节点的种类
     * @return 新节点 id；parent 不是容器或 at 越界时为 kInvalidNodeId
     */
    NodeId insertChild(NodeId parent, int at, NodeKind kind);

    /**
     * 移除 parent 的第 at 个子节点（连同它的整棵子树），返回被摘下子树的根 id。
     * 摘下的节点仍留在池里、ID 不回收，只是不再挂在树上 —— 撤销要把它原样搬回去，
     * 而"搬回去"要求身份不变，所以这里不能销毁节点。at 越界或 parent 不存在时
     * 返回 kInvalidNodeId。
     * @param parent 父节点 id
     * @param at 槽位
     * @return 被摘下子树的根 id；越界时为 kInvalidNodeId
     */
    NodeId eraseChild(NodeId parent, int at);

    /**
     * 追加到末尾。等价于 insertChild(parent, 子节点数, kind)，只是省去调用方数一遍。
     * @param parent 父节点 id
     * @param kind 新节点的种类
     * @return 新节点 id；失败为 kInvalidNodeId
     */
    NodeId appendChild(NodeId parent, NodeKind kind);

    /**
     * 把一棵已经摘下的子树挂回 parent 的第 at 个槽位，返回它的根 id（身份不变）。
     *
     * 这是 insertChild 与 eraseChild 之外的第三条挂载路径，存在的唯一理由是"撤销要把它原样
     * 搬回去"：insertChild 会新分配 id，搬回去就丢了身份，而控件这类节点是有身份的（宿主持着
     * 它做取值与回填）。要求 detached 存在、当前没有父节点、且挂进去不会成环；
     * 任一条件不满足都返回 kInvalidNodeId，不做隐式纠正。
     * @param parent 挂到哪个节点下
     * @param at 槽位
     * @param detached 已摘下的子树的根 id
     * @return 它的根 id（身份不变）；条件不满足时为 kInvalidNodeId
     */
    NodeId attachChild(NodeId parent, int at, NodeId detached);

    /**
     * 建一个不挂在树上的节点，返回它的 id。
     *
     * 它不是一条结构改动路径（文档没有变化），只是让一棵待插的子树先有固定身份：节点级步骤把
     * 子树物化成摘下的，再靠 attachChild 挂上、eraseChild 拿下，于是应用、撤销与重做之间
     * 同一个节点的 id 始终不变。摘下的节点不进流、不参与不变量检查。
     * @param kind 节点种类
     * @return 新节点的 id
     */
    NodeId createDetached(NodeKind kind);

    /**
     * 设置文本节点的码点。id 不存在或不是文本节点时返回 false。
     * @param id 文本节点
     * @param cps 新的码点序列
     * @return true = 换好了
     */
    bool setText(NodeId id, std::vector<uint32_t> cps);

    /**
     * 池里的节点数（含已摘下的）。
     * @return 节点数
     */
    size_t nodeCount() const { return pool_.size(); }

    /**
     * 迄今分配过的最大 ID。序列化要用它续上分配器（1.10）。
     * @return 迄今分配过的最大 id（0 表示还没分配过）
     */
    NodeId maxAllocatedId() const { return next_id_ - 1; }

    /**
     * 把另一棵树的内容整体拷过来：先清空自己，再按原 id 重建全部节点与父子链，
     * 连摘下的子树与节的页眉页脚引用一起带上，分配器也续在原处。
     *
     * 提供的是显式的拷贝入口而不是拷贝构造：整棵树的深拷贝很贵，隐式拷贝会藏在
     * "按值传一个 NodeTree"这种看不出代价的地方。修订留痕（1.9）的基线要的就是一份
     * 独立的值拷贝 —— 基线不能被后续编辑改到。
     * @param other 源树（内容与 id 都照搬）
     */
    void assignFrom(const NodeTree& other);

    /**
     * 一份独立的值拷贝，内容与 id 都与自己相同。
     * @return 一份独立的值拷贝（内容与 id 都相同）
     */
    NodeTree clone() const;

    /**
     * 回到"刚构造出来的空文档"：根保留（id 不变），其余节点连同池里的记录一起删掉，
     * 分配器回到 2。
     *
     * 与 eraseChild 的留池语义相反，因为它的用途只有序列化还原（1.10）——那里要按记录把
     * id 原样装回去，于是旧的 id 必须能被重新使用；留池会让 createDetachedWithId 撞上
     * 一个早就摘下的同 id 节点。
     */
    void reset();

    /**
     * 按指定的 id 建一个不挂在树上的节点，返回它的 id。
     *
     * 只有序列化还原走这条路：id 必须原样回来（id 落盘的全部意义就在这里），而 insertChild
     * 与 createDetached 都会另分配 id。id 无效、或池里已经有同 id 的节点时返回
     * kInvalidNodeId —— 不静默另分配一个，那会让还原出来的文档与文件不是同一份。
     * @param id 文件里记的 id
     * @param kind 节点种类
     * @return 建出来的 id；id 无效或已存在时为 kInvalidNodeId
     */
    NodeId createDetachedWithId(NodeId id, NodeKind kind);

    /**
     * 把分配器推进到给定水位（下一个要分配的 id 不小于 next）。序列化还原用它：
     * 文件里的水位可能比现存的最大 id 还大（保存前删过节点），不推进的话新分配的 id
     * 会与文件里引用过的 id 撞上。水位不比现在大时什么都不做。
     * @param next 下一个要分配的 id
     */
    void reserveIdWatermark(NodeId next);

private:

    NodeId allocateId() { return next_id_++; }

    /**
     * 全部节点（含已摘下的子树），下标与 id 无关。
     */
    std::vector<std::unique_ptr<Node>> pool_;

    /**
     * id 到节点的查表。
     */
    std::unordered_map<NodeId, Node*> index_;

    /**
     * 根节点（构造时建出来，之后不变）。
     */
    Node* root_ = nullptr;

    /**
     * 下一个要分配的 id。0 留给"无效"。
     */
    NodeId next_id_ = 1;
};

/**
 * 这个 id 指向的节点是否还挂在文档上（不是已被摘下留在池里的子树）。根没有父节点，也算挂着的。
 *
 * 它出现在类外，是因为判定要用 NodeTree 的查询；放在类里只会让"树管结构、这个只管一个判断"
 * 这件事更绕。
 * @param tree 文档树
 * @param id 节点号
 * @return true = 还挂在文档上（不是已摘下的子树）
 */
bool isAttached(const NodeTree& tree, NodeId id);

}  // namespace heditor
