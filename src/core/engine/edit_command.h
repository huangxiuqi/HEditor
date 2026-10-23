/**
 * 命令层：一次编辑怎么变成一条事务。
 *
 * 为什么单独成类：编辑入口原先散在引擎上（插入、退格、前删、分段、合并、缩进、删控件、
 * 改控件属性），每个入口都自己算步骤、自己拼事务、自己摆光标 —— 加一种编辑就要再抄一遍
 * 这套。而"一次编辑"的输入已经定下来了（阶段 1 落地：流位置是模型与引擎的对接口，编译
 * 产物是位置解析的唯一输入），所以这一层可以独立出来。风格切换、插图片、插表格将来都只是
 * 这里多一个命令，不必再碰引擎。
 *
 * 边界（按这条划，别让它长回去）：
 *   进 —— 落点判定（策略）、步骤构造、事务与撤销快照、光标落点、控件边界上的四条删除规则；
 *   不进 —— 编译产物的重建（命令只把事务交出去，见 EditHost）、排版、绘制、帧与窗口、
 *         控件查询与光标移动。
 *
 * 依赖方向：只认 EditView 与 EditHost 两个接口，不持引擎指针 —— 多要一样就得改这两个
 * 结构，依赖面在编译期看得见（与 CaretResolver 的 PositionView 同一套办法）。
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/doc/char_format.h"
#include "core/doc/compile.h"
#include "core/doc/control.h"
#include "core/doc/document.h"
#include "core/doc/edit_mode.h"
#include "core/doc/flow.h"
#include "core/doc/node.h"
#include "core/doc/range.h"
#include "core/doc/steps.h"
#include "core/engine/caret.h"
#include "core/layout/layout.h"
#include "core/rev/capability.h"
#include "core/rev/flow.h"
#include "core/rev/rev.h"

namespace heditor {
/**
 * 命令层交步骤的出口。引擎实现它：把整批步骤落到修订上，再把产物按新修订重算出来。
 *
 * 为什么是接口而不是回调：引擎就是唯一实现，而编辑是每次按键都要走的一步，虚调用比
 * std::function 少一层间接，也不必每次重排都重建一次闭包。
 */
class EditHost {
public:
    virtual ~EditHost() = default;

    /**
     * 把一批步骤交出去。返回 false 表示这一步没做成（步骤没落地、产物没重建），
     * 命令层据此中止整条编辑 —— 与"用户按的那一下没生效"同一口径。
     * @param steps 这一批步骤
     * @param dirty_para 起始脏段（-1 = 整篇重排）
     * @param selection_before 编辑前的选区
     * @param selection_after 编辑后的选区
     * @return false = 这一步没做成（步骤没落地、产物没重建）
     */
    virtual bool commitSteps(std::vector<Step> steps, int dirty_para, Range selection_before,
                             Range selection_after) = 0;
};

/**
 * 命令层的输入与写口，全是地址（与 PositionView 同一套办法：摆一次就够，因为容器都是
 * 引擎的成员对象、不换对象）。
 *
 * 与 PositionView 有一处不同：位置解析只读，而命令是"改文档"的那一层，所以树与光标是
 * 可写的。谁把某个容器换成了另一个对象（move 走、重新 new 一个），这里就是唯一的失效点。
 */
struct EditView {
    /**
     * 步骤的出口（引擎）。
     */
    EditHost* host = nullptr;

    /**
     * 文档（只读）。命令层只从它读"落点处是什么"，写一律经 EditHost 落成新修订。
     */
    const rev::Doc* doc = nullptr;

    /**
     * 落点与选区的写口。
     */
    CaretResolver* caret = nullptr;

    /**
     * 流索引（树 → 流位置）。没持树时它为空。
     *
     * 持的是 optional 的地址而不是它内容的地址：流索引是"重建时 emplace 进去"的，第一次
     * 重建之前它还没建，取内容的地址会拿到空指针并一直空下去（见 TRAPS 81）。
     */
    const std::optional<rev::FlowIndex>* flow = nullptr;

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
     * 每块占几个流位置。
     */
    const std::vector<int>* block_flow_span = nullptr;

    /**
     * 每块的来源身份（节点号与种类）。
     */
    const std::vector<BlockSource>* block_src = nullptr;

    /**
     * 表格的逻辑格（每格的内容块区间）。整格删除要按格清空时用它 ——
     * 这里是排版输入，不是排版产物，所以拿到的是编译结果里的那一份。
     */
    const std::vector<LayoutTable>* tables = nullptr;

    /**
     * 编译产物里的控件集合（区间树）。
     */
    const ControlSet* controls = nullptr;

    /**
     * 控件下标到节点号的映射。
     */
    const std::vector<NodeId>* control_node_ids = nullptr;

    /**
     * 编辑模式（文档 / 表单 / 只读）。
     */
    const EditMode* mode = nullptr;

    /**
     * 一个 Tab 折成几个空格。
     */
    const int* tab_spaces = nullptr;

    /**
     * 被拒编辑的计数（诊断量，宿主会读）。
     */
    long long* blocked_edits = nullptr;
};

/**
 * 命令层：把一次编辑变成一条事务，再把事务交给 EditHost。
 *
 * 一条编辑可能不止一条事务（"选区 + 打字"是一条：删除与插入合并，撤销一次就回到编辑前
 * 的两端；而"段首退格"先是括号规则搬光标、再并段，是两条）。所以这里不是"返回一份计划"
 * 而是"边算边交" —— 后一步的坐标按前一步重建之后的文档算。
 */
class EditCommand {
public:
    void setView(const EditView& v) { view_ = v; }

    /**
     * 编辑入口：坐标是 (段, 段内码点偏移)，落地前吸附到字素簇边界。
     * @param para 段号
     * @param offset 段内码点偏移
     * @param cps 要插入的码点
     * @return true = 插进去了
     */
    bool insertCodepoints(int para, size_t offset, const std::vector<uint32_t>& cps);

    /**
     * 插入一个码点。与插入一串的那条同一条路，只是参数少一个。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @param cp 一个码点
     * @return true = 插进去了
     */
    bool insertCodepoint(int para, size_t offset, uint32_t cp);

    /**
     * 按 UTF-8 文本插入（落点先吸附到簇边界）。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @param utf8 要插入的文本
     * @return true = 插进去了
     */
    bool insertTextUtf8(int para, size_t offset, const std::string& utf8);

    /**
     * 在光标处插字 / 删字（删除按簇，不是按码点）
     * @param utf8 要插入的文本
     * @return true = 插进去了
     */
    bool typeTextUtf8(const std::string& utf8);

    /**
     * 退格：删光标左边一个字簇。段首时退成合并前一段（见 mergeWithPreviousParagraph）。
     *
     * @return true = 删了
     */
    bool backspace();

    /**
     * 前删：删光标右边一个字簇。段末时退成合并后一段。
     *
     * @return true = 删了
     */
    bool deleteForward();

    /**
     * 跨段编辑：回车在光标处把这一段拆成两段；段首退格、段末 Delete 分别合并前后段 ——
     * 它们是 backspace() / deleteForward() 的自然延伸，不是另外的编辑命令。
     * @return true = 拆了
     */
    bool splitParagraph();

    /**
     * 把这一段并进前一段：段落标记被删掉，光标落到接缝处。
     *
     * @return true = 并了
     */
    bool mergeWithPreviousParagraph();

    /**
     * 把下一段并进这一段：段落标记被删掉，光标位置不动。
     *
     * @return true = 并了
     */
    bool mergeWithNextParagraph();

    /**
     * Tab 缩进：在光标处插入 tab_spaces 个空格，走与敲字相同的策略与重排路径。
     * Shift+Tab 反缩进：删掉光标前最多 tab_spaces 个连续空格。
     * 不做"跳到下一个字段"：Tab 在正文编辑器里表示缩进，换字段用鼠标点。
     * @return true = 缩进了
     */
    bool indent();

    /**
     * 反缩进：删掉光标前最多 tab_spaces 个连续空格。前面不是空格时什么都不做。
     *
     * @return true = 删了
     */
    bool outdent();

    /**
     * 落点判定的唯一入口：一次编辑能不能落在这里 / 能不能删掉这个区间。
     *
     * 六个编辑入口（插入、退格、前删、分段、合并、反缩进）全部走它，所以"判定与动作一致"
     * 是结构上的事实，不是一条要另外维护的约定。对外也开放（引擎原样转出去）：宿主拿它做
     * 禁用态提示，判据拿它把"能不能编辑"与"光标能不能到"分开 —— 表单模式下 setCaret 会把
     * 光标投影进最近的字段，"控件之外"那个位置根本到不了，只有直接问它才问得到。
     * @param p 文本坐标
     * @return 允许不允许，不允许时带原因
     */
    rev::EditCheck policyAt(TextPos p) const;

    /**
     * 删除区间 [a, b) 的判定。跨段删除一律拒（档 1 的删除不跨段，段落标记的删除走合并）。
     * @param a 区间起点
     * @param b 区间终点（不含）
     * @return 允许不允许，不允许时带原因
     */
    rev::EditCheck policyErase(TextPos a, TextPos b) const;

    /**
     * 删除一个控件。keep_content 为真时拆壳留内容（那一段字符变成普通正文），为假时连
     * 内容一起删。
     *
     * 策略走 canDoStructureOp（structureLock 默认锁住）：表单模式默认拒绝、文档模式放行。
     * 删完段数可能变（控件里的段落标记没了或者换了位置），所以走全量重编译。
     *
     * @param index        控件下标（对外一律用下标，节点 id 不外露）
     * @param keep_content 真 = 保留内容，假 = 连内容一起删
     * @param caret_before 删除之前光标在哪（流位置）。撤销恢复的就是它 —— 键盘那一路必须传
     *                     进来：不传（-1）时按"选中整个控件"记，撤销会把控件重新选中，光标
     *                     随之落到它的闭括号之后。
     * @param index 控件下标
     * @param keep_content true = 拆壳留内容
     * @param caret_before 删除前光标在哪（流位置）
     * @return true = 删掉了
     */
    bool removeControl(int index, bool keep_content, FlowPos caret_before = -1);

    /**
     * 改控件的一个运行时属性。幂等：值没变就不提交 —— 撤销栈里多一条什么都没做的记录
     * 只会让人困惑。
     *
     * 返回值分四档是因为调用方要据此决定"要不要重排"：值没变时状态与呈现一个字都没动，
     * 跟着白跑一次重排重绘没有意义；而 hidden 改的是呈现态的流结构（整棵摘除），影响面
     * 不可定位，整篇排版都得作废。
     */
    enum class PropResult {
        Failed,     // 没改成（不是控件、读不出旧值、提交失败）
        Unchanged,  // 幂等：值本来就等于要设的值
        Changed,    // 改了，段结构没变
        Structural  // 改了，而且改的是呈现态的流结构
    };

    /**
     *
     * @param index 控件下标
     * @param prop 要改的运行时属性
     * @param value 新值
     * @return 四档结果（见 PropResult）
     */
    PropResult setControlProp(int index, ControlProp prop, bool value);

    /**
     * 对选区应用一份字符格式补丁（粗体、斜体、下划线、字体、字号、字色）。
     *
     * 只改"补丁里设置了的字段"，没设置的保持原样 —— 这是与样式层三态继承同一套语义：
     *   工具栏按下"加粗"时只该动 bold，不该连带把字号写死成当前值。
     * 幂等：整段格式本来就等于目标值时返回 false，不往撤销栈里塞空记录。
     * 空选区返回 false：落点之后要打的新字沿用哪份格式，是另一件事（见 marksAtFlow）。
     *
     * @param patch 只含要改的字段
     * @return true = 至少有一个文本节点的标记变了
     */
    bool applyCharFormat(const CharFormat& patch);

    /**
     * 对选区覆盖的段落应用一份段落格式补丁（字体、字号、行距、段后距、分页）。
     *
     * 段落与选区的相交判定：选区从段中某处到另一段段首时，后一段不算被覆盖 ——
     *   那正是"选中到行首"与"选中到上一行末"的区别。
     * @param patch 只含要改的字段
     * @return true = 至少有一枚段落标记变了
     */
    bool applyParaFormat(const ParaFormatPatch& patch);

    /**
     * 光标所在位置的字符标记（工具栏回显用）。
     *
     * 返回的是"节点上写了什么"，不含样式层与段落默认值 —— 界面只需要知道
     *   "这一处有没有显式加粗"，而"看起来是不是粗的"由排版结果回答。
     * 空选区报"接着敲字会用的那一份"（与 marksAtFlow 同一个函数，回显与行为不可能分叉）；
     *   有选区报选区覆盖的全部文本节点的共同标记，字段不一致的算缺席。
     * @return 标记；落点周围没有任何文本节点时为空标记
     */
    CharFormat charFormatAtCaret() const;

    /**
     * 光标所在段落的格式覆盖（工具栏回显用）。
     * @return 段落补丁；光标不在任何段落里时为空补丁
     */
    ParaFormatPatch paraFormatAtCaret() const;

    /**
     * 粘贴一段片段：选区先删掉，再按流顺序把每个节点插到落点上。
     *
     * 与打字同一个口径 —— 用户按的是一下，撤销也该是一下：删除与插入合成一次提交。
     * 片段里的块级元素（表格）进不了段落内部，所以它前面后面各补一枚段落标记，
     * 让它自成段；本来就落在段首 / 段尾时不补，避免多出空段落。
     *
     * 一个节点一条 InsertNode 步骤（而不是一棵大树一条）：片段里可能既有文字又有表格，
     * 它们本来就按流顺序排列，"第几项插到哪个位置"由前一项的跨度累加决定，不需要一棵假根。
     *
     * @param nodes 片段内容（按流顺序）
     * @return true = 粘上了
     */
    bool pasteNodes(const std::vector<NodeSpec>& nodes);

    /**
     * 删掉当前选区。没有选区时返回 false —— 按下的那一下没有可做的动作。
     * @return true = 删掉了
     */
    bool deleteSelection();

private:

    /**
     * 位置解析器（光标与选区的真源）：命令算落点、摆光标、查簇边界都经过它。
     * 名字不叫 caret 是因为它同时还要提供 TextPos（pos().caret()），两者同名会打架。
     */
    CaretResolver& pos() const { return *view_.caret; }

    /**
     * 当前修订（视图里的那一份，只读）。
     */
    const rev::Doc& doc() const { return *view_.doc; }

    /**
     * 筛过的块流（按显示名单滤掉部分括号）。
     */
    const std::vector<BlockSpec>& blocks() const { return *view_.blocks; }

    /**
     * 未筛过的块流。编辑要在树上落笔时读它 —— 滤过的副本里没有那些对象。
     */
    const std::vector<BlockSpec>& blocksFull() const { return *view_.blocks_full; }

    /**
     * 每块内容起点的流位置（二分定位用）。
     */
    const std::vector<FlowPos>& blockFlow() const { return *view_.block_flow; }

    /**
     * 每块占几个流位置（流索引给的权威段长）。
     */
    const std::vector<int>& blockFlowSpan() const { return *view_.block_flow_span; }

    /**
     * 每块的来源身份（哪棵子树、被哪枚标记结束）。
     */
    const std::vector<BlockSource>& blockSrc() const { return *view_.block_src; }

    /**
     * 控件集合（区间树）。
     */
    const ControlSet& controls() const { return *view_.controls; }

    /**
     * 控件下标到节点号的映射。
     */
    const std::vector<NodeId>& controlNodeIds() const { return *view_.control_node_ids; }

    /**
     * 编辑模式（决定策略走哪一档）。
     */
    EditMode mode() const { return *view_.mode; }

    /**
     * 一个 Tab 折成几个空格。
     */
    int tabSpaces() const { return *view_.tab_spaces; }

    /**
     * 视图里有没有流索引（没持树时没有）。
     *
     * @return true = 有流索引
     */
    bool hasFlow() const { return view_.flow != nullptr && view_.flow->has_value(); }

    /**
     * 流索引（hasFlow 为真时才可用）。
     */
    const rev::FlowIndex& flowIndex() const { return **view_.flow; }

    /**
     * 记一笔"这一下被策略拒了"。
     */
    void noteBlocked() const {
        if (view_.blocked_edits != nullptr) ++*view_.blocked_edits;
    }

    /**
     * 把一批步骤交出去 —— 落地与重建产物是持有者的事。
     *
     * 选区快照随步骤一起给出：修订只记内容、不记光标，而撤销要回到"按下去之前"的那个位置。
     */
    bool commit(std::vector<Step> steps, int dirty_para) const {
        return view_.host != nullptr && view_.host->commitSteps(std::move(steps), dirty_para,
                                                                pending_before_, pending_after_);
    }

    /**
     * 光标紧邻的那一枚原子整枚删掉（退格看左边那一格、前删看右边那一格）。
     *
     * 原子没有可寻址的内部：它只占一个流位置，"删掉这一侧的那一个东西"落到它身上就是整枚
     * 移除。与括号那四条规则同一个语义，所以两者可以挨着排 —— 而且它们不会同时成立：
     * 光标紧邻一枚原子时，它就没有贴在任何一枚括号上。
     *
     * 判定走 canEditRange 而不是 canDoStructureOp：原子是内容，与一个字符同类，表单模式里
     * 该按"这一格在不在可编辑集合内"判，而不是按结构锁判。
     *
     * @return 真 = 已经删掉（整个按键做完了）；假 = 这一侧不是原子，调用方照原路走
     */
    bool eraseAtomBeside(FlowPos cur, bool left);

    /**
     * 控件边界上的一次删除落到哪一类（四条规则见 bracketBackspace 的实现处）。
     */
    enum class BracketHit {
        None,           // 不贴任何括号：调用方走普通删除
        MoveOnly,       // 只搬光标，不删东西
        MoveThenErase,  // 光标搬到 at，调用方接着删那个方向上的一个字符
        Handled         // 整个按键已经做完（删掉了控件本身）
    };

    /**
     * 选区被编辑入口"吃掉"时折算出来的一份删除计划（见 planEraseSelection）。
     */
    struct SelectionErase {
        /**
         * 折算出来的删除步骤（未提交）。
         */
        std::vector<Step> steps;

        /**
         * 删除之前的选区，撤销要回到它。
         */
        Range before{};

        /**
         * 删除区间起点的流位置，删完光标落这里。
         */
        FlowPos at = 0;

        /**
         * 策略拒绝了删除动作（调用方中止整条编辑）。
         */
        bool blocked = false;
    };

    /**
     * 表单模式的安全阀：一份"一个可编辑控件都没有"的文档退化为文档模式，而不是什么都不让改。
     *
     * 理由：一个还没配字段的文档切到表单模式，用户期望的是"跟平时一样"，而不是突然一个字都
     *   打不进去 —— 后者会被当成功能坏了，没人会想到是策略在生效。它是个刻意的安全阀，不是
     *   遗漏；将来若要做"严格表单"，应该另加一个 EditMode，不要改这里。
     *
     * 判据只看导出后的控件集合（隐藏控件在呈现态本就不在里面），有一个非只读的就为真。
     */
    bool formDegraded() const;

    /**
     * 导出后的控件集合里有没有可编辑的那一个。formDegraded 用它。
     */
    bool hasEditableControl() const;

    /**
     * 控件的流区间。不在流里（隐藏、摘除）返回 false。
     */
    bool controlFlowSpan(int index, FlowPos* lo, FlowPos* hi) const;

    /**
     * 控件内部除了自己那两枚括号什么都没有。只有它为真才允许一次按键删掉整个控件 ——
     * 有内容时按键删的永远是内容里的一项，控件本身得等它被删空。
     */
    bool controlIsEmpty(int index) const;

    /**
     * 流区间终点正好是 p 的控件（= 它的闭括号占 p-1 那一格）；没有返回 -1。
     * 括号各占一格且互不相同，所以这个匹配唯一，不需要挑"最内层"。
     */
    int controlEndingAt(FlowPos p) const;

    /**
     * 流区间起点正好是 p 的控件（= 它的开括号占 p 那一格）；没有返回 -1。
     */
    int controlStartingAt(FlowPos p) const;

    /**
     * 退格贴着控件括号时的四种情形（从左到右的书写方向）。
     *
     * 光标在闭括号右面：控件有内容 ⇒ 光标进控件（落到内容末尾），再删掉左边那一项；空 ⇒ 删控件。
     * 光标在开括号右面：控件有内容 ⇒ 光标退到开括号左面，一个字都不删；空 ⇒ 删控件
     *   （空控件的内部就这一格，两条规则落在同一个位置上）。
     *
     * "删掉左边那一项"只有两种东西：一个字符，或者一个空控件。左边是个有内容的控件时，就按
     * 同一套规则再往里进一层 —— 嵌套不是特例，是这个循环自然走出来的（参考实现 DCWriterLite
     * 的 DocumentControler.Backspace 也是拿 while 一路进入）。选"进入"而不是"直接删掉整个
     * 内层控件"，是因为后者会把用户没打算删的内容一起带走。
     */
    BracketHit bracketBackspace(FlowPos* at);

    /**
     * 前删贴着控件括号时的四种情形，与退格左右对称。
     *
     * 光标在开括号左面：有内容 ⇒ 光标进到开括号右面，删掉右边那一项；空 ⇒ 删掉控件。
     * 光标在闭括号左面：有内容 ⇒ 光标跳到闭括号右面（跳出控件），删掉右边那一项；空 ⇒ 删掉控件。
     *
     * 跳出之后右边可能又是一枚闭括号（内层控件正好是外层的最后一项），那就继续往外跳 ——
     * 括号本身没有"删掉它"这个动作，能删的永远是内容。
     */
    BracketHit bracketDeleteForward(FlowPos* at);

    /**
     * 合并的一般形式：把 head + 1 段并进 head 段。三种入口（段首退格 / 段末 Delete /
     * 显式调用）只是"从哪一端触发"不同，落到文档上的操作是同一个。
     */
    bool mergeParagraphInto(int head);

    /**
     * 树上的一段删除：把同一段内 [a, b) 这段（已吸附到簇边界）删掉。
     *
     * 退格、前删、反缩进三个入口只是算区间的方式不同，落到文档上的操作是同一个 ——
     * 一条 makeDeleteTextSteps + 一次事务提交。策略判定留在调用方：被拒时要报的是
     * "用户按的那一下没生效"，与"这一步做不成"不是一回事。
     *
     * caret_at_end 说的是"删除之前光标在这段文本的哪一端"，撤销要靠它把光标放回原处：
     * 退格与反缩进的光标在右端（删完留下的字在光标左边），前删在左端。记成左端的话，
     * 退格一次再撤销，光标会落到被恢复那个字的前面 —— 而它删除前明明在后面。
     */
    bool eraseOnTree(TextPos a, TextPos b, const char* label, bool caret_at_end = false);

    /**
     * 流区间 [from, to) 上的删除步骤（不提交）：区间覆盖到的文本按文本节点切
     * （makeDeleteTextSteps），整覆盖到的段落标记各发一步 RemoveParaMark。
     *
     * 两条构造混在一起从后往前发 —— 每步的坐标是它自己应用那一刻的坐标，而"删右边不会
     * 移动左边"正是从后往前的全部理由。分两批发（先文本后标记，或反过来）会让后一批的
     * 坐标过期。区间的总覆盖长度对不上时返回空：宁可整条编辑被拒，也不要删掉半截区间。
     */
    std::vector<Step> eraseFlowSteps(FlowPos from, FlowPos to) const;

    /**
     * 整格选区的删除步骤：选区覆盖到的每一格各删一次内容，合并成一批。
     *
     * 与 eraseFlowSteps 的差别是"不跨格边界" —— 格的结束标记（表格结构的一部分）不在
     * 任何一格的内容区间里，于是不会被删掉；按整条区间删则必然把它卷进去，安全阀过不去，
     * 整条编辑被拒（症状：选中几格，按 Del 没反应）。
     */
    std::vector<Step> eraseCellContents(FlowPos from, FlowPos to) const;

    /**
     * 被选区整张覆盖的表，整张删掉（连同表外那些纯文本段一起），不覆盖的表一张都不动。
     *
     * 为什么需要这条路：表格是块级容器，它的结构位（格尾、行尾）不在任何一条可删区间里，
     *   所以按流区间删到一张表，覆盖长度永远对不上安全阀，整条编辑被拒 —— 症状就是
     *   全选之后按 Del 什么也不发生。规则与"表格边界上退格 = 删整张表"同一条：要么整张
     *   删，要么不碰。
     *
     * "覆盖"要求选区把它整个跨过去（起在表首之前或终在表尾之后），全选例外。两头都落在
     *   表内的文字选区只是选了这张表里的字，删表比用户选的多 —— 那种情形不在这里处理。
     *
     * @return 空 = 选区里没有"整张被跨过去"的表，调用方照原路走
     */
    std::vector<Step> eraseWholeTables(FlowPos from, FlowPos to) const;

    /**
     * 两个相邻块在同一个单元格里（或都不在表格里）。
     *
     * 合并段落要拿它把关：格的最后一段与下一格的第一段之间隔着单元格边界，删掉那枚段落
     * 标记不会把两段并起来，只会让前一格少一枚结束标记 —— 那一格的内容一旦删空，整格连
     * 一个块都不剩，从此点不进去（它的行盒不存在，命中测试只能落到相邻格上）。跨格合并
     * 因此在结构上没有意义，一律拒。
     *
     * 这条判据同时决定了界面上的一条规则：在格内第一段的开头按退格、末尾按前删，
     * 什么都不发生。表格不会被这两条按键删掉 —— 光标在表内，删的是内容；要删整张表得
     * 用选区把它整个选上（见 planEraseSelection）。
     */
    bool sameCellBlocks(int a, int b) const;

    /**
     * 把当前选区折算成"删掉这一段"的步骤（不提交）。
     *
     * 编辑入口的统一前缀：任何编辑在选区非空时都是"先删掉选区，再在原地做这件事"。
     * 空选区返回空 steps（调用方照原路走）；策略拒绝时置 blocked —— 调用方据此中止整条
     * 编辑，与"这一下没发生"同一口径，被拒计数也已经记过一笔。
     *
     * 为什么不在这里直接提交：用户在"选中 + 打字"里按的是一下，撤销也该是一下。删除与
     * 插入合并进同一个事务，撤销一次就回到编辑前的两端。
     */
    SelectionErase planEraseSelection();

    /**
     * 把删除计划提交成一次事务，并把光标摆到删除区间的起点。
     */
    bool commitErase(const SelectionErase& plan, const char* label);

    /**
     * 用一段文本替换当前选区 —— 打字与缩进的共同落点。
     *
     * 删除与插入合并成一次事务：用户按下的是一下，撤销也该是一下。分成两个事务的话，
     * 第一次撤销只退掉那个字、选区还没回来，而"撤销回到编辑前两端"要求一次就回去。
     *
     * 插入的坐标直接用删除区间的起点 —— 那正是选区被删掉之后光标所在的位置（那些删除
     * 都在它右边，删右边不会移动左边的坐标，所以它在整批步骤里始终有效）。沿用的字符
     * 格式得在删除之前取：要的是"落点左边那个字"的格式，删完就取不到了。
     */
    bool replaceSelectionWithText(const std::vector<uint32_t>& cps);

    /**
     * 落点处接着写字会沿用的字符格式：取紧挨在它左边的那个文本节点的标记与样式引用。
     *
     * 不取"块级默认"：那样在粗体字中间敲一个字会得到不粗的新字。取邻居是"输入沿用落点处
     * 已有样式"这条约定的最小实现；左边没有文本节点时取右边的（段落 / 控件开头），
     * 两边都没有才返回空标记（= 全继承）。
     *
     * 注意"左边"不等于"posAt 落在哪个节点"：两段之间的那个流位置在规范表示里归给右边
     * 那个叶子，直接用它就成了沿用右边的格式（详见实现处的注释）。
     * @param at 落点（流位置）
     * @param style_ref 非空时写入所采用节点的样式引用
     * @return 字符标记
     */
    CharFormat marksAtFlow(FlowPos at, std::string* style_ref = nullptr) const;

    /**
     * 选区开头那一段的字符格式：打字盖住选区时沿用的那一份。
     *
     * 与 marksAtFlow 是两条规则：那一条取"左边的邻居"（接着已有的字往下写），这一条取
     * 选区里的第一个文本节点（把这段字换掉，换上去的还是这段字的格式）。选中加粗的一段
     * 再敲，敲出来的仍是加粗；若也按左邻居取，紧接普通文字后面的加粗段一改就变普通了。
     * @param lo 选区起点（流位置）
     * @param hi 选区终点（流位置）
     * @param style_ref 非空时写入所采用节点的样式引用
     * @return 字符标记；选区里没有文本节点时退回 marksAtFlow(lo)
     */
    CharFormat marksAtSelectionStart(FlowPos lo, FlowPos hi, std::string* style_ref) const;

    /**
     * 提交要记的两份选区。撤销与重做都靠它摆回光标。一条编辑可以提交多次（分段之后
     * 再改段落格式），此时以最后一次给出的两份值为准。
     */
    Range pending_before_{};

    /**
     * 片段里有没有块级元素。块级元素自成段落，粘贴要自己把段落断开。
     * @param nodes 片段内容
     * @return true = 含表格
     */
    static bool hasBlockLevel(const std::vector<NodeSpec>& nodes);

    /**
     * 流位置 at 处是不是"段落开头"（紧邻左边的是一枚段落标记，或它就在容器内容起点）。
     * @param at 流位置
     * @return true = 已经是段首
     */
    bool startsParagraph(FlowPos at) const;

    /**
     * 流位置 at 处是不是"段落结尾"（紧邻右边的是一枚段落标记，或它已经在容器内容末尾）。
     * @param at 流位置
     * @return true = 已经是段尾
     */
    bool endsParagraph(FlowPos at) const;

    /**
     * 这一批步骤提交之后光标该在哪（随步骤一起交给宿主）。
     */
    Range pending_after_{};

    /**
     * 只读输入（见 EditView）。引擎在重建文档与重排之后摆一次。
     */
    EditView view_{};
};

}  // namespace heditor
