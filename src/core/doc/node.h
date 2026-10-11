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
#include <unordered_map>
#include <vector>

#include "core/doc/char_format.h"
#include "core/doc/para_format.h"

namespace heditor {

/**
 * 节点身份。64 位、文档内唯一、不回收（删掉的 ID 不再复用，避免旧引用指向新节点），
 * 所以序列化时必须落盘。0 保留给"无效"。
 */
using NodeId = uint64_t;
inline constexpr NodeId kInvalidNodeId = 0;

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
 */
bool isContainer(NodeKind kind);

/**
 * 是否为文本节点：内部按码点编址，在流里按码点展开。
 */
bool isText(NodeKind kind);

/**
 * 是否为流内分隔元素。单独判定是因为它既不是容器也不是原子，
 * 但流位置换算必须给它一个位置 —— 段落序号正是数它的个数得来的。
 */
bool isParagraphMark(NodeKind kind);

/**
 * 种类名，与枚举逐项对应。用于诊断与报告文本（拿数字去对照枚举太费事），
 * 也让"新加一个结构种类"这件事集中在一处 —— switch 不写 default，
 * 少写一种时 -Wswitch 会报出来。
 */
const char* nodeKindName(NodeKind kind);

/**
 * 节点。字段公开：它是纯数据，封装留在树那一层（见 NodeTree）。
 * 给每个字段配一对访问器只会让"谁改了 parent"更难查。
 */
struct Node {
    NodeId id = kInvalidNodeId;
    NodeKind kind = NodeKind::TextRun;
    Node* parent = nullptr;
    std::vector<Node*> children;  // 仅容器非空
    std::vector<uint32_t> text;   // 仅文本节点非空；单位是码点

    /**
     * 隐藏（第七节）：作者设定的默认可见性，只在非编辑模式下生效。
     * 放在节点上而不是另建一张表，是因为流位置换算要看它 —— 非编辑模式下隐藏的控件
     * 整棵子树从流里摘除，位置空间随之变化，所以 FlowIndex 必须带可见性口径。
     */
    bool hidden = false;

    /**
     * 段落格式：只对 ParagraphMark 有意义（第三节：段落属性挂在结束标记上）。
     * 放在节点上而不是另建一张表，理由同 hidden —— 编译要按流顺序取它，
     * 而流顺序就是树的顺序。其余种类读它没有定义。
     */
    ParaFormat para;

    /**
     * 字符属性：只对 TextRun 有意义（第二节：标记是文本节点上的属性集合）。
     * 与 para 同一个理由：规范化与编译都要按流顺序取它。其余种类读它没有定义。
     */
    CharFormat marks;
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

    NodeId rootId() const { return root_->id; }

    /**
     * 节序列的第一节。0.1 只建一节，分节在 1.8。
     */
    NodeId firstSectionId() const { return section_->id; }

    /**
     * 找不到时返回 nullptr，不抛：拿 id 来查的多半是调用方自己算错了。
     */
    Node* find(NodeId id);
    const Node* find(NodeId id) const;

    /**
     * 在 parent 的第 at 个槽位插入一个 kind 类型的子节点，返回新 id。
     * at 取 [0, 子节点数]，等于子节点数就是追加。parent 不存在、kind 不是容器、
     * 或 at 越界时返回 kInvalidNodeId —— 不做隐式纠正。
     */
    NodeId insertChild(NodeId parent, int at, NodeKind kind);

    /**
     * 移除 parent 的第 at 个子节点（连同它的整棵子树），返回被摘下子树的根 id。
     * 摘下的节点仍留在池里、ID 不回收，只是不再挂在树上 —— 撤销要把它原样搬回去，
     * 而"搬回去"要求身份不变，所以这里不能销毁节点。at 越界或 parent 不存在时
     * 返回 kInvalidNodeId。
     */
    NodeId eraseChild(NodeId parent, int at);

    /**
     * 追加到末尾。等价于 insertChild(parent, 子节点数, kind)，只是省去调用方数一遍。
     */
    NodeId appendChild(NodeId parent, NodeKind kind);

    /**
     * 设置文本节点的码点。id 不存在或不是文本节点时返回 false。
     */
    bool setText(NodeId id, std::vector<uint32_t> cps);

    size_t nodeCount() const { return pool_.size(); }

    /**
     * 迄今分配过的最大 ID。序列化要用它续上分配器（1.10）。
     */
    NodeId maxAllocatedId() const { return next_id_ - 1; }

private:
    NodeId allocateId() { return next_id_++; }

    std::vector<std::unique_ptr<Node>> pool_;
    std::unordered_map<NodeId, Node*> index_;
    Node* root_ = nullptr;
    Node* section_ = nullptr;
    NodeId next_id_ = 1;  // 0 留给"无效"
};

}  // namespace heditor
