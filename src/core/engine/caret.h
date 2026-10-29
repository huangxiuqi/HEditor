/**
 * 光标与位置解析：把文档产物与位置互相换算，并持有当前光标与选区。
 *
 * 为什么单独成类：引擎对外说 (段, 段内码点偏移) 加侧别，模型层说流位置，排版层说行盒 ——
 * 三套坐标之间的换算原先散在引擎里，与编辑、绘制、控件挤在同一个类上。而这三套坐标已经定
 * 下来了（阶段 1 落地：流位置是模型与引擎的对接口，行盒的字段形状不再变），所以这一层可以
 * 独立出来：它只读文档产物，不碰编辑、不碰绘制、不碰帧与窗口。
 *
 * 边界（按这条划，别让它长回去）：
 *   进 —— 流位置与文本坐标的互换、侧别与归属提示、行盒查找、光标与选区矩形（页面磅）、
 *         命中测试的行内一半、光标移动、选区与光标状态；
 *   不进 —— 页内像素换算（那是呈现侧的事）、页像素步距（宿主摆页的约定）、
 *         重排触发（移动光标会换占位集合，那是引擎的事，见 EditorEngine 的转发层）。
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/doc/control.h"
#include "core/doc/flow.h"
#include "core/doc/flow_types.h"
#include "core/doc/node_id.h"
#include "core/doc/range.h"
#include "core/doc/text_pos.h"
#include "core/layout/layout.h"
#include "core/utils/types.h"

namespace heditor {
/**
 * 位置解析要读的那几样东西，全是只读。
 *
 * 为什么用视图而不是让解析器持引擎指针：解析器不该知道引擎还有编辑、绘制、审计那些事 ——
 * 一份只读输入把依赖面钉在编译期，多要一样就得改这个结构，看得见。引擎在每次重建文档与
 * 重排之后摆一次（见 EditorEngine::rebuildProjection 与 relayout）。
 */
class ControlIndex;

struct CellIndex;
struct StructureIndex;

/**
 * 光标/选区的位置：流位置与行别。
 *
 * affinity 只影响折行/页边处画在哪一行，不改变流位置本身。它是宿主与内部共用的
 * 位置口径；旧的 (段, 偏移, 侧别) 仍由它派生。
 */
struct Position {
    /**
     * 全局流位置。
     */
    FlowPos flow = 0;

    /**
     * 行别：0 = 默认，负数贴上一行，正数贴下一行。
     */
    int affinity = 0;
};

/**
 * 文字选区：一对 Position，anchor 是不动的那一端。
 */
struct TextSelection {
    /**
     * 不动的端点。
     */
    Position anchor;

    /**
     * 移动的端点。
     */
    Position focus;

    /**
     * 两端重合。
     * @returns true = 空选区
     */
    bool empty() const { return anchor.flow == focus.flow; }
};

struct PositionView {
    /**
     * 段文本与筛过的行内对象表。
     */
    const std::vector<BlockSpec>* blocks = nullptr;

    /**
     * 未按显示名单筛过的对象表。
     */
    const std::vector<BlockSpec>* blocks_full = nullptr;

    /**
     * 每块内容起点（全局流位置）。
     */
    const std::vector<FlowPos>* block_flow = nullptr;

    /**
     * 每块占几个流位置（流索引给的权威段长）。
     */
    const std::vector<int>* block_flow_span = nullptr;

    /**
     * 流索引（树 → 流位置）。没持树时它为空。
     *
     * 持的是 optional 的地址而不是它内容的地址：流索引是"重建时 emplace 进去"的，
     * 第一次重建之前它还没建，取内容的地址会拿到空指针并一直空下去。
     */
    const std::optional<doc::FlowIndex>* flow = nullptr;

    /**
     * 编译产物里的控件集合（区间树）。
     */
    const ControlSet* controls = nullptr;

    /**
     * 控件下标到节点号的映射。
     */
    const std::vector<NodeId>* control_node_ids = nullptr;

    /**
     * 控件区间索引；nullptr 时退回线性扫描。
     */
    const ControlIndex* control_index = nullptr;

    /**
     * 结构索引（含行内槽）；nullptr 时退回扫对象表。
     */
    const StructureIndex* structure = nullptr;

    /**
     * 节点号到控件下标的反查表。
     */
    const std::unordered_map<int, int>* owner_index = nullptr;

    /**
     * 行盒与字形的排版产物。
     */
    const LayoutResult* layout = nullptr;

    /**
     * 流索引建好了没有。
     * @return true = 视图里有流索引
     */
    bool hasFlow() const { return flow != nullptr && flow->has_value(); }

    /**
     * 流索引（hasFlow 为真时才可用）。
     * @return 流索引
     */
    const doc::FlowIndex& flowIndex() const { return **flow; }
};

/**
 * 整格选区的意图：一张表上的一块矩形，加上焦点格。
 *
 * 只有这一份是"用户选了什么"。块区间、流包络、渲染用的合并区间、插入点全由它派生
 * （见 CaretResolver::setCellRange）—— 六份状态各算一遍时，改一处忘另一处就出现
 * "反白与删除范围不一致"这类错。
 */
struct CellRange {
    /**
     * 表的身份（模型节点 id，见 CellSlot::table）。
     */
    uint64_t table = 0;

    /**
     * 行范围起点（含）。
     */
    int r0 = 0;

    /**
     * 行范围终点（含）。
     */
    int r1 = -1;

    /**
     * 列范围起点（含）。
     */
    int c0 = 0;

    /**
     * 列范围终点（含）。
     */
    int c1 = -1;

    /**
     * 焦点格的行号。
     */
    int focus_row = 0;

    /**
     * 焦点格的逻辑格号。插入点钉在这一格的末尾。
     */
    int focus_cell = 0;

    /**
     * 有没有矩形（端点没设过就是空）。
     * @return true = 没有可展开的范围
     */
    bool empty() const { return r1 < r0 || c1 < c0; }
};

/**
 * 光标与选区的位置解析器。
 *
 * 状态只有四样：选区两端、行别、上下键的列位、光标条宽度。其余全是"读视图算出来"的 ——
 * 所以引擎重编译或重排之后只要重新摆一次视图，解析器不需要任何失效通知。
 */
class CaretResolver {
public:

    /**
     * 摆一次视图（重建文档或重排之后）。视图里存的是各容器的地址，容器不换对象就一直有效。
     *
     * @param v 只读输入（块流、流索引、行盒与控件集合的地址）
     */
    void setView(const PositionView& v) { view_ = v; }

    /**
     * 光标条宽度（磅）。引擎在 setCaretAndSelectionStyle 里把它转进来。
     *
     * @param w 光标条宽度（磅）
     */
    void setCaretWidthPt(float w) { caret_width_pt_ = w; }

    /**
     * 控件下标 ↔ 产生端句柄（持树时是控件节点 id）的翻译。
     *
     * 占位簇上记的是句柄，而光标侧别、命中测试与宿主报的都是下标 —— 差这一道翻译，
     * 共享边界上就是"括号排对了、光标却画到另一侧"。两个方向都在这里，读的是视图里那两张表。
     *
     * @param owner 产生端句柄（持树时是控件节点号）
     * @return 控件下标；找不到为 -1
     */
    int controlIndexOfOwner(int owner) const;

    /**
     * 控件下标转产生端句柄。
     * @param control_index 控件下标
     * @return 句柄（节点号）
     */
    int slotOwnerOf(int control_index) const;

    /**
     * 某偏移上的行内对象统计。侧别与流位置都从它推出来（两者是同一件事的两种说法）。
     *
     * 分桶按"开括号 / 非开括号"而不是"开 / 闭"：行内原子既不是开也不是闭，但它同样占一格，
     * 而且默认侧别取"在所有控件之外、在所有原子之后"最说得通 —— 程序化定位到某个偏移时
     * 落在那一处内容的末尾，而不是夹进图片前面。于是 closes + opens 恒等于这一处压着的
     * 对象总枚数，侧别的上界与"跨过这一处所有对象"的偏移都用它，不必再单独记一个总数。
     */
    struct BracketCounts {
        /**
         * 非开括号的对象枚数（闭括号加原子）= "所有控件之外"那一侧的侧别。
         */
        int closes = 0;

        /**
         * 开括号的对象枚数。
         */
        int opens = 0;

        /**
         * 排在这个偏移前面的对象枚数（对象表按 cp 升序，也就是它的下标）。
         */
        int before = 0;
    };

    /**
     * 选区与光标：唯一真源。caret() 是它的投影（整格选区时投影到焦点那一格的末尾），
     * 两者不是两套状态。
     *
     * 整格时返回的是吸附到格边界的那一对（见 setCellRange），而不是用户按下/松开的那两个
     * 字符位置 —— 渲染与编辑都用这一个读数，于是"看到反白的格"与"删掉的格"不可能分叉。
     * 要用户那两个原值（往回拖回原格时要靠它恢复文字选区），用 rawSelection。
     * @return 选区（整格时是吸附到格边界的那一对）
     */
    Range selection() const {
        if (sel_cell_ && cell_lo_ >= 0 && cell_hi_ > cell_lo_) {
            // 方向跟着用户焦点走：向前拖的焦点在后，返回值也保持"anchor 在前"；
            //   向后拖则翻过来。begin / end 因此不变，而"删除之后光标落在哪一端"
            //   这类读 focus 的地方看到的仍是用户拖动的那一头。
            return sel_.focus.flow >= sel_.anchor.flow ? Range{cell_lo_, cell_hi_}
                                                       : Range{cell_hi_, cell_lo_};
        }
        return Range{sel_.anchor.flow, sel_.focus.flow};
    }
    /**
     * 一格的身份：它占的块区间，段号连续、半开。
     *
     * 为什么格的身份不能是流区间：空格的内容长度是 0，流区间退化成一个点，而"点"没有身份 ——
     * 空段不占流位，下一个块也起在同一个位置，于是"这一格"与"下一格的第一段"在流坐标上
     * 是同一个数。块区间是格自己的身份证：每格至少一个块，块区间两两不重叠。
     */
    struct CellBlocks {
        /**
         * 这一格占的块的起始下标。
         */
        int first = -1;

        /**
         * 这一格占几个块（空格也有，恒为正）。
         */
        int count = 0;

        /**
         * 这一格有没有内容。
         * @return true = count 不大于 0
         */
        bool empty() const { return count <= 0; }

        /**
         * 某个块在不在这格里。
         * @param para 块下标
         * @return true = 在这格的块区间里
         */
        bool holds(int para) const { return count > 0 && para >= first && para < first + count; }
    };

    /**
     * 用户按下 / 松开的那两个原值（没有吸附到格边界）。
     * 往回拖回原格时要靠它恢复文字选区 —— 对外露的 selection() 是吸附之后的那一对。
     *
     * @return 原样的选区
     */
    Range rawSelection() const { return Range{sel_.anchor.flow, sel_.focus.flow}; }

    /**
     * 有没有文字被选中。整格但内容为空的格不算（那一条问的是 selectionIsCell）。
     *
     * @return true = 两端不重合
     */
    bool hasSelection() const { return !sel_.empty(); }

    /**
     * 整格选区覆盖的每一格（按块号升序，一格一份块区间）。
     *
     * 为什么不是"首格首到末格末"那一条：跨行拖选要的是矩形那一块，而矩形在流里不是区间 ——
     * 从第二行第三列拖到第三行第五列，选区的流区间会顺带把第三行前四列包进来（文档顺序是
     * 逐行的，一跨行就整行）。所以这里记的是"选中了哪几格"，流包络只是它们的 min/max。
     * 空表示没有逐格信息（全选那一路），渲染与删除都退回按包络算。
     *
     * @return 选中的每一格（块区间，按块号升序）
     */
    const std::vector<CellBlocks>& cellBlocks() const { return cell_blocks_; }

    /**
     * 选区是"单元格选区"还是"文字选区"。
     *
     * 这两套不是同一件事的粗细两档，而是两条独立的操作线：文字选区只盖文字（拖选、Shift
     * 扩展、程序化设区间都会落在这里），单元格选区整格反白（含内边距与显式行高多出来的
     * 空白），只能由"整格"这个动作产生（点格内左侧的边距带、三击）。全选也按整格铺满 ——
     * 与 Word 同一条（Ctrl+A 之后表格是整片反白）。
     * 靠几何去猜（"选区正好覆盖了整段"⇒ 一定是整格）是不行的：把文字一路选到最后一个字
     * 与选整格的位置一模一样，猜出来的结果就是"选到行尾就跳成整格"。
     *
     * 空格也成立：内容是空的（sel_ 一对相等的端点），但粒度确实是整格 —— 反白要铺满它。
     * 与 hasSelection 分工：那一个问"有没有文字被选中"，这一个问"选的是什么粒度"。
     * @return true = 选的是整格
     */
    bool selectionIsCell() const { return sel_cell_ && (!sel_.empty() || !cell_blocks_.empty()); }

    /**
     * 选区是否吸附到了格边界上（跨格拖选或单格选格）。
     *
     * 与 selectionIsCell 的差别在"全选"：那一个为真，但端点没有吸附（全篇不是一格）。
     * 删除要按"逐格清空"走的时候判这一个 —— 全选仍然按整篇删除。
     * @return true = 端点已经吸附到格边界
     */
    bool hasCellRange() const { return sel_cell_ && !cell_blocks_.empty(); }

    /**
     * 行别：折行处同一个位置属于两行（上一行行尾 / 下一行行首），只有点击落点会带非零值。
     * @return 行别
     */
    int lineAffinity() const { return sel_.focus.affinity; }

    /**
     * 把光标摆到 (段, 段内码点偏移, 侧别)。侧别小于零 = 没有侧别信息，按"最内层含端点"
     * 投影一次。只摆位置，不触发重排 —— 重排由引擎的转发层协调（见 projectCaret）。
     * @param para 段号
     * @param offset 段内码点偏移
     * @param side 侧别（小于 0 = 没有信息，按最内层投影）
     * @param extend true = 扩展选区
     * @param line_aff 行别
     */
    void placeCaret(int para, size_t offset, int side, bool extend = false, int line_aff = -1);

    /**
     * 整篇的选区矩形（页面坐标，磅）。判据与审计用它，呈现不要用 —— 用 selectionRectsIn。
     *
     * @return 整篇的选区矩形
     */
    std::vector<PageRect> selectionRects() const;

    /**
     * 限定页范围的选区矩形（页面坐标，磅）。整篇选中的矩形条数是"文档行数"这个量级，
     * 而呈现每拍都要过一遍 —— 限页之后这一笔只随看得见的行数走。
     *
     * @param first_page 窗口首页
     * @param count 窗口页数（<= 0 表示不限页）
     * @return 落在窗口内的那一部分
     */
    std::vector<PageRect> selectionRectsIn(int first_page, int count) const;

    /**
     * 重排之后重新投影一次光标：输入（段, 偏移, 侧别）不变，但排版换了簇表，簇吸附要按
     * 新表再来一遍。收敛 —— 第二次不会再触发重排。
     * @param extend true = 扩展选区
     */
    void reprojectCaret(bool extend);

    /**
     * 重编译之后把选区两端按段内位置落回新基。
     *
     * 选区两端存的是全局流位置，而基（块起点表）换了之后同一个数字含义就变了 —— 前面某
     * 一段变长之后它会静默漂到相邻位置。两端都按"段内位置"重算；只落 focus 会把 anchor
     * 收成空选区（症状是"Shift 扩展之后来一次重编译，选区就没了"）。
     * @param anchor_para anchor 的段号
     * @param anchor_rel anchor 的段内流位置
     * @param focus_para focus 的段号
     * @param focus_rel focus 的段内流位置
     */
    void rebaseSelection(int anchor_para, int anchor_rel, int focus_para, int focus_rel);

    /**
     * 这一段的行内对象表（按码点升序）。侧别、流位置与括号统计都从它推出来。
     *
     * @param para 段号（块下标）
     * @return 对象表（没有对象时为空）
     */
    const std::vector<InlineObject>& objectsFor(int para) const;

    /**
     * 某偏移上压着的行内对象统计。
     * 侧别与流位置都从它推出来 —— 两者是同一件事的两种说法：
     * 流位置 = 码点偏移 + 排在它前面的边界数 + 侧别。
     *
     * @param para 段号（块下标）
     * @param cp 段内码点下标
     * @return 开 / 非开两桶与"排在它前面"的枚数
     */
    BracketCounts bracketCountsAt(int para, size_t cp) const;

    /**
     * 段内相对流位置：码点偏移加上压在这一处的对象枚数，再加侧别。
     *
     * @param para 段号（块下标）
     * @param cp 段内码点下标
     * @param side 侧别（这一处跨过了几枚括号；小于 0 = 没有信息）
     * @return 段内相对流位置
     */
    int relAt(int para, size_t cp, int side) const;

    /**
     * 文本码点区间 [a, b) 在流里占的那一段（段内相对，半开）。
     *
     * 括号不是文本：它们只占位置、不进这个区间。所以起点是"a 那个字自己的位置"（＝跨过这个
     * 偏移上所有括号之后），终点是"b 那个字自己的位置"。两端都不猜 —— 用"最内层含端点"猜
     * 侧别的话，删除区间会多含一枚括号：删"天"把 ]中 也圈进去，删完光标就落到括号的另一侧。
     *
     * 判定（policyErase）与实际删除（eraseOnTree）共用它：两处一旦分叉，就会出现
     * "判据说能删、真删却越界"这种只在边界上冒出来的错。
     *
     * @param para 段号（块下标）
     * @param a 区间起点（含）
     * @param b 区间终点（不含）
     * @param from 写回流起点（段内相对）
     * @param to 写回流终点（不含，段内相对）
     * @return true = 这个区间落在本段里
     */
    bool textSpanAt(int para, size_t a, size_t b, int* from, int* to) const;

    /**
     * 不带侧别时的默认侧别。
     *
     * 规则是"最内层含端点" —— 与编辑归属同一条：区间树里包含这个
     * 偏移的最深那个控件，同深度取下标小者。于是"程序化定位到字段末尾"落在 ] 之内、
     * "定位到字段开头"落在 [ 之后，相邻空字段则进到它自己那一对括号之间。
     *
     * 为什么不由括号排布直接算：同一个偏移上有几个位置，光看"有几枚括号"分不出该停哪个 ——
     * 归属那套（区间包含 + 嵌套深度 + 下标次序）才是既有判据钉住的答案，两条口径必须同源。
     * 控件集合为空时没有可归的地方，停在所有括号之前。
     * @param para 段号
     * @param cp 段内码点下标
     * @return 默认侧别
     */
    int defaultSideAt(int para, size_t cp) const;

    /**
     * 归属提示 → 侧别：区间树里最内层含这个点的那一枚括号，落在它之前还是之后。
     * 宿主只认控件下标，而引擎内部一律说侧别 —— 这是两者之间的那道翻译。
     *
     * @param para 段号（块下标）
     * @param cp 段内码点下标
     * @param hint 归属提示（控件下标）
     * @return 侧别
     */
    int sideFromHint(int para, size_t cp, int hint) const;

    /**
     * 文本坐标 → 全局流位置：侧别由位置自己归一次。
     *
     * 同一个码点偏移上可能压着好几枚括号，只凭偏移定不出插入点，所以这里先按
     * "最内层含端点"归一次侧别。光标那一处用光标自己存的侧别 —— 它可能来自一次点击，
     * 比默认归属更准；其余位置（删除区间的两个端点、诊断打印）按默认归属。
     * 编辑路径只认这一个入口，不再各自拼 (段, 偏移, 侧别) 三件套。
     * @param p 文本坐标
     * @return 全局流位置；段号越界时为 -1
     */
    FlowPos flowAt(TextPos p) const;

    /**
     * 段内位置 + 侧别 → 全局流位置。侧别已定时这是纯加法（块起点 + 段内相对流位置）。
     *
     * @param para 段号（块下标）
     * @param cp_offset 段内码点偏移
     * @param side 侧别（这一处跨过了几枚括号；小于 0 = 没有信息）
     * @return 全局流位置
     */
    FlowPos flowPosAtSide(int para, size_t cp_offset, int side) const;

    /**
     * 带归属提示的换算：侧别由提示投影一次，再走 flowPosAtSide。
     * 只给"说话方式是控件下标"的入口用 —— 编辑路径一律自己算侧别。
     *
     * @param para 段号（块下标）
     * @param cp_offset 段内码点偏移
     * @param owner_hint 归属提示（控件下标；-1 = 没有）
     * @return 全局流位置
     */
    FlowPos flowPosAt(int para, size_t cp_offset, int owner_hint) const;

    /**
     * 段内相对流位置 → 段内码点偏移：减掉这一处对象占的位置。
     *
     * @param para 段号（块下标）
     * @param rel 段内相对流位置
     * @return 段内码点偏移
     */
    int cpOffsetInBlock(int para, int rel) const;

    /**
     * 全局流位置 → 文本坐标。落在段间的位置归到相邻段的端点上（模型里没有"段间"这个位置）。
     *
     * @param flow 全局流位置
     * @param out 写回值
     * @return true = 翻得出来（不持树时为 false）
     */
    bool flowToTextPos(FlowPos flow, TextPos* out) const;

    /**
     * 段内相对流位置 → 全局流位置（夹到本段跨度内）。
     *
     * 夹取只做范围收缩，不做簇吸附（簇表要排版之后才有，见 relayout 里的说明）。
     * 上界取 view_.block_flow_span（流索引给的权威段长）而不是显示名单筛过的那张对象表的长度：
     * 前者是树的口径，后者随聚焦档变化，拿它当上界会在换档时把光标往回拉。
     *
     * @param para 段号（块下标）
     * @param rel 段内相对流位置
     * @return 全局流位置
     */
    FlowPos flowForRel(int para, int rel) const;

    /**
     * 任意流位置落在哪一块（派生）。view_.block_flow 单调，二分取最后一个起点不超过它的块。
     * caretPara 是它在 focus 上的特例 —— 选区另一端的段号也走这里。
     * @param f 流位置
     * @return 块下标
     */
    int paraForFlow(FlowPos f) const;

    /**
     * 任意流位置的段内流位置（派生）= 它减本段起点。
     * @param f 流位置
     * @return 段内相对流位置
     */
    int relForFlow(FlowPos f) const;

    /**
     * 任意流位置的侧别（派生）：段内流位置减去"这个偏移在流里占的起点"就是跨过的边界枚数。
     * 没有"存着的侧别"可以失配，所以既不需要失效标记，也不需要拿边界枚数去卡它。
     * @param f 流位置
     * @return 侧别
     */
    int sideForFlow(FlowPos f) const;

    /**
     * 这一段的簇表与当前文本对不对得上。
     *
     * 排版之后文档又改过（命令改了树但还没重排）时表就是旧的：此时按它吸附会让落点
     * 落到上一个字。判据是表尾的码点数与当前段落长度是否相等。
     * @param para 段号
     * @param starts 簇起点表
     * @return true = 表与当前文本对得上
     */
    bool clusterTableFresh(int para, const std::vector<uint32_t>& starts) const;

    /**
     * 吸附到字素簇边界：不大于 offset 的最近簇起点。偏移落在一个簇中间时退回它的开头。
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @return 簇起点
     */
    size_t snapToCluster(int para, size_t offset) const;

    /**
     * 下一个簇边界：大于 offset 的最小簇起点；没有（或这一段没有簇表）就返回 offset。
     *
     * 与 snapToCluster 配成一对（那个是"<= offset 的最大簇起点"）。前删要用它找"下一个可删
     * 边界"—— 删一个簇而不是删一个码点，否则 e + U+0301 会被拆开，留下一个孤立的组合重音。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @return 下一个簇起点；没有则为 offset
     */
    size_t clusterCeilAt(int para, size_t offset) const;

    /**
     * 光标侧别（派生）= focus 的侧别，与 caret() 同源，不可能不同步。
     * @return 光标侧别
     */
    int caretSide() const;

    /**
     * 光标那一处的归属（控件下标）。按光标的流位置查控件内容区间（ControlSpans），不是
     *   独立状态；不在任何控件里返回外侧哨兵，控件集合为空返回 -1。
     *
     * @return 控件下标；-1 = 没有控件；LineBox::kInlineOffsetOuter = 在所有控件之外
     */
    int caretFieldHint() const;

    /**
     * 唯一写入点：段内流位置。其余入口都只是把它换算成全局流位置再调这里。
     *
     * @param para 段号（块下标）
     * @param rel 段内相对流位置
     * @param collapse true = 两端收到一起（否则保留锚点）
     * @param line_aff 行别：0 = 贴上一行，大于 0 = 贴下一行，小于 0 = 按流位置自动判
     *   （"贴着左边那一格所在的行"，见 lineAffinityAtFlow）。点击落点显式给 0/1，
     *   其余入口走自动 —— 折行处只按偏移投影会把"闭括号之后"画到上一行去。
     */
    void setCaretRel(int para, int rel, bool collapse = true, int line_aff = -1);

    /**
     * 折行处的行别：这个流位置该贴哪一行（0 = 上一行行尾，1 = 这一行行首）。
     *
     * 折行处同一个文字偏移压着两行，光看偏移分不出"上一行行尾"与"下一行行首"。
     *   行别不能只看"点在哪一行"：方向键跨过折行时没有点击，得由流位置自己说 ——
     *   光标是"贴着左边那一格"，左边那一格排在哪一行，光标就在那一行。
     *   症状举例：控件跨行、闭括号落在下一行行首时，闭括号之后那个位置的偏移与
     *   "内容末尾"相同，按贴上一行画就跳到上一行去了。
     *
     * @param f 流位置
     * @return 0 或 1
     */
    int lineAffinityAtFlow(FlowPos f) const;

    /**
     * 光标那一段（派生）：focus 落在哪一块。
     *
     * 不能借 flowToTextPos：那个入口的职责是"把流位置翻成文本坐标"，段间的位置会被归到相邻段
     * 端点；这里要的是"focus 归属哪一块"，段尾那个位置（段落标记所在位）必须仍算本段。
     * @return 光标所在块的下标
     */
    int caretPara() const;

    /**
     * 段内流位置（派生）= focus 减本段起点。
     * @return 光标的段内相对流位置
     */
    int caretRel() const;

    /**
     * 光标位置（派生）：段号 + 段内码点偏移，由光标的流位置投影出来 —— 不是独立状态。
     * 光标的流位置通常就是 focus，整格选区例外：它是焦点那一格的末尾（见 cell_caret_）。
     * @return 光标位置（段号 + 段内码点偏移）
     */
    TextPos caret() const;

    /**
     * 按对外坐标（段, 段内码点偏移）写光标，侧别按"最内层含端点"投影一次。
     * 这是文本坐标投影到流位置的唯一约定，只给"没有侧别信息"的入口用（宿主程序化定位、
     * 排版收口）；编辑路径一律走 setCaretAtFlow / setCaretRel —— 侧别由操作自己算出来。
     * @param p 文本坐标
     */
    void setCaretByText(TextPos p);

    /**
     * 把光标摆到一个流位置上（绝对）。位置与侧别一次算准 —— 不猜、不查区间。
     *
     * 编辑路径全用它：删除后的落点是删除区间的起点、插入后是插入区间的终点、Tab 的目标、
     * 撤销恢复的位置，它们都已经是流位置。只留下一个码点偏移、再由"最内层含端点"猜回侧别的话，
     * 猜错的表象就是"删一个字光标跳一格"：删完"天"，落点会被猜成 ]中 之前，
     * 而它该在 ]中 之后（＝被删那个字所在的位置）。
     * @param flow 目标流位置
     * @return true = 摆好了
     */
    bool setCaretAtFlow(FlowPos flow);

    /**
     * 程序化设选区（宿主恢复、探针摆位）。两端各自夹到文档流的合法范围内 —— 越界的那
     * 一端收到能到的最远处，不会让一个悬空端点把后面的几何全带偏。
     * @param r 选区
     * @return true = 摆好了
     */
    bool setSelection(Range r);

    /**
     * 只搬锚端，焦点端与行别都不动。
     *
     * 拖选从控件内部起手时要它：按下那一刻已经把焦点端夹到块外了，锚端得放回按下的位置。
     * 不能借 setSelection —— 那会把行别清成 0，折行处的焦点端会画到上一行行尾。
     * @param anchor 新的锚端流位置（越界夹到文档流两端）
     * @return true = 搬好了
     */
    bool setAnchorAtFlow(FlowPos anchor);

    /**
     * 整格选区：把 range 这块矩形记成"单元格选区"，其余状态都由它派生。
     *
     * 意图只存这一份（表、行列范围、焦点格）。块区间、流包络、渲染用的合并区间、插入点
     * 都由 setCellRange 一次产出 —— 分散算的话，改一处忘另一处，反白就会与删除范围分家。
     * 展开按格的合并跨度判相交：跨进矩形的合并格算选到它。
     *
     * @param range 选中的矩形与焦点格
     * @param cells 块↔格索引（块区间由它展开，不再现场扫表格）
     * @return true = 记下了；矩形里不足两格（表格结构刚变过）时为假，且状态未改
     */
    bool setCellRange(const CellRange& range, const CellIndex& cells);

    /**
     * 取消整格吸附：退回文字选区（选区本身不动）。
     */
    void clearCellRange();

    /**
     * 取消选区：anchor 收到 focus，焦点端不动 —— 光标留在原地。
     */
    void clearSelection();

    /**
     * 全选：文档首到文档末。
     *
     * 末端是"最后一块的段内末端"（块起点 + 本块占的位数），而不是下一块之前那个段落标记
     * 所在的位置 —— 段落标记不属于任何一段的内容，够不到它。
     * @return true = 选好了
     */
    bool selectAll();

    /**
     * 找到包含该位置的行盒。
     *
     * 折行处有两个候选：上一行的末尾与下一行的开头是同一个位置（偏移与侧别都一样），
     * aff 就是选哪一个 —— 0 取先遇到的那一枚（上一行），大于 0 取最后一枚（下一行）。
     * 不选的话点下一行行首会画到上一行行尾，连按两下 Down 也会"原地不动"（当前行取成
     * 上一行，Down 落到下一行，位置与光标一模一样）。
     * @param pos 文本坐标
     * @param aff 行别（0 = 取上一行，大于 0 取下一行）
     * @return 行盒；找不到为 nullptr
     */
    const LineBox* findLineFor(const TextPos& pos, int aff = 0) const;

    /**
     * 某个位置的页号与页面矩形（磅）。光标那一处用它；任何位置都能问，与 caretRectPt 差一个入参。
     *
     * @param pos 文本坐标
     * @param page_out 写回页号
     * @param out 写回矩形（页面坐标，磅）
     * @return true = 找到了行盒
     */
    bool caretRectFor(const TextPos& pos, int* page_out, Rect* out) const;

    /**
     * 任意 Position 的页面矩形（磅）。行别取 Position.affinity，侧别由流位置派生。
     * @param pos 位置
     * @param page_out 写回页号
     * @param out 写回矩形
     * @returns true = 找到了行盒
     */
    bool caretRectFor(const Position& pos, int* page_out, Rect* out) const;

    /**
     * 当前光标的页面矩形（磅）。光标不可见或找不到行盒时返回假。
     *
     * @param out 写回值
     * @return true = 找到了行盒
     */
    bool caretRectPt(Rect* out) const;

    /**
     * 光标所在页（绝对页号）。宿主拿它决定窗口锚在哪、输入法框摆在哪。
     *
     * @return 页号；-1 = 找不到行盒
     */
    int caretPage() const;

    /**
     * 光标是否落在竖排的段里。宿主据此把方向键按物理方向映射 —— 竖排的行内轴是竖直的，
     * 上下走字、左右换列（见 keyToOp）。
     *
     * @return true = 这一段的行内轴是竖直的
     */
    bool caretVertical() const;

    /**
     * 光标所在段的列序：中日竖排列自右向左（反向），蒙文竖排列自左向右。
     * 取自段自己的书写模式，不从行盒推 —— 行盒上的 sideways 只是"字形要不要转"，
     * 与列序是两件事，拿它代用会在将来加"侧倒但列序反向"的组合时静默出错。
     * @return true = 列序反向（第一列在最右）
     */
    bool caretBlockReversed() const;

    /**
     * 在某一行里按"沿行内轴的页面坐标"（横排 x / 竖排 y）命中插入点。
     *
     * 每个插入点的势力范围是它到相邻插入点的中点；落在某个簇的后半段时，落点是下一个簇的
     * 首码点 —— 这就是"字素不可切分"的正确语义（点 e+U+0301 的右半边，光标跑到它后面，
     * 而不是钻进它内部）。hitTestPt 与上下键共用它，免得两份实现分头长出不同的势力范围。
     * @param lb 行盒
     * @param inline_page_pt 沿行内轴的页面坐标
     * @param cp_out 写回码点下标
     * @param side_out 写回侧别
     */
    void hitTestInLine(const LineBox& lb, float inline_page_pt, uint32_t* cp_out,
                       int* side_out) const;

    /**
     * 光标沿行内轴的页面坐标 —— 上下键的列位就是它。找不到行盒返回 0。
     * @return 光标沿行内轴的页面坐标
     */
    float caretInlineAxisPt() const;

    /**
     * 按簇走 dcp 步（正 = 向后）。到文档两端时停住。
     *
     * @param dcp 走几个字（负 = 向前）
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretBy(int dcp, bool extend);

    /**
     * 到所在行的行首（不是段首）：Home 的语义。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToLineStart(bool extend);

    /**
     * 到所在行的行尾（不是段尾）：End 的语义。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToLineEnd(bool extend);

    /**
     * 到文档首 / 文档末（Ctrl+Home / Ctrl+End）。
     *
     * 落点用流位置给（块起点 / 块起点 + 本块占的位数），不走文本坐标 —— 段末那个位置在
     * 文本坐标里与"下一段段首"是同一个偏移，翻回来会跑到下一段去。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToDocStart(bool extend);

    /**
     * 到文档末。落点用流位置给（最后一块的段内末端），不走文本坐标 ——
     * 段末那个位置在文本坐标里与下一段段首是同一个偏移。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToDocEnd(bool extend);

    /**
     * 上下键：沿块轴走到相邻行，列位沿行内轴保持。
     *
     * 实现是"当前行盒在全局行数组里的下标 ±1 → 在目标行上按列位做一次行内命中"，不新增
     * 排版接口 —— 行盒数组本来就是按块轴排好的。到文档首/末时停在原地（返回假），但列位
     * 留着，于是"按到头上、再按回来"落的是同一个列位。
     *
     * @param dir 方向（负 = 向上 / 向左）
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretVertical(int dir, bool extend);

    /**
     * 光标位置（流位置 + 行别）。
     * @returns 光标位置
     */
    Position position() const;

    /**
     * 按 Position 摆光标。
     *
     * 不要先拆成 (段, cp, 侧别) 再拼回来：那样中间要经过一次"文本投影"，而投影对
     * 段间那些位置（段落标记、格边界）没有对应物。流位置进、流位置落，一步到底。
     *
     * @param p 位置；affinity 传 -1 表示按位置自己推
     * @param collapse true = 顺带清掉选区（定位）；false = 只搬焦点端（扩展）
     * @returns true = 摆位成功
     */
    bool setPosition(const Position& p, bool collapse = true);

    /**
     * 选区两端的位置。
     * @returns 选区
     */
    TextSelection selectionPositions() const;

    /**
     * 按一对 Position 摆选区。
     * @param s 选区
     * @returns true = 摆位成功
     */
    bool setSelectionPositions(const TextSelection& s);

private:

    /**
     * 光标的流位置（见 cell_caret_）：整格选区时是焦点那一格的末尾，否则就是 focus。
     * caretPara / caretRel / caretSide 三处派生都从这里取，光标位置因此只有一个真源。
     */
    FlowPos caretFlow() const;

    /**
     * 已解析出文本位置、行别与侧别之后的光标矩形。
     * @param pos 文本位置
     * @param line_aff 行别
     * @param side 侧别
     * @param page_out 写回页号
     * @param out 写回矩形
     * @returns true = 找到了行盒
     */
    bool caretRectResolved(const TextPos& pos, int line_aff, int side, int* page_out,
                           Rect* out) const;

    /**
     * 只读输入（见 PositionView）。每次重建文档与重排之后由引擎摆一次。
     */
    PositionView view_{};

    /**
     * 选区两端（唯一真源）。方向有意义：anchor 是不动的那一端。
     */
    TextSelection sel_{};

    /**
     * 选区的粒度（见 selectionIsCell）。它只活在一次"整格"动作之后，直到下一次定位或扩展。
     *
     * cell_lo_ / cell_hi_ 是整格时的流包络（渲染与编辑的端点摘要）；-1 表示没有吸附
     * （文字选区或全选）。两者与 sel_cell_ 必须同时成立：sel_cell_ 为真而端点是 -1 是
     * "全选"那一支，走的仍是 sel_ 自己（全选本来就覆盖整篇，不需要吸附）。
     */
    bool sel_cell_ = false;

    /**
     * 整格选区的流包络起点；-1 = 没有吸附。
     */
    FlowPos cell_lo_ = -1;

    /**
     * 整格选区的流包络终点（不含）；-1 = 没有吸附。
     */
    FlowPos cell_hi_ = -1;

    /**
     * 选中了哪几格（见 CellBlocks）。
     */
    std::vector<CellBlocks> cell_blocks_;

    /**
     * 上面那几份块区间合并之后的结果（升序、两两不相交、相邻的并成一份）。
     *
     * 渲染每过一行都要问一次"这段在一格被选中的格子里吗"，而块区间可以嵌套（外层格
     * 包着内层表）—— 直接对 cell_blocks_ 二分会漏掉"外层格盖住、内层格没选"那种位置。
     * 合并成不相交之后，一次二分就能答。
     */
    std::vector<Range> cell_mask_;

    /**
     * 某个块是否落在某个被选中的格里（渲染按行盒筛用）。没有逐格信息时恒真 ——
     * 全选那一路按包络铺，没有逐格信息可筛。
     *
     * @param para 段号（块下标）
     * @return true = 这一块在被选中的格子里（没有逐格信息时恒真）
     */
    bool cellCoversPara(int para) const;

    /**
     * 整格选区的插入点（全局流位置），-1 = 没有（插入点就是 focus）。
     *
     * 为什么与 sel_.focus.flow 分开：拖选时 focus 是鼠标落下那一格里的任意位置，每动一下
     * 就变一次；而整格选区的插入点应当是"焦点那一格的末尾"，格内移动不该让它逐字漂。
     * 读 focus 的那几处（渲染、编辑、输入法定位）因此统一走 caretFlow()。
     */
    FlowPos cell_caret_ = -1;

    /**
     * 上下键的列位有没有取过（见 moveCaretVertical）。
     */
    bool vertical_goal_valid_ = false;

    /**
     * 上下键的列位（沿行内轴的页面坐标，磅）。
     */
    float vertical_goal_pt_ = 0.0f;

    /**
     * 光标条宽度（磅）。与 EngineConfig::caret_width_pt 同一档，引擎每次摆视图时覆盖它。
     */
    float caret_width_pt_ = 0.5f;
};

}  // namespace heditor
