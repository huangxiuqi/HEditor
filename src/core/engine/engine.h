/**
 * 引擎门面：原生宿主与 WASM 宿主共用同一份实现。
 * 这一层就是"平台边界"——向上只暴露三样东西：按页给出的 RGBA 像素（pagePixels()，
 * 宿主贴到各页画布）与一份窗口导出图（frame()，给诊断与取图）；一组度量数字
 * （EngineStats / EditStats），用于性能取证；一组编辑与光标查询入口
 * （insertTextUtf8 / backspace / caretRectPt / hitTestPt）。
 * 坐标一律是"段 + 码点偏移"，不暴露任何内部指针。
 * 边界上流的是"一大块位图"，不是几千条绘制指令——这是 Figma 那条纪律
 * （"循环留在引擎内，跨边界只传大块"）的最小可用形态。
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/doc/compile.h"
#include "core/doc/control.h"
#include "core/doc/document.h"
#include "core/doc/edit_mode.h"
#include "core/doc/flow.h"
#include "core/engine/caret.h"
#include "core/engine/edit_command.h"
#include "core/engine/engine_config.h"
#include "core/engine/engine_stats.h"
#include "core/layout/layout.h"
#include "core/render/bitmap.h"
#include "core/rev/capability.h"
#include "core/rev/flow.h"
#include "core/rev/project.h"
#include "core/rev/rev.h"
#include "core/rev/serialize.h"
#include "core/rev/session.h"
#include "core/text/font.h"
#include "core/text/writing_mode.h"

namespace heditor {

/**
 * 一次复制的结果：片段文本、它的纯文本投影、实际取用的流区间。
 *
 * 区间可能比选区宽：端点落在容器内部时整块一起复制（半个控件粘不回去）。剪切要删的正是
 * 这一段，所以两个值必须一起交出来，不能各算一次。
 */
struct ClipCopy {
    /**
     * 片段 JSON 文本（跨实例的通道）。
     */
    std::string fragment;

    /**
     * 这一段的纯文本投影（系统剪贴板的 text/plain）。
     */
    std::string text;

    /**
     * 这一段的 HTML（系统剪贴板的 text/html，带字符格式）。
     */
    std::string html;

    /**
     * 实际取用的流区间起点。
     */
    FlowPos lo = 0;

    /**
     * 实际取用区间的终点（不含）。
     */
    FlowPos hi = 0;

    /**
     * 取到了没有。
     */
    bool ok = false;

    /**
     * 失败原因（没有选中内容、写不出片段）。
     */
    std::string error;
};

class EditorEngine : private EditHost {
public:

    /**
     * 装载字体并建库。路径顺序决定字体索引 —— 字体链与文档里的字号都按它说话。
     *
     * @param fontPaths 字体文件路径（顺序即字体索引）
     * @return true = 全部装上了
     */
    bool init(const std::vector<std::string>& fontPaths);

    /**
     * 整份换配置。与逐项设的区别是它一次算清"哪些派生量要跟着变"：
     * 页度量变了整篇重排，光栅配置变了整份页缓存作废，两者都是不可定位的影响面。
     *
     * @param cfg 新的配置（整份替换）
     */
    void setConfig(const EngineConfig& cfg);

    /**
     * 当前配置（引擎夹取之后的生效值）。宿主读回来的就是这个 —— 它不必再抄一份夹取规则。
     *
     * @return 生效中的配置
     */
    const EngineConfig& config() const { return cfg_; }

    /**
     * 只改光标与选区的样式（颜色、光标宽度）。
     *
     * 为什么不复用 setConfig：那一处会作废页缓存，而页缓存重建要把窗口里的每一页重新
     *   光栅（3 页约 32 ms）。样式由界面上的取色器连续改时，每一下都付这笔钱。
     *   页位图里没有光标与选区 —— 它们是导出时叠上去的瞬态，所以样式改了只需再呈现一次。
     *
     *   调用方改完要呈现一次（引擎不自己重绘）。
     *
     * @param caret 光标颜色
     * @param caret_width_pt 光标条宽度（磅）
     * @param selection 选区颜色
     */
    void setCaretAndSelectionStyle(const Color& caret, float caret_width_pt,
                                   const Color& selection);

    /**
     * 预热一页的字形：把它的 mask 光栅一遍送进字形缓存，不呈现、不改画面。
     *
     * 为什么要有它：滚动换窗那一拍最贵的部分是冷字形光栅 —— 2000 段长文档 @1.5 倍
     *   一页约 1500 个字形，冷缓存下实测 50~90 ms（热缓存只要 8~12 ms），而它必须
     *   在换窗那一拍里做完，于是"滚动时那几页还没画出来"。把这笔钱挪到当前画面还
     *   够用的空闲帧里先付掉，换窗就只剩铺底色与脏矩形记账。
     * 返回本次预热触及的字形数（0 = 这一页没有字形或还没排版）。
     *
     * @param page 页号
     * @return 本次预热触及的字形数（0 = 没有字形或还没排版）
     */
    int prewarmPageGlyphs(int page);

    /**
     * 页度量：断行依赖内容宽度，所以改它必须整篇重排（影响面不可定位）。
     *
     * @param page 页度量（页宽、页高、页边距）
     */
    void setPage(const PageMetrics& page) {
        if (page != cfg_.page) markAllLayoutDirty();
        cfg_.page = page;
    }

    /**
     * 排版输入。名单覆盖全部控件时它就是 blocks_full_（同一份，不拷贝）；
     * 只有真的筛掉了某些括号时才有另一份。
     *
     * @return 块流（名单覆盖全部控件时就是唯一那一份，不拷贝）
     */
    const std::vector<BlockSpec>& blocks() const { return filter_active_ ? blocks_ : blocks_full_; }

    /**
     * 与块流同一层的产物：表格结构（宽度、列宽、行规则、每格的跨度与块区间）。
     *
     * 单列一个读口是因为它与 blocks() 是两种形状：块流描述"排什么"，这里描述"框在哪"。
     * 只比块流看不出列宽与跨度的错，正如只比表格看不出文字。
     *
     * @return 表格结构（宽度、列宽、行规则、每格的跨度与块区间）
     */
    const std::vector<LayoutTable>& tableSpecs() const { return tables_; }

    /**
     * 文档装载：把模型树换成一份修订（rev::fromTree），此后引擎只持修订，不再持树。
     *
     * 表示不了的形状（目前是多节）整份拒绝、状态停在"没有装载"——装一半的结果是一份看起来
     * 完整的修订，而它与模型不是同一份文档，调用方没有别的办法知道。控件边界、段落格式与
     * 字符格式因此仍只有一个产处（投影），引擎不自己拼。
     *
     * 隐藏控件的可见性口径与投影同一处取值（见 rebuildFromTree），所以 setEditMode 必须重建。
     * 装载会复位光标、作废页缓存并整篇标脏。
     *
     * @param tree 模型树
     * @param sheet 样式表
     */
    void setDocument(const NodeTree& tree, const StyleSheet& sheet);

    /**
     * 持有中的文档。引擎只认这一条装载入口，所以它一直有效（未装载时是一份空修订）。
     *
     * @return 持有中的修订（未装载时是一份空修订）
     */
    const rev::Doc& document() const;

    /**
     * 保存：把当前修订写成 JSON 文本（格式见 rev::serialize）。
     *
     * 存的是"形状与身份"，不存任何派生量 —— 打开时重新投影、重排、重光栅，像素必然回到
     * 保存时的样子（判据就是这么写的：存盘 → 新引擎 → 打开 → 整帧逐字节相同）。
     *
     * 文件里只有文档树：命名样式表（StyleSheet）与二进制资源都还没有进文件 —— 产品至今
     * 没有"新建命名样式"的入口，样式表恒为空，所以这不丢信息；等样式编辑落地时要带版本号
     * 加进同一份文件，不能在 v1 里改它们的含义。
     *
     * @return 文档 JSON 文本与失败原因
     */
    rev::SerializeResult saveDocument() const;

    /**
     * 打开：JSON 文本 → 修订 → 装上（与 setDocument(tree) 同一套收尾）。
     *
     * 失败（版本不符 / 结构坏了 / 节点号重复）时引擎一个字节都不动：先解析到局部变量，
     * 成功了才碰会话。调用方拿到的 error 是给人看的一句话，不是异常路径的副产品。
     *
     * 打开会复位光标、清空撤销栈、作废页缓存并整篇标脏 —— 与装载同一口径：
     * 历史里那些旧修订属于"上一份文档"，留着只会让撤销把两份文件混在一起。
     *
     * @param text 文档 JSON 文本
     * @return 成功与否与给人看的一句话（失败时引擎一个字节都不动）
     */
    rev::DeserializeResult loadDocument(const std::string& text);

    /**
     * 复制：把当前选区取成一段可移植的片段（格式见 rev::fragment）。
     *
     * 取的是结构而不是文字：控件、表格、图片、公式连同各自的记录一起带走，粘贴回来是同一段
     * 内容，不是"一段长得像的文字"。片段自带字符串表与字体表，所以跨实例、跨标签页都成立 ——
     * 那边不需要知道这一边的节点号，也不需要字体装载顺序一致。
     *
     * 端点落在容器（控件、表格、行、格）内部时整块一起复制，effective 区间随之扩到那块的
     * 两端；剪切要删的正是这一段，所以两个区间都交给调用方。
     *
     * @return 片段文本（JSON）、纯文本投影、实际取用区间与失败原因
     */
    ClipCopy copySelection() const;

    /**
     * 粘贴片段：解析后按 InsertNode 插到光标处，选区先删掉（一次编辑一次撤销）。
     *
     * 落点判定与打字同一条（表单模式下控件之外落不进去）。失败时引擎一个字节都不动 ——
     * 片段读坏了、或者落点不接受这些内容，都返回原因而不是"粘了一半"。
     *
     * @param text 片段 JSON 文本（来自 copySelection 或另一个实例的剪贴板）
     * @return 空串 = 成功；否则是给人看的原因
     */
    std::string pasteFragment(const std::string& text);

    /**
     * 粘贴纯文本：按 CRLF / CR / LF 切成段落，每段一个文本节点，段落之间插段落标记。
     *
     * 给"从别的应用粘文字"这条通用做法用。字符格式留空表示全继承落点处已有格式，
     * 与输入沿用邻居格式同一条约定。
     *
     * @param utf8 纯文本
     * @return 空串 = 成功；否则是给人看的原因
     */
    std::string pastePlainText(const std::string& utf8);

    /**
     * 删掉当前选区（剪切的后一半，也是"选中之后按 Del"的那一条）。
     * @return true = 删掉了
     */
    bool deleteSelection();

    /**
     * 粘贴片段并收尾（重排 + 呈现）。与别的编辑入口同一口径：宿主一次调用就完事。
     * @param text 片段 JSON 文本
     * @return 空串 = 成功；否则是给人看的原因
     */
    std::string pasteFragmentAndRender(const std::string& text);

    /**
     * 粘贴纯文本并收尾。
     * @param utf8 纯文本
     * @return 空串 = 成功；否则是给人看的原因
     */
    std::string pastePlainTextAndRender(const std::string& utf8);

    /**
     * 删除选区并收尾。
     * @return true = 删掉了
     */
    bool deleteSelectionAndRender();

    /**
     * 对选区应用字符格式并收尾。
     * @param patch 只含要改的字段
     * @return true = 至少有一个文本节点的标记变了
     */
    bool applyCharFormatAndRender(const CharFormat& patch);

    /**
     * 对选区覆盖的段落应用段落格式并收尾。
     * @param patch 只含要改的字段
     * @return true = 至少有一枚段落标记变了
     */
    bool applyParaFormatAndRender(const ParaFormatPatch& patch);

    /**
     * 光标处的字符标记（工具栏回显），JSON 文本。
     *
     * 默认报"这一处显式写了什么"（三态：没有、true、false）；effective 为真时报"实际
     *   生效的那一份" —— 样式链与段落默认值都已算进去，字号字体永远有值。工具栏要显示
     *   "当前是什么字体、多少磅"就得读后者：正文一个字都没改过，也仍然有一个字体一个字号。
     * @param effective 是否要算上样式层与默认值
     * @return 标记的 JSON（可能为空对象）
     */
    std::string charFormatAtJson(bool effective = false) const;

    /**
     * 落点处实际生效的字符格式（字形真正用的那一份），JSON 文本。
     *
     * 值来自排版产物里覆盖落点的那个 run，所以它与画面上看见的逐字一致（不是再算一遍）。
     * @return 标记的 JSON；落点不在任何段里时为空对象
     */
    std::string effectiveFormatAtJson() const;

    /**
     * 光标所在段落的格式覆盖（工具栏回显），JSON 文本。
     * @return 段落补丁的 JSON（可能为空对象）
     */
    std::string paraFormatAtJson() const;

    /**
     * 段落坐标与模型流位置的桥接 —— 第十四节那条接缝的另一半。
     *
     * 引擎对外一直说 (段, 段内码点偏移)，模型层说流位置；两者不能靠算式互换 —— 控件与节这些
     * 容器在流里各占起止两位，而段的码点里没有它们。所以换算只认一个权威：编译给出的块来源
     * 身份（BlockSource）加 FlowIndex。桥是单向的：模型是真相，段坐标是它的一个视图。
     * 树是唯一的装载入口，所以桥一直存在；没装载过（段流为空）时入口返回 -1 与 false。
     *
     * owner_hint 是宿主与探针的说话方式（它们只认控件下标）：同一处边界上"控件之前 /
     * 进入控件"是两个位置，提示把它投影成一个控件下标。引擎内部一律说侧别，见 flowPosAtSide。
     *
     * @param para 段号（块下标）
     * @param cp_offset 段内码点偏移
     * @param owner_hint 归属提示（控件下标；-1 = 没有）
     * @return 全局流位置；没装载过时为 -1
     */
    FlowPos flowPosAt(int para, size_t cp_offset, int owner_hint = -1) const;

    /**
     * 桥接的底层：位置 = (段, 码点偏移, 侧别)。侧别见 caretSide 的注释。
     *
     * @param para 段号（块下标）
     * @param cp_offset 段内码点偏移
     * @param side 侧别（这一处跨过了几枚括号；小于 0 = 没有信息）
     * @return 全局流位置
     */
    FlowPos flowPosAtSide(int para, size_t cp_offset, int side) const;

    /**
     * 反向：流位置 → (段, 段内码点偏移)。落在段间（段落标记、节边界）上时归到相邻段的端点上
     * —— 模型里的位置只有"内容上"的，没有"段间"的。不持树时返回 false。
     *
     * @param flow 全局流位置
     * @param out 写回值
     * @return true = 翻得出来（不持树时为 false）
     */
    bool flowToTextPos(FlowPos flow, TextPos* out) const;

    /**
     * 流的总长度（最后一个合法位置 + 1）。它是流索引给的，与块流那边数出来的前缀和是
     * 两份独立的口径 —— 判据拿两者对拍（见 edit_test 的"位置划分"那一节）。
     *
     * @return 流的总长度（最后一个合法位置 + 1）
     */
    FlowPos flowSize() const;

    /**
     * 第 para 块内容起点的流位置 / 它占的位数。越界时给 0。
     *
     * 对外只给这两个读数，不给数组：调用方拿数组就会有人按下标扫一遍，那是 O(n) 的隐藏
     * 代价；而"块的位置"本来就该由这一对读数表达。
     *
     * @param para 段号（块下标）
     * @return 块内容起点的流位置；越界时为 0
     */
    FlowPos blockFlowStart(int para) const;

    /**
     * 第 para 块在流里占几个位置（= 文本码点数 + 控件边界数）。
     *
     * @param para 段号（块下标）
     * @return 位数；越界时为 0
     */
    int blockFlowSpan(int para) const;

    /**
     * 重排版：从 layout_dirty_from_ 起的段重排，之前的段原样复用（续排契约见
     * core/layout/layout.h）。没有脏段时按全量处理 —— 宁可多排一次，也不拿旧前缀
     * 去接新后缀。页的内容随之作废，呈现时按页指纹增量重画。
     */
    void relayout();

    /**
     * 全量：保证窗口内每页的内容新鲜，再按当前窗口导出像素。
     */
    void repaint();

    /**
     * 只渲染 [first_page, first_page+count) 这几页（连续视图）。
     *
     * @param first_page 窗口首页
     * @param count 要画的页数
     */
    void paintPageRange(int first_page, int count);

    /**
     * 视口锚点：这一拍只保证 [first_page, first_page+page_count) 这几页是新鲜的。
     *
     * 页位图按页缓存，窗口外只留两条 —— 所以不设锚点时（整篇），缓存与光栅量都按
     *   文档页数走；长文档上是内存与装载耗时的双重放大。产品宿主按"可见页 ± 余量"设它，
     *   单测与探针不设（整篇），两种情形下页像素本身逐字节相同。
     *
     * @param first_page 窗口首页
     * @param page_count 锚定的页数（<= 0 = 整篇）
     */
    void setViewportAnchor(int first_page, int page_count);

    /**
     * 本拍写过（有脏矩形）的页（绝对页号，升序去重）。
     * 宿主据 pageDirtyPagesJson 决定贴哪几页；这一份是"本拍写过什么"的读数，
     * 供面板与探针用 —— 两者的区别见 pageDirtyPagesJson 的注释。
     *
     * @return 本拍写过脏矩形的页号（升序去重）
     */
    const std::vector<int>& dirtyPages() const { return last_dirty_pages_; }

    /**
     * 本次呈现覆盖的首页（= 宿主算页偏移与命中换算的基准）。
     *
     * 这是"当前窗口"的唯一读数。它由 presentInternal 按视口锚点落定 ——
     *   不再有"帧槽"这一层：窗口就是 [win_first_page_, win_first_page_ + win_page_count_)，
     *   每页各有一张自己的位图，页在画布上的位置是 (页号 x 步距)，与窗口开在哪无关。
     *
     * @return 窗口首页（宿主算页偏移与命中换算的基准）
     */
    int windowFirstPage() const { return win_first_page_; }

    /**
     * 本次呈现覆盖的页数。
     *
     * @return 窗口页数
     */
    int windowPageCount() const { return win_page_count_; }

    /**
     * 页在设备像素下的几何。宿主用它给页画布定位、把页内坐标换成文档坐标。
     * 三者都是 cfg_ 与 zoom 的纯函数。
     *
     * @return 页宽（设备像素）
     */
    int pagePxWidth() const;

    /**
     * 页高（设备像素）。
     *
     * @return 页高
     */
    int pagePxHeight() const;

    /**
     * 页间隙（设备像素）。连页模式下它是 0。
     *
     * @return 间隙
     */
    int pageGapPx() const;

    /**
     * 光标。对外坐标仍是 (段, 段内码点偏移)，任何设置入口都会先夹到合法范围、
     * 再吸附到字素簇边界 —— 所以光标永远不会停在半个 é（e + U+0301）中间。
     *
     * 引擎内部的真源是一对全局流位置（selection_ 的 anchor 与 focus，见它的注释），
     * 段号与段内流位置都由 focus 投影出来。下面这些入口都是"把别的坐标系翻成它"，
     * 对外接口一个字没变。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     */
    void setCaret(int para, size_t offset);

    /**
     * 按文本坐标定位（重载）。
     *
     * @param p 文本坐标
     */
    void setCaret(TextPos p) { setCaret(p.para, p.offset); }

    /**
     * 带"归属"的定位：点画布时用户点的是某个字段的括号，位置本身（共享边界上的
     * 那个偏移）分不出是哪一侧，于是把结论一起带进来。归属是侧别的投影，见 setCaretAtSide。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @param field 落点归属（控件下标）
     */
    void setCaretAt(int para, size_t offset, int field);

    /**
     * 带归属的定位（重载）。
     *
     * @param p 文本坐标
     * @param field 落点归属（控件下标）
     */
    void setCaretAt(TextPos p, int field) { setCaretAt(p.para, p.offset, field); }

    /**
     * 带侧别的定位：点击（宿主把 hitTestPt 量到的侧别带回来）与文档重建后搬回光标走它。
     * 侧别见 caretSide 的注释。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @param side 侧别（这一处跨过了几枚括号；小于 0 = 没有信息）
     */
    void setCaretAtSide(int para, size_t offset, int side);

    /**
     * 带侧别的定位（重载）。
     *
     * @param p 文本坐标
     * @param side 侧别（这一处跨过了几枚括号；小于 0 = 没有信息）
     */
    void setCaretAtSide(TextPos p, int side) { setCaretAtSide(p.para, p.offset, side); }

    /**
     * 带侧别与行别的定位（点击落点）。
     *
     * 行别说的是"折行处贴哪一行"：一行排满折到下一行时，上一行的末尾与下一行的开头
     * 是同一个位置（偏移与侧别都一样），光凭位置分不开。默认贴上一行行尾；点下一行的
     * 行首时点明贴下一行，光标才画在点的地方。行别见 caretLineAffinity。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @param side 侧别（这一处跨过了几枚括号；小于 0 = 没有信息）
     * @param line_aff 行别（0 = 贴上一行，大于 0 = 贴下一行）
     */
    void setCaretAtSideLine(int para, size_t offset, int side, int line_aff);

    /**
     * 拖选时的焦点端定位：只搬 focus，anchor 不动。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @param side 侧别（这一处跨过了几枚括号；小于 0 = 没有信息）
     * @param line_aff 行别（0 = 贴上一行，大于 0 = 贴下一行）
     */
    void setFocusAtSideLine(int para, size_t offset, int side, int line_aff);

    /**
     * 拖选：把焦点端搬到点击位置，anchor 端不动。
     *
     * 与 setCaretAtSide 只差收尾方式：那边两端一起摆到新位置（定位），这边只动 focus。
     * 按下时宿主先用 setCaretAtSide 定位，于是 anchor 落在按下点；拖动过程中的每一次
     * 移动都走这里，选区就是从按下点到当前位置那一段。侧别的含义与 setCaretAtSide 相同。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @param side 侧别（这一处跨过了几枚括号；小于 0 = 没有信息）
     */
    void setFocusAtSide(int para, size_t offset, int side);

    /**
     * 把光标摆到一个流位置上（绝对）。编辑路径全用它：删除后的落点是删除区间的起点、
     * 插入后是插入区间的终点、Tab 的目标、撤销恢复的位置，它们都已经是流位置。
     *
     * @param flow 全局流位置
     * @return true = 摆好了
     */
    bool setCaretAtFlow(FlowPos flow);

    /**
     * 选区：一对流位置，带方向（anchor 是不动的那一端）。
     *
     * 引擎从第一天起就只存这一个值（见 selection_ 的注释），此前对外只露它的 focus；
     * 下面这几个入口把它按选区露出来。空选区（两端重合）就是光标 —— 所以
     * hasSelection() 为假时 caret() 给出的就是它的位置。有选区时 caret() 是插入点：
     * 文字选区在 focus 那一端，整格选区在焦点那一格的末尾。两者始终只有一份状态。
     *
     * @return 选区（整格时是吸附到格边界的那一对）
     */
    Range selection() const { return caret_.selection(); }

    /**
     * 选区非空：两端不重合。它判的是"有没有选中东西"，不是"光标在不在"。
     *
     * @return true = 两端不重合（有文字被选中）
     */
    bool hasSelection() const { return caret_.hasSelection(); }

    /**
     * 程序化设选区（宿主恢复、探针摆位）。两端各自夹到文档流的合法范围内。
     * 传空区间就等于定位。
     *
     * 这里不做端点规范化（外扩到控件边界）：那是"要取出片段"时才需要的性质
     * （见 range.h 的 normalizeRange），而拖选出来的端点本来就该停在用户指的地方。
     *
     * @param r 选区（一对流位置）
     * @return true = 摆好了
     */
    bool setSelection(Range r);

    /**
     * 取消选区：anchor 收到 focus，焦点端不动 —— 光标留在原地。
     */
    void clearSelection();

    /**
     * 全选：文档首到文档末。
     *
     * 末端的口径是"最后一块的段内末端"，不是最后一块之后：段间那个段落标记不属于
     * 任何一段的内容，够不到它。
     *
     * @return true = 选好了
     */
    bool selectAll();

    /**
     * 整格选区：按"段号"选它所在的那一格（渲染整格铺满，删除逐格清空）。
     *
     * 收段号而不是流区间：一格的身份是它占的块区间（见 CaretResolver::CellBlocks）——
     * 空格的内容长度为 0，流区间退化成点，与下一格的第一段共用同一个流位置，分不开。
     * 段号来自命中（hitTestPt 的 out_cell_para），因为"哪一格"是几何问题 —— 引擎不提供
     * 按表号行号列号选格那一层：跨页拆行时行的逻辑号与画面对不上，而用户点的是画面。
     *
     * @param para 段号（块下标）
     * @return true = 选好了
     */
    bool selectCellAtPara(int para);

    /**
     * 选区是不是"单元格选区"。文字选区与它是两条独立的操作线，靠这个标记分开 ——
     * 不靠"选中了多少"去猜（见 CaretResolver::selectionIsCell）。
     *
     * @return true = 选的是整格
     */
    bool selectionIsCell() const { return caret_.selectionIsCell(); }

    /**
     * 选区是否吸附到了格边界上（跨格拖选、单击边距带、三击）。全选为假：它按整篇删。
     *
     * @return true = 端点已经吸附到格边界
     */
    bool hasCellRange() const { return caret_.hasCellRange(); }

    /**
     * 这一段内容所在那一格的内容区间（全局流位置，左闭右开）。不在表格里返回假。
     *
     * 空格的内容长度是 0（两端相等），所以它同时也是"这一格有没有字"的判据。
     * 嵌套表格取"包含这一段的最短区间" = 最内层那一格 —— 外层格把内层表的段也算进去，
     * 按最短挑才不会被外层截走。
     *
     * @param para 段号（块下标）
     * @param lo 写回区间起点（全局流位置）
     * @param hi 写回区间终点（不含）
     * @return true = 这一段在表格里
     */
    bool cellFlowRangeAt(int para, FlowPos* lo, FlowPos* hi) const;

    /**
     * 光标位置（派生）。段号 + 段内码点偏移，由光标的流位置投影出来 —— 不是独立状态。
     * 整格选区时它是焦点那一格的末尾（插入点不跟着鼠标在格内跑）。
     *
     * @return 光标位置（段号 + 段内码点偏移）
     */
    TextPos caret() const;

    /**
     * 光标当前的归属（-1 = 不在任何控件里；LineBox::kInlineOffsetOuter = 在所有控件之外）。
     * 由 focus 投影出来的段内流位置直接查，不是独立状态。
     *
     * @return 控件下标；-1 = 不在任何控件里
     */
    int caretFieldHint() const;

    /**
     * 光标的侧别（派生）：在它那个码点偏移上跨过了几枚边界括号（0 = 在所有括号之前）。
     * 与 caret() 合起来才是光标的完整位置 —— 同一个偏移上可能有好几个插入点
     * （控件之前 / 进入控件 / 内容末尾 / 控件之后），光有偏移表达不了。
     * 它是段内流位置与码点偏移的差，而两者都由 focus 投影 —— 这一路没有中间状态，
     * 所以不可能与光标位置不同步。这就是它取代 caret_side_ 与 caret_side_at_ 那两个
     * 字段的理由（那两个字面量天然可能失配，才需要失效标记兜）。
     *
     * @return 光标侧别（这一处跨过了几枚括号）
     */
    int caretSide() const;

    /**
     * 光标当前的行别：折行处贴上一行行尾（0）还是贴下一行行首（1）。
     *
     * 它只解决一件事：折行把一个位置落在两行之间。段内第 k 个位置既是上一行的末尾，
     * 也是下一行的开头，偏移与侧别都一样 —— 只按位置找行，找回来的必然是上一行，
     * 于是"点下一行的行首"变成"光标跳到上一行行尾"。行别是那个被丢掉的自由度。
     *
     * 它不是文档状态：任何不带行别的定位都会把它清成 0（贴上一行），与上下键的列位
     * 同一待遇（见 vertical_goal_valid_）。写完字之后光标本来就该贴在前一个字后面，
     * 而那个位置在上一行的末尾。
     *
     * @return 行别（0 = 贴上一行，大于 0 = 贴下一行）
     */
    int caretLineAffinity() const { return caret_.lineAffinity(); }

    /**
     * 光标是否落在竖排的段里（宿主据此把方向键按物理方向映射，见 keyToOp）。
     *
     * @return true = 光标所在段是竖排
     */
    bool caretVertical() const;

    /**
     * 光标所在段的列序是否反向（中日竖排为真：第一列在最右；蒙文为假：第一列在最左）。
     *
     * 方向键要用它：竖排下左右是换列，而"下一列在哪一边"由列序决定 ——
     * 中日竖排往左换列，蒙文往右换列。只按"是竖排吗"映射会把蒙文的左右按反。
     *
     * @return true = 列序反向（第一列在最右）
     */
    bool caretBlockReversed() const;

    /**
     * 按对外坐标（段, 段内码点偏移）写光标，侧别按"最内层含端点"投影一次。
     * 这是文本坐标投影到流位置的唯一约定 —— 只给"没有侧别信息"的入口用（宿主程序化定位、
     * 排版收口）；编辑路径一律走 setCaretAtFlow / setCaretRel，侧别由操作自己算出来。
     *
     * @param p 文本坐标
     */
    void setCaretByText(TextPos p);

    /**
     * 按"用户感知字符"（簇）移动。dcp > 0 向后、< 0 向前。
     * 返回 true 表示光标真的动了（没动就别重绘）。
     *
     * extend 为真时锚点不动：Shift+方向 是从锚点那一端继续扩展，而不是每次重新起头。
     * 锚点不需要"进入扩展态"这样一个额外状态 —— 第一次按 Shift 时它自然就是那时的
     * 光标位置（此前所有非扩展移动都把两端收到一起）。
     *
     * @param dcp 走几个字（负 = 向前）
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretBy(int dcp, bool extend = false);

    /**
     * 到行首/行尾，不是段首/段尾 —— 这才是 Home/End 该有的语义。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToLineStart(bool extend = false);

    /**
     * 到所在行的行尾（End 的语义，不是段尾）。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToLineEnd(bool extend = false);

    /**
     * 到文档首 / 文档末（Ctrl+Home / Ctrl+End）。
     *
     * 与行首行尾分开：段与行是两个概念，而"整篇的开头"还要再退过节点边界。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToDocStart(bool extend = false);

    /**
     * 到文档末（Ctrl+End）。落点用流位置给，段末那个位置不会翻到下一段去。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToDocEnd(bool extend = false);

    /**
     * 上下键：沿块轴走到相邻行，列位沿行内轴保持。
     *
     * dir < 0 向上、> 0 向下。跨短行时列位不丢（列位在第一次按时取自光标当前的行内
     * 坐标，之后连续按都沿用）。列位是会话状态不是文档状态，任何非扩展的定位都会清掉它
     * （见 setCaretRel）—— 不清的症状是"鼠标点去别处，再按上还跳回那一列"。
     *
     * @param dir 方向（负 = 向上 / 向左）
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretVertical(int dir, bool extend = false);

    /**
     * 光标闪不闪的开关。关掉之后导出图里没有光标，页像素本身不受影响。
     *
     * @param on true = 画光标，false = 不画
     */
    void showCaret(bool on);

    /**
     * 编辑入口：坐标是 (段, 段内码点偏移)，落地前吸附到字素簇边界。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @param utf8 要插入的文本
     * @return true = 插进去了
     */
    bool insertTextUtf8(int para, size_t offset, const std::string& utf8);

    /**
     * 插入并收尾（重排 + 重绘），与 typeTextAndRender 同一条路 —— 外部 API 的
     * insertText(pos, text) 用它：模板填充要"一次调用、一个撤销步、画面立刻更新"。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @param utf8 要插入的文本
     * @return true = 插进去了
     */
    bool insertTextAndRender(int para, size_t offset, const std::string& utf8);

    /**
     * 在指定位置插入一串码点。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @param cps 要插入的码点
     * @return true = 插进去了
     */
    bool insertCodepoints(int para, size_t offset, const std::vector<uint32_t>& cps);

    /**
     * 在指定位置插入一个码点。
     *
     * @param para 段号（块下标）
     * @param offset 段内码点偏移
     * @param cp 一个码点
     * @return true = 插进去了
     */
    bool insertCodepoint(int para, size_t offset, uint32_t cp);

    /**
     * 在光标处插字 / 删字（删除按簇，不是按码点）
     *
     * @param utf8 要插入的文本
     * @return true = 插进去了
     */
    bool typeTextUtf8(const std::string& utf8);

    /**
     * 退格：删光标左边一个字簇。段首时并进前一段。
     *
     * @return true = 删了
     */
    bool backspace();

    /**
     * 前删：删光标右边一个字簇。段末时并进下一段。
     *
     * @return true = 删了
     */
    bool deleteForward();

    /**
     * 跨段编辑。
     *
     * 回车在光标处把这一段拆成两段；新段整体继承原段的样式，只把 page_break_before
     * 清掉（段落分隔符不该附带一次翻页）。段首退格、段末 Delete 分别合并前后段 ——
     * 它们是 backspace() / deleteForward() 的自然延伸，不是另外的编辑命令。
     *
     * @return true = 拆了
     */
    bool splitParagraph();

    /**
     * 把这一段并进前一段（段首退格的延伸）。
     *
     * @return true = 并了
     */
    bool mergeWithPreviousParagraph();

    /**
     * 把下一段并进这一段（段末 Delete 的延伸）。
     *
     * @return true = 并了
     */
    bool mergeWithNextParagraph();

    /**
     * 编辑 + 重排 + 呈现的组合入口
     * 为什么要把三步合成一次调用：这三步之间没有任何需要宿主参与的信息。
     * 让宿主分别调三次，就是三次跨界 + 三次对象封送 —— 那才是 WASM 的减速带。
     * 组合在 C++ 里，宿主一次调用拿到"有没有变"和一组统计。
     *
     * @param utf8 要插入的文本
     * @return true = 插进去了
     */
    bool typeTextAndRender(const std::string& utf8);

    /**
     * 退格 + 重排 + 呈现。
     *
     * @return true = 删了
     */
    bool backspaceAndRender();

    /**
     * 前删 + 重排 + 呈现。
     *
     * @return true = 删了
     */
    bool deleteForwardAndRender();

    /**
     * 分段 + 重排 + 呈现。
     *
     * @return true = 拆了
     */
    bool splitParagraphAndRender();

    /**
     * 并进前一段 + 重排 + 呈现。
     *
     * @return true = 并了
     */
    bool mergeWithPreviousParagraphAndRender();

    /**
     * 并进下一段 + 重排 + 呈现。
     *
     * @return true = 并了
     */
    bool mergeWithNextParagraphAndRender();

    /**
     * 按簇移动 + 收尾重绘。
     *
     * @param dcp 走几个字（负 = 向前）
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretAndRender(int dcp, bool extend = false);

    /**
     * Home / End：到行首 / 行尾。
     *
     * 不用 moveCaret(±1e9) 实现：那样依赖 moveCaretBy 走不动时提前 break，且语义不同。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToLineStartAndRender(bool extend = false);

    /**
     * 到行尾 + 收尾重绘。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToLineEndAndRender(bool extend = false);

    /**
     * 到文档首 + 收尾重绘。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToDocStartAndRender(bool extend = false);

    /**
     * 到文档末 + 收尾重绘。
     *
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretToDocEndAndRender(bool extend = false);

    /**
     * 上下移动 + 收尾重绘。
     *
     * @param dir 方向（负 = 向上 / 向左）
     * @param extend true = 扩展选区（锚点不动）
     * @return true = 位置变了
     */
    bool moveCaretVerticalAndRender(int dir, bool extend = false);

    /**
     * 全选 + 收尾重绘。
     *
     * @return true = 选好了
     */
    bool selectAllAndRender();

    /**
     * 设选区 + 收尾重绘。
     *
     * @param r 选区（一对流位置）
     * @return true = 摆好了
     */
    bool setSelectionAndRender(Range r);

    /**
     * 选整格 + 收尾重绘。
     *
     * @param para 段号（块下标）
     * @return true = 选好了
     */
    bool selectCellAtParaAndRender(int para);

    /**
     * 控件集合。表单控件是文档流里的区间标注，不是矩形（模型见 core/doc/control.h）。
     *
     * 控件本身不参与断行/分页，参与排版的是它的括号：要显示括号的字段会在自己的边界
     *   落成一项行内对象（见 BlockSpec::objects），排版据此插入占位簇。于是挂控件、改
     *   控件范围不必重排（正文没变），而改括号的显示开关必须重排 —— 对象表变了。
     * 结构与绘制共用同一份落点：括号画在哪由占位簇的几何给出（owner_control 指回控件），
     *   不再两边各记一套。
     *
     * @return 控件集合（可写入口；其余地方走 setControlProp）
     */
    ControlSet& controls() { return controls_; }

    /**
     * 控件集合（只读）。
     *
     * @return 控件集合（只读）
     */
    const ControlSet& controls() const { return controls_; }

    /**
     * 换编辑模式。模式决定光标能落在哪、哪些命令可用，以及隐藏控件在流里可不可见，
     * 所以它一变就要重建流与排版。
     *
     * @param m 目标编辑模式
     */
    void setEditMode(EditMode m);

    /**
     * 当前编辑模式。
     *
     * @return 当前编辑模式
     */
    EditMode editMode() const { return mode_; }

    /**
     * 当前光标所在的最内层控件下标（-1 = 不在任何控件内）。
     * 由光标位置与侧别推出来（见 caretFieldHint）；推不出来时（光标落在所有控件之外）
     * 回落成"最内层含端点"—— 与编辑归属同一口径。
     *
     * 这条回落不只是为了好看：光标落在段落末尾、或刚跳完字段时，光标的"侧别"推不出
     * 归属，而编辑命令与面板都要一个确定的字段下标（见 edit_command 的归属口径）。
     *
     * @return 控件下标；-1 = 不在任何控件里
     */
    int focusedControlIndex() const {
        const int hint = caretFieldHint();
        if (hint >= 0) return hint;
        return controls_.empty() ? -1 : controls_.innermostAt(caret(), true);
    }

    /**
     * 占位簇上的 owner 句柄 → 对外控件下标。
     *
     * owner 是控件的节点 id（第五刀：序号会在插入之后整体平移，而节点 id 是身份）。
     * 对外（宿主、探针、既有判据）的口径始终是下标，所以"拿下标去和占位簇的
     * owner_control 比"的地方都要先过这里 —— 宿主报"这枚括号归谁"正是其中一处。
     *
     * @param owner 产生端句柄（持树时是控件节点号）
     * @return 控件下标；找不到为 -1
     */
    int controlIndexOfOwner(int owner) const {
        const auto it = owner_index_.find(owner);
        return it == owner_index_.end() ? -1 : it->second;
    }

    /**
     * 反方向：对外控件下标 → 块流对象表里的 owner 句柄。
     *
     * caretRectFor 拿它把光标归属（下标）换成句柄再去问排版 —— 比反了的表现是"共享边界上
     * 光标画到另一侧去"，位置差一格，只有像素级判据看得出来。
     * 负数（没有归属、贴外侧哨兵）原样传下去：它们在排版侧是同一种含义的另一种取值。
     *
     * @param control_index 控件下标
     * @return owner 句柄（节点号）；越界为 -1
     */
    int slotOwnerOf(int control_index) const {
        if (control_index < 0) return control_index;
        if (control_index >= static_cast<int>(control_node_ids_.size())) return -1;
        return static_cast<int>(control_node_ids_[static_cast<size_t>(control_index)]);
    }

    /**
     * 要不要在排版里给控件边界留占位（= 显示括号）。
     *
     * 只有两档：全部或没有。只读模式不占 —— 它是呈现与打印态，括号落进版面就印出来了；
     * 没有控件时自然也没有占位（空文档与"没有表单"逐字节一致）。
     *
     * @return true = 排版里要给控件边界留占位
     */
    bool bracketsOn() const {
        return mode_ != EditMode::ReadOnly && cfg_.show_brackets && !controls_.empty();
    }

    /**
     * 「括号占位计划」的指纹：变了就必须重排（占位进文档流，集合一变排版就变）。
     * 编码：-1 = 没有占位；0 = 全部控件都占位。
     *
     * 用 int 而不是 bool 是因为调用点全是"记一下、操作完再比一下"（十余处），
     * 比较值的形状跟着改没有收益。占位集合与光标无关 —— 光标跨字段移动不重排，
     * 这是括号常显换来的。
     *
     * @return 占位计划的指纹（-1 = 没有占位，0 = 全部控件都占位）
     */
    int bracketPlanId() const { return bracketsOn() ? 0 : -1; }

    /**
     * 把控件边界写进块流的行内对象表 —— 元素流在引擎侧的落点，也是排版层唯一的
     * 括号来源（layoutDocument 不再收控件集合）。幂等，每次 relayout 之前调用。
     *
     * 产出端暂时还是引擎：只有它知道编辑模式与显示开关，也就知道哪些字段要
     * 显示括号。等编译层能把内容控件直接编成行内对象，这里就退化成一次过滤。
     */
    void syncInlineObjects();

    /**
     * 累计被编辑策略拒掉的次数。
     *
     * 用 getter 直读成员，不走 EditStats：被拒时不触发 repaint，而 EditStats 要等下一次
     * 呈现才刷新，走它会差一拍。
     *
     * @return 累计被策略拒掉的编辑次数
     */
    long long blockedEdits() const { return blocked_edits_; }

    /**
     * 撤销与重做：历史由会话持有，每格是一份旧修订。
     *
     * 撤销不是"反向应用逆操作"，而是把旧修订拿回来 —— 修订不可变，所以拿回来的是同一份内容，
     * 不是重放出来的近似。两份修订一减就得到损伤区间，投影据此只重算那一段。
     * 修订不记光标，所以撤销栈旁边另有一份选区快照、与它同步裁剪（见 recordSelection）。
     * 没装载过文档时没有会话，四个入口一律返回 false，不假装能做。
     *
     * @return true = 能撤销
     */
    bool canUndo() const { return session_ && session_->canUndo(); }

    /**
     * 有没有可重做的一步。
     *
     * @return true = 能重做
     */
    bool canRedo() const { return session_ && session_->canRedo(); }

    /**
     * 撤销一步：把上一份修订拿回来（不是反向应用逆操作）。
     *
     * @return true = 撤了一步
     */
    bool undo();

    /**
     * 重做一步：把被撤销的那份修订拿回来。
     *
     * @return true = 重做了一步
     */
    bool redo();

    /**
     * 撤销 + 重排 + 呈现。
     *
     * @return true = 撤了一步
     */
    bool undoAndRender();

    /**
     * 重做 + 重排 + 呈现。
     *
     * @return true = 重做了一步
     */
    bool redoAndRender();

    /**
     * 文档坐标（磅）落在哪个控件上（取最内层）。-1 = 没有。
     * 它走的是 hitTestPt 的同一份几何，所以"视觉上在框里"与
     * "逻辑上在控件里"永远一致。
     *
     * @param x_pt_doc 文档坐标 x（磅）
     * @param y_pt_doc 文档坐标 y（磅）
     * @return 控件下标；-1 = 没有
     */
    int controlAtPt(float x_pt_doc, float y_pt_doc) const;

    /**
     * 页面坐标下的一个矩形（页号 + 页面坐标磅矩形）。
     *
     * 定义已提到 utils/types.h：光标与选区那一层（CaretResolver）也要用它，
     * 而那一层不该反过来依赖引擎头。这里留一个同名别名，既有调用点不必动。
     */
    using PageRect = heditor::PageRect;
    /**
     * 控件矩形的原名。改名只是把"页号 + 页面矩形"从控件里提出来，调用点不必动。
     */
    using ControlRect = PageRect;

    /**
     * 一个控件覆盖的页面矩形（可能跨页拆分）。
     *
     * @param index 控件下标
     * @return 页面矩形（页号 + 页面坐标磅矩形）
     */
    std::vector<ControlRect> controlRects(int index) const;

    /**
     * 选区 → 页面坐标矩形（跨行拆、跨页分段）。空选区是空的 —— 没有选区就没有要画的。
     *
     * 只返回落在页范围 [first_page, first_page + count) 里的那部分；count <= 0 表示不限页。
     *
     * 为什么要能限页：整篇选中的矩形条数是"文档行数"这个量级（2000 段 ≈ 4000 条），
     *   而呈现与状态推送每拍都要过一遍。不限页时，全选之后每一拍都要造 4000 条矩形、
     *   把它们全刻进擦除清单、再逐条混合一遍 —— 其中九成以上落在当前窗口之外，
     *   画不到也擦不着（量到的是单线程宿主 present 从 3.6 ms 涨到 9.8 ms，
     *   Worker 宿主每拍状态消息里那一份 197 KB）。
     *   限页之后这两笔都只随"看得见的行数"走，与选区多大无关。
     *
     * @param first_page 窗口首页
     * @param count 这一批要保证新鲜的页数
     * @return 落在窗口内的选区矩形
     */
    std::vector<PageRect> selectionRectsIn(int first_page, int count) const;

    /**
     * 整篇的选区矩形。判据与审计用它（要看"选区到底盖了多少行"），
     * 呈现与状态推送不要用它 —— 用 selectionRectsIn。
     *
     * @return 整篇的选区矩形（判据与审计用）
     */
    std::vector<PageRect> selectionRects() const;

    /**
     * 当前视口窗口内的选区矩形。宿主取证入口用它，口径与"这一帧画了什么"一致。
     * 视口没钉过（vp_count_ <= 0）时限页失效，退化成整篇。
     *
     * @return 当前视口窗口内的选区矩形
     */
    std::vector<PageRect> selectionRectsVisible() const;

    /**
     * 删除一个控件。keep_content 为真时拆壳留内容（那段字符变成普通正文），假时连内容一起删。
     *
     * 策略走 canDoStructureOp（structureLock 默认锁住）：表单模式默认拒绝、文档模式放行 ——
     * 增删字段是设计期的事。删完段数可能变，所以走全量重编译，不进增量那条路。
     *
     * @param index        控件下标（对外一律用下标，节点 id 不外露）
     * @param keep_content 真 = 保留内容，假 = 连内容一起删
     * @param caret_before 删除之前光标在哪（流位置），撤销恢复的就是它。键盘那一路必须传；不传
     *                     （-1）时按"选中整个控件"记，撤销会把控件重新选中、光标落到闭括号之后。
     * @return 是否真的改了文档
     */
    bool removeControl(int index, bool keep_content, FlowPos caret_before = -1);

    /**
     * 在当前段之后插一张空表格，光标落进第一格。
     *
     * 走 InsertNode 那条结构步骤，所以撤销、位置映射与增量排版全部复用现成机制。
     * 只在文档模式放行：插入表格是设计动作。
     *
     * @param rows 行数
     * @param cols 每行的格数
     * @param header true = 首行是标题行
     * @return true = 插进去了
     */
    bool insertTable(int rows, int cols, bool header);

    /**
     * 插入表格 + 重排 + 呈现（理由同上面那组 AndRender）。
     *
     * @param rows 行数
     * @param cols 每行的格数
     * @param header true = 首行是标题行
     * @return true = 插进去了
     */
    bool insertTableAndRender(int rows, int cols, bool header);

    /**
     * 在光标处插一个内容控件（表单字段），光标落进它的内容末尾。
     *
     * @param spec 控件记录
     * @param label 控件内容
     * @return true = 插进去了
     */
    bool insertControl(const ControlSpec& spec, const std::string& label);

    /**
     * 插入控件 + 重排 + 呈现。
     *
     * @param spec 控件记录
     * @param label 控件内容
     * @return true = 插进去了
     */
    bool insertControlAndRender(const ControlSpec& spec, const std::string& label);

    /**
     * 删除控件 + 重排 + 呈现的组合入口（理由同上面那组 AndRender）。
     * 宿主侧一次调用就收尾，不必自己记得再排一次 —— 漏掉那一步的症状是
     * "控件没了但括号还在画布上"。
     *
     * @param index 控件下标
     * @param keep_content true = 拆壳留内容
     * @return true = 删掉了
     */
    bool removeControlAndRender(int index, bool keep_content);

    /**
     * 改控件的一个运行时属性（hidden / readonly / required / structureLock）。
     * 与 removeControl 同一套策略门；设计期属性（类型、格式、选项）不走这里。
     *
     * @param index 控件下标
     * @param prop 要改的运行时属性
     * @param value 新值
     * @return true = 改成了（幂等时也成立）
     */
    bool setControlProp(int index, ControlProp prop, bool value);

    /**
     * 改控件属性 + 重排 + 呈现的组合入口。hidden 会改呈现态的流结构（整棵摘除），
     * 所以这一次重排不能省。幂等（值没变）时它和 setControlProp 一样返回 true，
     * 但不会白跑一次重排与重绘。
     *
     * @param index 控件下标
     * @param prop 要改的运行时属性
     * @param value 新值
     * @return true = 改成了
     */
    bool setControlPropAndRender(int index, ControlProp prop, bool value);

    /**
     * Tab / Shift+Tab：把光标移到下一个（上一个）可编辑控件的起点。
     * 到头会绕回第一个 —— 表单里 Tab 就该是循环的。
     *
     * @param forward true = 下一个控件，false = 上一个
     * @return true = 落到了一个控件上
     */
    bool focusControl(bool forward);

    /**
     * Tab / Shift+Tab 的策略版：走模型层的能力表（edit_capability 的 tabFrom），
     * 到最后一个可编辑控件就放行（返回 false，不消费该 Tab），不绕回第一个。
     *
     * 与 focusControl 的差别不只是循环与否：focusControl 认的是控件集合的区间起点，
     * 而 tabFrom 认的是"内容起点落在可编辑集合内"这个谓词 —— 于是"只读控件里嵌的
     * 可编辑控件"不会成为落点，反之亦然，两处口径不会各说各话。
     *
     * @param backward true = 反向（Shift+Tab）
     * @return true = 光标动了（到头时为 false，不消费这个 Tab）
     */
    bool tabCaret(bool backward);

    /**
     * 同上，带收尾重绘。
     *
     * @param backward true = 反向（Shift+Tab）
     * @return true = 光标动了
     */
    bool tabCaretAndRender(bool backward);

    /**
     * 跨字段移动光标必须用这一个（Tab / Shift+Tab 走的就是它）。
     * focusControl 只改状态、不重绘，而宿主随后会拿上一次呈现的脏矩形去上屏，
     * 画布于是成了新旧混合体；收尾重绘不可省，理由见 engine.cpp 的实现。
     *
     * @param forward true = 下一个控件，false = 上一个
     * @return true = 落到了一个控件上
     */
    bool focusControlAndRender(bool forward);

    /**
     * 帧自审：把当前这一帧与"整帧重画一遍"的结果逐字节比，返回
     * {"diff":N,"frameW":..,"frameH":..,"box":[x0,y0,x1,y1]}。
     * diff > 0 就等于"增量路径漏掉了什么"（典型：某个旧表单标记没擦干净）。
     * 跑完顺便把这一帧修成真值。理由见 engine.cpp 的实现。
     *
     * @return {"diff":N,"frameW":..,"frameH":..,"box":[x0,y0,x1,y1]}
     */
    std::string auditFrameJson();

    /**
     * 内容自审：不含瞬态的窗口拼装 == 从零重来吗？返回
     * {"diff":N,"frameW":..,"frameH":..,"box":[x0,y0,x1,y1]}。
     *
     * 与 auditFrameJson 分工：两条都差 ⇒ 内容画错了；只有帧那条差 ⇒ 瞬态叠错了。
     *   它比的是"页位图按窗口拼起来"的那一层：清页缓存、重画、逐字节比。
     *   拼装本身错（页落到错的槽、窗口顺序反了）只有这一条照得出来 —— 帧自审读的
     *   是同一份拼装，两边会一起错。
     * 跑完顺便把画面修成真值（与 auditFrameJson 同一口径：自审不掩盖问题）。
     *
     * @return 同上形状的 JSON（不含瞬态的那一层）
     */
    std::string auditBaseJson();

    /**
     * 页自审：把第 @p page 页从零光栅一遍，与缓存里那一份逐字节比，返回
     * {"diff":N,"w":..,"h":..,"box":[x0,y0,x1,y1],"cached":0|1}。
     *
     * 为什么要有这一条：页缓存一旦写错，"增量帧 == 整帧重画"照样可能全绿 ——
     *   两条路径读的是同一份缓存。这里的参照物是"绕过缓存重新光栅"，
     *   与 auditLayout 用"不用缓存的全量重排"当参照是同一个手法。
     * 页不在缓存里（被淘汰）时返回 diff = -1，不做假绿。
     *
     * @param page 页号
     * @return {"diff":N,"w":..,"h":..,"box":[..],"cached":0|1}；页不在缓存里时 diff = -1
     */
    std::string auditPageJson(int page);

    /**
     * 排版自审：用"不用缓存的全量重排"做一份对照，与当前排版结果逐字段比对，
     * 返回不符项数（0 = 一致）。
     * 理由与 auditFrameJson 相同：增量的正确性由"与全量一致"定义，
     * 而不是由"看起来对"定义。逐字段而不是只比行数：增量最典型的错法是
     * "少排了一段"或"前缀多留了一段"，两者都只差几行。
     *
     * @return 不符项数（0 = 一致）
     */
    int auditLayout();

    /**
     * 表格结构自审：块与格这两份记录是否互相撑得住，返回不符项数（0 = 一致）。
     *
     * 为什么不并进 auditLayout：那一条比的是"增量排版与全量排版"，两边吃的是同一份
     *   blocks_ 与 tables_，结构本身坏了它照样全绿 —— 一格连一个块都不剩时，两边都排不出
     *   那一格的行盒，逐字段比对完全一致。而"格没有块""块没有结束标记"这类损坏恰恰是表格
     *   编辑最典型的失败方式：后果是那一格连行盒都没有，光标点不进去，画面上只是少了几行字。
     *
     * 查六条：格至少一个块；格的块区间在表内且互不重叠；格里的块都有结束标记；块的
     *   cell_path 与格给出的区间一致（两份记录互为对照）；格行列跨度不越界；表句柄在块
     *   路径里指同一张表。
     *
     * @return 不符项数（0 = 一致）
     */
    int auditTables() const;

    /**
     * 同上的 JSON 版，供宿主与探针取证（含各类不符的计数与首个位置）。
     *
     * @return 各项计数与首个出问题的格
     */
    std::string auditTablesJson() const;

    /**
     * 同上的 JSON 版，供宿主取证（含两侧规模、首个不符位置与两个耗时）。
     *
     * @return 两侧规模、首个不符位置与两个耗时
     */
    std::string auditLayoutJson();

    /**
     * 字形页索引自审：逐页验"二分出来的区间"与"全篇筛一遍"是否给出同一批字形。
     *
     * 为什么要单开一条：绘制侧改成按页二分之后，auditBaseJson 变得盖不住它了 ——
     *   那条是"强制从零重画再与现在比"，而重画走的是同一条二分路径，两边错得一样，
     *   逐字节比照样全绿（这正是本条存在的理由：同一个代码路径当判据必然恒绿）。
     * 判据取双向：区间内每个字形的 page 必须等于该页（不漏），并且全篇里属于该页的
     *   字形数必须等于区间长度（不多不少）—— 只查前者会漏掉"区间里混进别页"，
     *   只查后者会漏掉"区间错位但长度对"。
     * 没有比对对象时返回空结果（diff 0、checked 0），不做假绿。
     *
     * @return 逐页的比对结果（diff 与 checked）
     */
    std::string auditGlyphPagesJson() const;

    /**
     * 编译自审：拿"整树全量编译"做一份对照，与当前产物逐字段比对，返回不符项数。
     *
     * 与 auditLayout 同一个理由：编译这一层的增量的正确性由"与全量一致"定义。
     * 只比段数看不出来 —— 增量最典型的错法是"某一段的产物还是旧的"，
     * 而段数、行数全都对得上。没装载过文档时返回 0：那时没有可比的东西。
     *
     * @return 不符项数（0 = 一致；没装载过时为 0）
     */
    int auditCompile() const;

    /**
     * 同上的 JSON 版：给出本轮实际编译耗时与一次全量编译耗时，两者同源。
     * 这是"增量到底省了多少"的唯一证据 —— 没有分母的耗时数说明不了任何事。
     *
     * @return 本轮编译耗时与一次全量编译的耗时
     */
    std::string auditCompileJson();

    /**
     * 编辑类入口的统一收尾：先让光标回到可见相位，再重绘。
     *
     * 闪烁可能正停在隐藏半拍，此时 editStats.caretVisible 为 false，宿主会走补救路径
     * 再 present 一次。把"编辑后光标可见"这条不变量落在引擎里，该路径就不会被触发。
     */
    void repaintAfterEdit();

    /**
     * Tab 缩进：在光标处插入 tab_spaces 个空格，走与敲字相同的策略与重排路径。
     * Shift+Tab 反缩进：删掉光标前最多 tab_spaces 个连续空格。
     * 不做"跳到下一个字段"：Tab 在正文编辑器里表示缩进，换字段用鼠标点。
     *
     * @return true = 插了
     */
    bool indentAndRender();

    /**
     * 反缩进 + 重排 + 呈现。
     *
     * @return true = 删了
     */
    bool outdentAndRender();

    /**
     * 在光标处插入 tab_spaces 个空格（缩进）。
     *
     * @return true = 插了
     */
    bool indent();

    /**
     * 删掉光标前最多 tab_spaces 个连续空格（反缩进）。
     *
     * @return true = 删了
     */
    bool outdent();

    /**
     * 诊断：把模式、控件树、每个控件的区间与矩形块数摊平。
     *
     * @return 可读文本
     */
    std::string describeControls() const;

    /**
     * 几何：同一份行盒几何的正反查
     * 正查：(段, 偏移) → 页面坐标下的光标矩形（磅）
     *
     * @param out 写回值
     * @return true = 找到了行盒
     */
    bool caretRectPt(Rect* out) const;

    /**
     * 反查：文档坐标（磅）→ (段, 偏移) + 侧别 + 行别；点落在页间空隙时返回 false。
     * 文档坐标以第 0 页顶边为原点 —— 与"窗口开在哪一页"无关，也与画布怎么摆无关。
     * out_side 是落点侧别（同一偏移上可能压着几枚括号），out_owner 是它的投影，只给人看。
     * out_line_aff 是折行处的行别：点下一行行首时大于 0，不带回去会画到上一行行尾。
     * out_cell_para：落在格子的边距带上时给出这一格的段号（否则 -1）。
     *   边距带 = 格边框到文字左沿之间那一条：点这里选整格，点字上是文字光标。
     *   空格没有文字可挡，整格都是边距带 —— 点空格的任何位置都选中这一格。
     *   给段号而不是流区间：一格的身份是它占的块区间，空格的流区间退化成一个点，
     *   与下一格的第一段共用同一个流位置（见 CaretResolver::CellBlocks）。
     *
     * @param x_pt_doc 文档坐标 x（磅）
     * @param y_pt_doc 文档坐标 y（磅）
     * @param out 写回值
     * @param out_owner 写回落点归属（控件下标）
     * @param out_side 写回落点侧别
     * @param out_line_aff 写回行别
     * @param out_cell_para 写回这一格的段号（不在格内为 -1）
     * @param clamp 落在页外或页间空隙时是否夹到最近的页边（拖选那一路要，单击那一路不要）
     * @return true = 点落在正文上（页间空隙且不夹时为 false）
     */
    bool hitTestPt(float x_pt_doc, float y_pt_doc, TextPos* out, int* out_owner = nullptr,
                   int* out_side = nullptr, int* out_line_aff = nullptr,
                   int* out_cell_para = nullptr, bool clamp = false) const;

    /**
     * 按点选整格（三击的入口）：点在格里的任何位置都选中这一格。
     *
     * 单击的"整格"只发生在格左侧的边距带上（见 hitTestPt 的 out_cell_para）—— 那是一条
     * 独立的操作线；三击则是用户明确说"我要这一格"，点字中间也算。点不在表格里返回假。
     *
     * @param x_pt_doc 文档坐标 x（磅）
     * @param y_pt_doc 文档坐标 y（磅）
     * @return true = 选好了（点不在表格里为 false）
     */
    bool selectCellAtPt(float x_pt_doc, float y_pt_doc);

    /**
     * 窗口导出图：把窗口内各页的"内容 + 瞬态"按窗口顺序拼成一张，供宿主 / 探针 / 单测读。
     *
     * 它不是渲染目标 —— 渲染只写页位图（见 PageBitmap），这里是一次读请求时现场拼的
     *   派生视图（第一次读时拼一次，下一次 present 作废重拼）。所以它没有槽、没有增量，
     *   也不可能与页位图分叉：内容全部来自页位图，瞬态全部来自同一份矩形。
     * 保留它是为了让"扫一遍画面"这类判据（像素断言、markCheck、inkRows）不必认识页。
     *
     * @return 窗口导出图的像素（第一次读时现场拼）
     */
    const uint8_t* frame() const;

    /**
     * 窗口的横向像素：与页同宽（页在窗口里只纵向排列）。
     *
     * @return 窗口的横向像素（与页同宽）
     */
    int frameWidth() const { return pagePxWidth(); }

    /**
     * 窗口的纵向像素：窗口页数 x 步距（末页之后那一格页间隙也留着，导出图因此整格对齐）。
     *
     * @return 窗口的纵向像素（窗口页数 x 步距）
     */
    int frameHeight() const;

    /**
     * 窗口导出图的大小（字节）。
     *
     * 只做乘法，不拼图：调用方（statsJson 每份状态都带一格）问的是"多大"，
     *   不是"给我像素"。要像素走 frame()，那一条才真的按需拼一次。
     *
     * @return 窗口导出图的大小（字节）
     */
    size_t frameBytes() const;

    /**
     * 窗口导出图拼一次（见 frame 的说明）。返回它的字节数。
     * 只在导出图被 presentInternal 作废之后才真的重拼 —— 拼的是页位图的副本。
     *
     * @return 窗口导出图的字节数（必要时重拼一次）
     */
    size_t ensureFrameExport() const;

    /**
     * 把窗口内各页拼进 @p out。@p with_transient 为假时只拼内容（审计用）。
     *
     * @param with_transient true = 连光标与选区一起拼
     * @param out 拼到这里
     */
    void assembleWindow(bool with_transient, Bitmap* out) const;

    /**
     * 第 page 页"画到画布上应该是什么"的像素（页内坐标，RGBA），零拷贝视图。
     *
     * 内容（页位图）是纯的，光标与选区是瞬态 —— 后者不写进页位图，而是在这里现场合成：
     *   把页位图拷进单页导出图，再叠上这一页的瞬态矩形。于是"擦旧光标"不需要记账，
     *   下一次导出从干净的页位图重来，可写的只有页位图这一份真相。
     * 页上没有瞬态时直接给页位图本身（省掉一页 3.5 MB 的拷贝）。只在"这一页在缓存里、
     *   且像素还作数"时给得出来；刻意不在内部补光栅 —— 那会把字形账算到一次读命令头上。
     *
     * @param page 页号
     * @return 这一页的字节数；拿不出来时为 0
     */
    size_t pageBytes(int page) const;

    /**
     * 与 pageBytes 同一份像素的指针（见那一条的说明）。
     *
     * @param page 页号
     * @return RGBA 像素；拿不出来时为 nullptr
     */
    const uint8_t* pagePixels(int page) const;

    /**
     * 自上次取走以来写过这一页的矩形（页内像素坐标）。读后清零。
     *
     * 口径是"这一页上画出来的东西真的变了的地方"：内容重光栅（整页或行带）+ 瞬态
     *   （光标 / 选区）的新旧矩形。窗口平移不算 —— 页像素一个字节都没动，只是落点变了，
     *   把它算进来会让"换窗"退化成整窗重传。
     *
     * @param page 页号
     * @return 自上次取走以来写过这一页的矩形（读后清零）
     */
    std::vector<Rect> takePageDirtyRects(int page);

    /**
     * takePageDirtyRects 的 JSON 版（宿主与探针取证用）。
     *
     * @param page 页号
     * @return 矩形数组的 JSON
     */
    std::string takePageDirtyRectsJson(int page);

    /**
     * 有累积脏矩形的页号（升序）。读后不清，清零发生在 takePageDirtyRects。
     *
     * 与 dirtyPages() 的区别：那一份是"这一拍写过哪几页"，这一份是"还欠哪几页没贴"。
     *   矩形是累积的 —— 某一拍的 present 可能根本没被宿主消费（窗口平移、页面不可见），
     *   那些矩形会留到下一拍。按本拍清单贴就会永久漏掉那几块，
     *   症状是屏幕上留几十像素的旧光标。
     *
     * @return 还欠着没贴的页号（升序，读后不清）
     */
    std::string pageDirtyPagesJson() const;

    /**
     * 内容底边（窗口像素）：最后一行墨迹的底，再加一个下边距。
     *
     * 连页模式下宿主拿它当滚动高度 —— 分页下滚动高度是"页数 x 步距"（页是实在的），
     *   连页下没有页可数，只有内容。分页下这个值同样成立（页号 x 条带高 + 行底），
     *   只是没人用它。
     *
     * @return 内容底边（窗口像素）
     */
    int contentBottomPx() const;

    /**
     * 排版与呈现的账（上一次 relayout / present 的结果）。
     *
     * @return 排版与呈现的账
     */
    const EngineStats& stats() const { return stats_; }

    /**
     * 编辑那一侧的账（文档规模、重排段数、上屏面积、光标读数）。
     *
     * @return 编辑那一侧的账
     */
    const EditStats& editStats() const { return edit_stats_; }

    /**
     * 排版产物（行盒、字形、每段切片）。审计与判据读它。
     *
     * @return 排版产物
     */
    const LayoutResult& layout() const { return layout_; }

    /**
     * 字体库（可写）：装载字体走这一条。
     *
     * @return 字体库（可写）
     */
    FontLibrary& fonts() { return fonts_; }

    /**
     * 只读视图。保存要用它：saveDocument() const 里得把字体下标翻成文件名，而那份查询
     * 不改库里的任何东西。
     *
     * @return 字体库（只读）
     */
    const FontLibrary& fonts() const { return fonts_; }

    /**
     * 诊断用：把一串文本的整形结果摊成可读文本（字形 id / 推进 / cluster），
     * 让"整形到底有没有发生"这件事可被断言，而不是只看能不能编译。
     *
     * @param font_index 字体索引
     * @param size_pt 字号（磅）
     * @param text 要整形的文本
     * @param wm 书写模式
     * @param language 语言标签（HarfBuzz 的 language）
     * @return 可读文本（字形 id / 推进 / cluster）
     */
    std::string describeShaping(int font_index, float size_pt, const std::string& text,
                                const WritingMode& wm, const char* language);

    /**
     * 诊断用：把当前光标的行盒几何、簇表、命中结果摊成可读文本。
     *
     * @return 可读文本
     */
    std::string describeCaret() const;

    /**
     * 页坐标 -> 设备像素坐标
     *
     * @param pt 磅值
     * @return 设备像素
     */
    float ptToPx(float pt) const { return pt * cfg_.zoom; }

    /**
     * 全文码点总数（不含块间分隔）
     *
     * @return 全文码点数
     */
    size_t documentCodepoints() const;

    /**
     * 落点判定的唯一入口：一次编辑能不能落在这里 / 能不能删掉这个区间。
     *
     * 六个编辑入口（插入、退格、前删、分段、合并、反缩进）全部走它，所以"判定与动作
     * 一致"是结构上的事实，不是一条要另外维护的约定。对外也开放：宿主拿它做禁用态提示，
     * 判据拿它把"能不能编辑"与"光标能不能到"分开 —— 表单模式下 setCaret 会把光标
     * 投影进最近的字段，"控件之外"那个位置根本到不了，只有直接问它才问得到。
     *
     * 落点的侧别由位置自己推（见 sideForPos）：光标那一处用光标自己的侧别，别处按
     * "最内层含端点"归一次。不另收归属提示 —— 那是"控件下标"这套说话方式，只有
     * 区间判定需要它，而落点判定已经改说侧别了。
     *
     * @param p 文本坐标
     * @return 允许不允许，不允许时带原因
     */
    rev::EditCheck policyAt(TextPos p) const;

    /**
     * 删除区间 [a, b) 的判定。跨段删除一律拒（档 1 的删除不跨段，段落标记的删除走合并）。
     *
     * @param a 区间起点（含）
     * @param b 区间终点（不含）
     * @return 允许不允许，不允许时带原因
     */
    rev::EditCheck policyErase(TextPos a, TextPos b) const;

private:

    /**
     * 一份修订装上之后的公共收尾：换会话、复位光标、清页缓存、投影、整篇标脏。
     * 装载（模型树）与打开（JSON）两条路都走它 —— 两条路的收尾必须是同一份。
     */
    void installDocument(rev::Doc doc);

    /**
     * 按当前修订重算产物：投影出块流与控件跨度，再把流索引、位置与控件视图带上。
     * 隐藏控件的可见性口径按当前编辑模式取（见 flowVisibility）。
     */
    void rebuildFromTree(const rev::BlockDamage* dmg = nullptr);

    /**
     * 重建的收尾：流索引、块位置的前缀和、控件视图。
     *
     * 控件跨度由投影给（它走树时记下的），不在这里重扫对象表 —— 见 exportControls。
     */
    void finishRebuildFromTree(FlowVisibility vis, const std::vector<rev::ControlSpan>& controls);

    /**
     * 由 block_extents_ 累加出 block_flow_ / block_flow_span_。
     *
     * 只是把局部量加起来，不做任何查询、也没有第二种说法要调和 —— 表格的格边界表现为
     * 那一个 gap 等于 2，而不是"两个算法给出不同答案时取较大值"。
     */
    void buildBlockFlow();

    /**
     * 隐藏控件的可见性口径。编译与流索引必须同源（非编辑模式下隐藏的子树整棵摘除），
     * 所以只在这里取一次，两个入口都用它。
     */
    FlowVisibility flowVisibility() const;

    /**
     * 命令层的出口（EditHost）：把一批步骤落到修订上，再把产物按新修订重算一次。
     *
     * 重编译是必须的而不是图省事：块流、流索引、块起点、来源身份、控件集合全是修订的派生量，
     * 少重建任何一个，下一轮就会拿旧修订的位置去对新修订的块。失败时状态一个字节都不动 ——
     * 会话层保证"被拒的编辑不占撤销栈"，这里再保证产物不跟着半途改。
     */
    bool commitSteps(std::vector<Step> steps, int dirty_para, Range selection_before,
                     Range selection_after) override;

    /**
     * 撤销上限（会话要的那个数）。配置里的 0 是"不限制"，与"不留历史"不是一回事。
     */
    int undoLimit() const;

    /**
     * 记下这一批步骤对应的两份选区，并与撤销栈同步裁剪。
     */
    void recordSelection(Range before, Range after);

    /**
     * 写出去时的字体身份。保存文档与复制片段共用一份 —— 两处各写一遍，等于让"同一张脸"
     * 有了两种说法，而两边看起来都成功。
     * @return 字体下标到字体身份的回调
     */
    rev::SerializeOptions fontWriteOptions() const;

    /**
     * 读进来时的字体反查，与 fontWriteOptions 同一个顺序（PostScript 名 → 族名 → 文件名）。
     * @return 字体身份到本机下标的回调
     */
    rev::DeserializeOptions fontReadOptions() const;

    /**
     * 从投影给出的跨度建控件集合。
     *
     * 跨度是投影走树时记下的（起止块号与段内码点，见 rev::ControlSpan），业务标识与锁定态
     * 就在节点载荷里 —— 所以这一层不再扫块流的对象表，也不再对每个控件 findNode 一次。
     * 两边各算一遍的代价不只是慢：区间一旦分叉，界面上只表现为"某个字段锁不住"或"括号画在
     * 别处"，而两份产物各自看都"对"。
     */
    void exportControls(const std::vector<rev::ControlSpan>& spans);

    /**
     * 选区矩形（页内像素坐标，与 caretRectPxOf 同口径）。只取落在页范围
     * 页范围 [first_page, first_page + count) 里的那部分；count <= 0 表示不限页。
     * 页内坐标 + 页号是这一层唯一的口径：窗口位置由宿主按 (页号 x 步距) 加回去，
     *   引擎不再产生任何"窗口内偏移"（那种数一旦跨层就会与窗口首页的取值挂钩）。
     */
    std::vector<PageRect> selectionRectsPxIn(int first_page, int count) const;

    /**
     * 整篇的选区矩形（页内像素坐标）。判据/取证用它，呈现不要用 —— 用 selectionRectsPxIn。
     */
    std::vector<PageRect> selectionRectsPx() const;

    /**
     * 把"文本坐标 + 侧别"投影成 rel 并摆好（含重排）。side < 0 = 按"最内层含端点"投影。
     * extend 为真时不动锚点（Shift+Home/End 用）。line_aff 见 caretLineAffinity。
     */
    void projectCaret(int para, size_t offset, int side, bool extend = false, int line_aff = 0);

    /**
     * 光标矩形（页内像素坐标，出参 @p page 给它是哪一页）。caret_visible 为 false
     * 或找不到行盒时返回零矩形、@p page = -1。
     */
    Rect caretRectPx(int* page) const;

    /**
     * 光标所在页（绝对页号；-1 = 找不到行盒）。
     * 窗口化之后这是必需的一个数：宿主拿它决定窗口该锚在哪、输入法框摆在哪。
     */
    int caretPage() const { return caret_.caretPage(); }

    /**
     * 统一呈现入口：按窗口逐页光栅 + 记瞬态矩形，不产生整窗帧。
     */
    void presentInternal(int first_page, int count);

    /**
     * 一页上叠的瞬态：选区矩形（可以有多条）+ 光标矩形（最多一条）。
     *
     * 分成两份而不是混在一个数组里：导出时选区混色在下、光标实心在上，顺序由类型定，
     *   而不是由"谁最后被 push 进数组"定 —— 后者只要有一处 push 顺序变了，
     *   光标就会被半透明地再混一次，而那种错只在重叠的几个像素上看得出。
     */
    struct PageTransient {
        /**
         * 这一页上的选区矩形（页内像素坐标）。
         */
        std::vector<Rect> sel;

        /**
         * 这一页上的光标矩形（最多一条）。
         */
        Rect caret{};
    };

    /**
     * 这一页现在该叠哪些瞬态（页内像素坐标）。presentInternal 按页算差集、导出按页取用。
     */
    void collectTransient(int page, PageTransient* out) const;
    /**
     * 一页的光栅结果：页内坐标（原点 = 页左上角），不含页间隙。
     *
     * 按页缓存是唯一的像素缓存：换窗、换缩放都不作废它（作废的判据是页指纹与
     *   光栅签名），于是滚过一页不会把已经画好的页重画一遍（一页约 8~9 ms）。
     * fp 记的是光栅时那一版页指纹（哨兵值也算一版，见 kFpDirtyBit）：
     *   与 page_fp_new_ 相等就说明这一页的像素还作数，不必重画。
     */
    struct PageBitmap {
        /**
         * 页像素（页内坐标，不含页间隙）。
         */
        Bitmap px;

        /**
         * 光栅时那一版页指纹。
         */
        uint64_t fp = 0;

        /**
         * 这一份像素是在哪一版"光栅配置"下画的。
         *
         * 页指纹只哈希字形等内容，看不见页面底色 / 边框 / 缩放 / 页度量这些"画出来是什么"
         *   的输入。把它们的哈希记在这里，比一次就是不新鲜 —— 新增一个光栅配置项时只要把它
         *   加进 rasterSignature()，所有页自动失效，不必再维护一张"记得清缓存"的清单。
         */
        uint64_t raster_sig = 0;

        /**
         * 上一次导出时叠在这一页上的瞬态（页内像素坐标：选区 + 光标）。
         *
         * 它只是"上一次记过哪几块"的账，用来算新旧差集 —— 像素本身不在页位图里，
         *   所以没有"擦"这一步：下一次导出从干净的页位图重来。
         */
        PageTransient transient;

        /**
         * 这一格装着页没有（没有就不用比对指纹）。
         */
        bool valid = false;
    };

    /**
     * 页缓存。容量按"窗口页数 + 2"条，超出时按离窗口最远淘汰。
     *
     * 它是唯一的像素缓存（96 dpi 一页 3.6 MB），所以余量不再放宽：窗口内的页
     *   一律留着，窗口外只留两条，够"滚回来一页"不重光栅。
     */
    std::unordered_map<int, PageBitmap> page_cache_;

    /**
     * 光栅配置的签名（见 PageBitmap::raster_sig）。由 setConfig 写入。
     */
    uint64_t raster_sig_ = 0;

    /**
     * 每页待上屏的脏矩形（页内像素坐标），"自上次取走以来"累积。
     */
    std::unordered_map<int, std::vector<Rect>> page_dirty_rects_;

    /**
     * 记一笔"这一页上的这块变了"（累积到 takePageDirtyRects 被取走为止）。
     *
     * @param page 页号
     * @param r 页内像素矩形
     */
    void notePageDirty(int page, const Rect& r);

    /**
     * 缓存里这一页的像素还作数吗：有条目、有指纹、尺寸对得上、指纹与本次排版相同。
     * 没有指纹（页号超出指纹数组）一律返回假 —— 不敢断言"作数"的时候就重画。
     */
    bool pageBitmapFresh(int page) const;

    /**
     * 一页的光栅本体（页内坐标）。band 非空时只画那一条横带。
     * 与 rasterPageInto 分开是为了让页自审能绕过缓存画到一块临时位图上。
     */
    void rasterIntoBitmap(Bitmap& target, int page, const float* band_lo_pt,
                          const float* band_hi_pt);

    /**
     * 把这一页光栅进缓存，并记下它对应哪一版指纹。
     */
    void rasterPageInto(int page, const float* band_lo_pt, const float* band_hi_pt);

    /**
     * 这一页"画到画布上应该是什么"：有瞬态时把内容 + 瞬态合成进一页的导出图。
     * 返回 nullptr 表示这一页拿不出来（不在缓存里或已过期）。
     */
    const uint8_t* pageDisplayPixels(int page) const;

    /**
     * 缓存淘汰：窗口内的页一律留下，窗口外按距离由远及近释放到容量以内。
     */
    void trimPageCache(int first_page, int count);

    /**
     * 本拍光栅出来的账。字形只画进页位图，所以这几个数由
     * rasterIntoBitmap 累加，呈现末尾一次性上报；自审路径会先存后还，不污染它们。
     */
    int raster_glyphs_ = 0;

    /**
     * 本拍"取字形包围盒"花掉的毫秒（记账用，不进像素）。
     */
    double raster_glyph_bounds_ms_ = 0.0;

    /**
     * 本拍铺面积花掉的毫秒。
     */
    double raster_fill_ms_ = 0.0;

    /**
     * 本拍画字形花掉的毫秒。
     */
    double raster_glyph_ms_ = 0.0;

    /**
     * 页指纹：本次 relayout 的结果 / 已画成像素的那一版。
     *
     * 只重算受影响页（@p dirty_lo 到 @p dirty_hi，含端点）：没有脏页信息时
     * 传 -1 走整篇重算。增量成立的前提是"没动过的页，字形一个字节都没动" ——
     * 它由排版那侧的前缀复用保证，而不是靠这里猜。
     */
    void computePageFingerprints(int dirty_lo = -1, int dirty_hi = -1);

    /**
     * 行指纹：与页指纹同一次遍历里算出来，只是分桶到行（见 line_fp_new_）。
     * @p dirty_lo / @p dirty_hi 是脏页范围，与页指纹同一口径；同样只重算落在这几页里的行。
     */
    void computeLineFingerprints(int dirty_lo, int dirty_hi, bool full);

    /**
     * 把 [first_page, 末页) 直接标成"与已画那一版不同"，不去哈希。
     *
     * 用在后缀整体平移那一级：后缀的每一行都沿块轴挪了位，于是它在的每一页内容都变了 ——
     * 这个答案在进这里之前就已经确定，再把几百页的字形哈希一遍纯属白花
     * （实测 161 页 = 21 ms，比排版本身贵一个数量级，而它在"回车/合并/敲到换行"上每一下都付）。
     * 页数变多时新页也落在 [first_page, 末页) 里，一并标脏，不必退回整篇重算。
     */
    void markPagesChangedFrom(int first_page);

    /**
     * 把当前行指纹与行盒几何存成"已画进页位图的那一版"。
     * 与 page_fp_prev_ 同一时机调用：页像素变了就要跟着换。
     */
    void snapshotLineGeometry();

    /**
     * 行带边界：第 k 行独占的块轴区间（页内磅），上下都取与相邻行盒的中线。
     * 见 engine_paint.cpp 的实现注释（为什么不能用行盒自己的 block_lo/hi）。
     */
    void lineBand(int k, float* lo, float* hi) const;

    /**
     * 第 @p page 页上的字形下标区间（半开区间 [begin, end)）；这一页没有字形时两者相等。
     *
     * 依据：排版按段流顺序 push 字形，页号只前进不后退，所以 layout_.glyphs 的 page
     *   单调不降 —— 两端各二分一次即可。为什么要二分而不能整篇筛：绘制是按页进行的，
     *   而整篇筛是 O(全篇字形)，每画一页就付一遍；2000 段文档约 8 万个字形，
     *   反推约 2 ms/页，比真正画那一页的字形还贵。
     *   单调性是推理 + 运行期审计（auditGlyphPagesJson）双重保证的，不是假设。
     */
    void glyphRangeOnPage(int page, int* begin, int* end) const;

    /**
     * 第 p 段的第一行落在哪一页；这一段没有行盒时返回 -1。
     * 截断之前读一次、重排之后再读一次，就能框出"这一轮可能动到的最靠前那一页"。
     */
    int firstPageOfParagraph(int p) const;

    /**
     * 把排版结果里的规模数搬进 stats_（页数、行数、字形数、段数）。
     */
    void syncStatsFromLayout();

    /**
     * 标记"从第 para 段起需要重排"（取最小值，一次操作跨多段也只需一次标记）。
     * 凡是改段内容或改段数的入口都要调它，否则增量会拿旧前缀去接新后缀。
     *
     * 它只说起点：右端当成"一直脏到末尾"。知道右端时用 markLayoutDirtyRange。
     */
    void markLayoutDirty(int para);

    /**
     * 标记 [lo, hi) 这一段需要重排。hi 是新版（重排后）块坐标里的右端、不含。
     *
     * 右端不是可有可无的细节：后缀复用要知道"被换掉的是几段"才能把后缀接回去。没有右端时
     * 它只能按前后段数差反推，而那个反推假定一次提交只在一处动段落标记 —— 撤销一批跨段的
     * 编辑不满足（一批步骤可以是"删选区 + 插入 + 改格式"，损伤区间比段数差宽得多）。
     * 多报右端是安全的（多排几段没变的内容），少报会让后缀断在错的地方，而错位是静默的。
     */
    void markLayoutDirtyRange(int lo, int hi);

    /**
     * 全量记号：段数、页度量、控件集合这类"影响面不可定位"的改动用它。
     * 与 markLayoutDirty(0) 的区别在于它禁止走后缀复用快路：整篇都脏时
     * 后缀该不该变是不知道的（页宽变了、段数变了），只有重排一遍才算数。
     */
    void markAllLayoutDirty() {
        layout_dirty_from_ = 0;
        layout_dirty_to_ = -1;
        layout_needs_full_ = true;
    }

    /**
     * 按损伤区间标脏：从区间起点排起，右端给后缀复用当"被换掉几段"用。
     *
     * 区间右端可能落在文档末尾之外（尾部整段删掉时 lo == hi == 块数）：那时没有"新的脏段"，
     * 但旧排版多出来的尾巴必须截掉 —— 从最后一段起排就把它截掉了。
     */
    void markLayoutDirtyForDamage(const rev::BlockDamage& dmg);

    /**
     * 括号占位计划变了（编辑模式切换、控件集合替换）时标脏。
     * 只标到"最靠前的那个控件所在段"：占位只插在控件边界所在的段里，
     * 更靠前的段排版结果不变。没有控件时不动 —— 那时占位集合恒为空。
     */
    void markLayoutDirtyForBrackets();

    /**
     * 控件列表里最靠前的段下标；没有控件时返回 -1。
     */
    int minControlParagraph() const;

    /**
     * 脏区起点对齐到表格边界：from 落在某张表里时，返回这张表（最外层）的起始段。
     *
     * 表格的行高要等一整行的所有格都排完才结算，格内还可能嵌着表 —— 从中途续排会丢掉
     * "已经排到第几行、这一行已经多高"这些上下文，产出的几何与全量重排不一致。从表的
     * 起始段续排则是自然的：表上下文从零建立，与全量走同一条路。
     */
    int tableAlignedFrom(int from) const;

    /**
     * 脏区右端对齐到整张表的末尾：右端落在表里（或者后缀首段已经又落回同一张表）时，
     * 把脏区扩到那张最外层表的最后一段之后。
     *
     * 表的行高要等一整行的所有格都排完才结算，所以"排到一半的表"是个中间态 ——
     * 既不能平移（行片段还没结算），也不能把后缀接在它后面。脏区只要擦着表，就得把整张表
     * 包进来。左端由 tableAlignedFrom 对齐，两端一起构成"表永远整张在脏区一侧"这条不变式。
     *
     * @param to 脏区右端（新版块坐标，不含）；< 0 或 >= 块数时原样返回（那是"排到末尾"）。
     * @return 右端的新值；没有表格输入时原样返回。
     */
    int tableAlignedTo(int to) const;

    /**
     * 把 layout_ 截断到第 para 段之前：丢掉 [para, ...) 的行盒、字形与切片，
     * 保留 [0, para) 原样，供 layoutDocumentRange 从 para 续排。
     * para <= 0 时清空整个结果。
     *
     * 表格的绘制几何一起丢：起始段 >= para 的那些 PlacedTable 属于被丢掉的那一半。
     * 不丢的话，续排重新 openTable 会再 push 一条，同一张表就有两份几何（一份是旧的）。
     */
    void truncateLayoutAt(int para);

    /**
     * 后缀复用的三种结局。
     * 三种都要求脏段自身的排版已重做完，差别只在后缀要不要动。
     */
    enum class SuffixReuse {
        None,       // 复用不成立，调用方走常规续排（从脏段排到末尾）
        Unchanged,  // 后缀一个字节都不用动（续排状态逐位相同）
        Shifted,    // 后缀整体沿块轴平移 + 重编页码/行号（行结构变了，但只有块轴位置变）
    };

    /**
     * 后缀复用：只重排脏区，然后试着不动后缀。判据两级 —— 同状态（后缀逐位相同，整块搬回）
     * 与纯横排平移（只有块轴起点变）。后缀里可以包着表，但表只能整张在脏区里或整张在复用区里：
     * 遇到第一张表就把复用截断在表头，剩下的从那张表续排（表的行高靠结算、跨页靠切片）。
     *
     * @param dirty_count 脏区在新版里占几段（右端见 markLayoutDirtyRange）
     * @param page_lo,page_hi 出参：脏区新旧两版覆盖的页区间（-1 = 整篇）
     * @param relaid_paragraphs 出参：脏区在新版里占几段
     * @param cut 出参：复用只到第 cut 段之前（后缀第一张表，或段数）；cut_state 是那儿的锚点
     * @param all_pages_changed 出参：Shifted 专用 —— 后缀在的每一页都变了
     * @return None 时后缀未被动过（调用方可以安全走续排），其余两种排版已完成
     */
    SuffixReuse trySuffixReuse(int from, int dirty_count, int* page_lo, int* page_hi,
                               int* relaid_paragraphs, int* cut, ReflowState* cut_state,
                               bool* all_pages_changed);

    /**
     * 后缀整体平移：把 [from, to) 的每一行沿块轴挪到新的位置，并重编页码与全局行号，字形跟着
     * 它的行一起挪，断行结果（簇与行内位置）完全不重算 —— 横排的块轴推进是一条纯折线（每行
     * 占字号乘行距、段间让出 space_after_pt、放不下就换页），知道起点的续排状态就唯一确定。
     *
     * @param from 后缀的第一段（含）；段数变化时它与脏区首段相差不止一
     * @param first_glyph 后缀的第一个字形在 glyphs 里的下标（脏区的字形之后）
     * @param to 平移的上界（不含）：后缀里的第一张表，或段数
     * @param end_state 出参：走到 to 时的续排状态；to < 段数时调用方拿它从那儿的表续排
     */
    bool shiftSuffixBlockAxis(int from, size_t first_glyph, int to, const ReflowState& after,
                              ReflowState* end_state);

    /**
     * [from, to) 里有没有竖排段 —— 竖排不能走平移复用。
     */
    bool suffixIsHorizontalOnly(int from, int to) const;

    /**
     * 画不可见字符标记（段落标记、空格里的中点、制表符箭头）到一页的位图上。
     *
     * 只画进页位图、不占宽度：标记不是内容，改档位只让像素变（见 rasterSignature），
     *   断行、光标落点、命中与流位置一个都不动 —— 这也是它可以随手开关的前提。
     *
     * 几何从行盒派生：段落标记坐在"这一段的最后一行"的行末（段内码点数 == 行盒的
     *   last_cp），空格的中点坐在该簇 inline_pt 与下一簇 inline_pt 的中点。字体与基线取
     *   行盒的 bracket_font / bracket_axis_pt（块主字体），所以标记的粗细风格跟正文一致。
     * band 与 spill_only 的语义与 rasterControlMarks 相同。
     */
    void rasterEditingMarks(Bitmap& target, int page_index, const float* band_lo_pt,
                            const float* band_hi_pt, float dy_px, bool spill_only);

    /**
     * 编辑标记此刻画不画：档位不是"不显示"，而且不在只读态（只读是呈现态，标记会被印出来）。
     *
     * 光栅签名混的是这一位而不是档位 —— 切只读不改内容，只有签名能让页位图作废。
     */
    bool marksShown() const { return cfg_.marks != MarksMode::None && mode_ != EditMode::ReadOnly; }

    /**
     * 画表单标记（方括号与小点）到一页的位图上 —— 页内坐标，不认帧。
     *
     * 几何全部从排版的行盒派生（占位簇的位置 + 行盒的括号参考字体/基线）：
     * 排版说占位在哪，就画在哪 —— 空字段（一个字形都没有）也画得出来。
     * 判据见 docs/步5-渲染层方案.md 的 5D 小节。
     *
     * band 非空时只画与这条带相交的行与控件 —— 与字形、原子框同一套裁剪口径。
     * dy_px 把整笔上移（连页接缝补画那半截时传一个负的带高）；spill_only 为真时
     *  只画越过带底的那几条行盒与控件 —— 与 rasterIntoBitmap 的接缝补画同一口径。
     */
    void rasterControlMarks(Bitmap& target, int page_index, const float* band_lo_pt,
                            const float* band_hi_pt, float dy_px, bool spill_only);

    /**
     * 画一枚行内原子的显式占位框到页位图上。
     *
     * 与表单标记分开：原子是页面内容（它是流里的一项，进页指纹与行指纹），所以画进页位图、
     * 不随光标状态擦除；标记跟编辑模式走，换模式要重算页内容，两者都不进瞬态层。
     *
     * 几何全部从行盒的占位簇派生：沿行内轴的两端取簇的 inline_pt 与下一个簇的 inline_pt，
     * 底边坐在行盒的基线上，高度取簇自带的盒高。于是"占了位"与"画了框"共用同一份数据。
     *
     * @param target 光栅目标（页位图）
     * @param a1_pt 框在行内轴上的末端（页内磅）；a0 由 c.inline_pt 与行盒的 inline_origin 给
     * @param off_y 这一页在目标里的纵向偏移（像素）
     */
    void drawAtomBox(Bitmap& target, const LineBox& lb, const LineCluster& c, float a1_pt,
                     float off_y);

    /**
     * 光标 / 编辑模式动过之后调用：括号占位计划变了就重排。
     * 传的是改动之前的 bracketPlanId()。All 档下它恒为常数 →
     * 光标在同一/不同字段间移动都不会重排（"光标移动 = 真增量"的地基）。
     */
    void relayoutIfBracketPlanChanged(int prev_plan);

    /**
     * 占位计划从 prev 换到 cur 时标脏（prev / cur 是 bracketPlanId 的取值）。
     * 两个字段各自的段跨度取凸包；不是具体字段时退回 markLayoutDirtyForBrackets。
     */
    void markLayoutDirtyForBracketPlans(int prev, int cur);

    /**
     * 这一段内容所在的最内层那一格（连它所属的表一起给出）。不在表格里返回空。
     *
     * 与 cellFlowRangeAt 同一把尺子（最短区间 = 最内层），只是把"哪一格"也交出去 ——
     * 跨行拖选要按行列取矩形包络，光有流区间是取不出来的。
     */
    const LayoutCell* layoutCellAt(int para, const LayoutTable** table_out = nullptr) const;

    /**
     * 一格的内容流区间（左闭右开）。空格子或坐标与块表对不上时返回假。
     */
    bool cellFlowRangeOf(const LayoutCell& c, FlowPos* lo, FlowPos* hi) const;

    /**
     * 拖选 / Shift 扩展之后重算选区粒度：两端落在两个不同的格里就是单元格选区。
     *
     * 为什么放在引擎而不是宿主：这是"选区盖住了什么"的判断，与表格结构有关，而宿主手里
     * 只有命中测试的落点。放在引擎里还有一条好处 —— 拖选、Shift 加方向键、程序化扩展
     * 走的是同一条判定，不会出现"拖行、按方向键不行"。
     *
     * 两端都在同一个格（或有一端不在格内）时不吸附，仍是文字选区：格内把字选到最后一个
     * 不该整格反白 —— 那是两套操作，不是粗细两档。
     *
     * 选中的是两格之间的矩形（行包络 x 列包络），不是"首格首到末格末"那条流区间：
     * 文档顺序是逐行的，按流区间取会把两端那两行里没选中的格一起反白（跨行就整行）。
     */
    void syncCellSelection();

    /**
     * 表格自审的分项计数（见 auditTables）。逐项分开记是为了取证：同一条判据下，
     * "某格没有块"与"块没有结束标记"是两种完全不同的坏法。
     */
    struct TableAudit {
        /**
         * 扫过的格数。
         */
        int cells = 0;

        /**
         * 一个块都没有的格（这种格没有行盒，光标进不去）。
         */
        int empty_cell = 0;

        /**
         * 块的区间越出块表。
         */
        int bad_range = 0;

        /**
         * 两个格认领同一块（同表）。
         */
        int overlap = 0;

        /**
         * 格里的块没有结束标记。
         */
        int block_without_mark = 0;

        /**
         * 块的 cell_path 与格给出的区间对不上。
         */
        int path_mismatch = 0;

        /**
         * 跨度越出表的尺寸。
         */
        int bad_span = 0;

        /**
         * 块说自己在正文，格却认领了它。
         */
        int blocks_in_no_cell = 0;

        /**
         * 首个出问题的格（跨表连续编号）；-1 = 没有。
         */
        int first_cell = -1;

        /**
         * 各类不符之和。
         * @return 不符合项总数
         */
        int total() const {
            return empty_cell + bad_range + overlap + block_without_mark + path_mismatch +
                   bad_span + blocks_in_no_cell;
        }
    };

    /**
     * 表格自审的分项计数（见 auditTables 的六条）。
     *
     * @return 各项计数与首个出问题的格
     */
    TableAudit auditTablesDetail() const;

    /**
     * 生效中的配置（整份替换由 setConfig 做）。
     */
    EngineConfig cfg_;

    /**
     * 字体库（字体链、字脸与字形缓存）。
     */
    FontLibrary fonts_;

    /**
     * 筛过括号的那一份，只在 filter_active_ 为真时有效。
     */
    std::vector<BlockSpec> blocks_;

    /**
     * 文档（唯一真相）：一份修订加一条历史。blocks_ 与 blocks_full_ 都是它的投影产物，
     * 没有第二份可写的树 —— 编辑只产步骤，产物每轮由新修订重算。
     */
    std::optional<rev::Session> session_;

    /**
     * 装载时给的样式表（命名段样式 / 字符样式）。
     */
    StyleSheet sheet_;

    /**
     * 上一轮每块的身份（内容哈希 + 来源）与样式表版本，与 blocks_full_ 同长同序。
     *
     * 它只是"上一轮的账"，不是第二个真相：块本身就在 blocks_full_ 里，投影按损伤区间
     * 原地保留区间之外的那些 —— 所以既没有第二份块流，也没有整份拷贝。装载新文档时清掉。
     */
    rev::BlockHistory proj_hist_;

    /**
     * 装载过文档没有（setDocument 置位）。没装载过时没有树、没有流、也没有事务栈 ——
     * 编辑入口一律拒绝，审计入口报"没有可比的东西"，而不是拿一棵空树去编出 0 段来。
     */
    bool loaded_ = false;

    /**
     * 块流（唯一一份）：投影就地更新它，排版也吃它。
     *
     * "按要显示括号的名单筛一遍"是破坏性的（对象表会被删项），而名单会来回变，所以
     * 只有真的需要筛时才把筛过的结果放进 blocks_ —— 名单覆盖全部控件时 blocks_ 是空的，
     * blocks() 直接给这一份，不存在副本。
     */
    std::vector<BlockSpec> blocks_full_;

    /**
     * 筛过括号的块流。只为"名单真的排除了某些控件"而存在，其余时候为空。
     */
    bool filter_active_ = false;

    /**
     * 表格结构（编译产物的一种投影，与 blocks_full_ 同源）。排版吃它来按格布局；
     * 它也是"这张表跨了几页、格线画在哪"的输入。没有表格时为空。
     */
    std::vector<LayoutTable> tables_;

    /**
     * 控件集合下标 → 控件的节点 id（导出时按同一顺序建）。
     */
    std::vector<NodeId> control_node_ids_;

    /**
     * 产物的 owner（节点 id）→ 控件集合下标。导出时一并建好，命中测试与编辑归属
     * 靠它把句柄翻译成本层对外的口径。
     */
    std::unordered_map<int, int> owner_index_;

    /**
     * 树口径的流索引。可见性与编译同一处取（编辑态是设计态，呈现态摘掉隐藏子树），
     * 于是"块流里数得到的东西"与"流位置里数得到的位数"始终是同一份文档的两种说法。
     *
     * 它是派生量：树一改就过期，所以只在重建块流时一起重建，不单独维护。用 optional
     * 而不是常驻对象，是因为 FlowIndex 没有默认构造 —— 它一造出来就要一棵树。
     */
    std::optional<rev::FlowIndex> flow_;

    /**
     * 每块的局部尺寸，与 blocks_full_ 同长同序，是投影走树时数出来的（见 rev::BlockExtent）。
     *
     * 它是位置的唯一来源：下面那两个数组都是它的前缀和。写进这里的只有"这一块多长"与
     * "它前面那道界多宽"—— 两个局部量。上游变长不会让下游的任何数字过期，因为下游本来
     * 就没存绝对数。
     */
    std::vector<rev::BlockExtent> block_extents_;

    /**
     * 每块内容起点的流位置（全局），与 blocks_full_ 同长同序。
     *
     * 它是 block_extents_ 的前缀和 —— 纯派生，每轮重建时整份重算（一次加法循环）。之所以
     * 还要物化成数组而不是每次都现累加：光标定位要二分（paraForFlow）、命中测试要随机
     * 读，O(log n) 次随机访问不能变成 O(n log n)。
     */
    std::vector<FlowPos> block_flow_;

    /**
     * 每块内容在流里占的位数：文本码点数 + 控件边界数。段间那个段落标记不在这里 ——
     * 它是段与段之间的位置，不属于任何一段的内容。与 block_extents_[i].span 同一个数。
     */
    std::vector<int> block_flow_span_;

    /**
     * 每块的来源身份（编译给出，与 blocks_full_ 同长同序）。编辑要在树上落笔时靠它找到
     * "这一段是哪一棵子树里的那一段、被哪枚标记结束"。
     */
    std::vector<BlockSource> block_src_;

    /**
     * 当前排版产物（行盒、字形、每段切片、页数）。
     */
    LayoutResult layout_;

    /**
     * 段级整形缓存（HarfBuzz 结果 + 簇推进 + 断点）。
     * 整形是唯一"必须摸字体"的一步，也是最贵的；它的输入只有文本与字体参数，
     * 所以改页宽、切编辑模式之后照样命中（见 layout.cpp 的 shapingFingerprint）。
     */
    ShapingCache shaping_cache_;

    /**
     * 需要重排的最小段下标；>= blocks().size() 表示没有脏段。
     * 前后两个数构成一个半开区间：前缀原样保留，后缀由 trySuffixReuse 试着搬回，
     * 中间 [from, to) 这一段必须重排。
     */
    int layout_dirty_from_ = 0;

    /**
     * 脏区间的右端（不含，按重排后的块坐标）。-1 = 不知道有多长。
     *
     * 为什么要它：撤销没有命令层算出的落点，只有两份修订相减得到的块区间 —— 区间知道
     * 自己到哪儿结束。没有右端时 trySuffixReuse 只能按段数差反推"被换掉几段"，而那个
     * 反推假定一次提交只在一处动段落标记；一批跨段的编辑（删选区 + 插入 + 改格式）
     * 会让反推偏窄，后缀就从错的地方接上去，症状是"撤销之后后半篇整体错位"。
     */
    int layout_dirty_to_ = -1;

    /**
     * 整篇都脏（换文档、改页度量、页度量变化）。与"从第 0 段起脏"不同：
     * 后者只是第一段的内容变了，后缀仍有可能原样复用（见 trySuffixReuse）。
     */
    bool layout_needs_full_ = true;

    /**
     * 上一次 relayout 的粒度，由 presentInternal 上报到 EditStats。
     */
    int last_relaid_paragraphs_ = 0;

    /**
     * 上一次 relayout 里后缀原样复用的段数。
     */
    int last_reused_paragraphs_ = 0;

    /**
     * 后缀是被"平移复用"的（见 SuffixReuse::Shifted）—— presentInternal 上报用。
     */
    bool last_suffix_shifted_ = false;

    /**
     * 上一次 relayout 真正重排的行数。
     */
    int last_relaid_lines_ = 0;

    /**
     * 上一次 relayout 是全量还是增量。
     */
    bool last_layout_full_ = true;

    /**
     * 字体表的持有者：0x0，一像素都不画。
     *
     * Skia 的字形缓存按 strike 规格索引，规格里含字体对象的身份 —— 每块位图各建一份
     *   字体表，缓存就分成互不相见的两份，往其中一份预热对另一份毫无帮助，而症状只是
     *   "没变快"。它就是这个锚点：其余位图一律 shareFonts(font_owner_)。
     */
    Bitmap font_owner_;

    /**
     * 预热字形用的临时靶子：一条页宽的窄带，画进去的像素随用随丢。
     *
     * 为什么不另做一条"只光栅不画"的路径：mask 进缓存这件事由绘制路径负责
     *   （Skia 的 strike 只认真正走过的 drawGlyphs）。另起一条就要再写一遍
     *   缓存键的算法，而两条路径的键只要差一点，预热就毫无效果，且不会报错。
     *   靶子够宽够高，字形不会被裁，缓存才存得下。
     */
    Bitmap prewarm_;

    /**
     * 排版与呈现的账（由 syncStatsFromLayout 与呈现各写一半）。
     */
    EngineStats stats_;

    /**
     * 编辑那一侧的账（文档规模、重排段数、上屏面积、光标读数）。
     */
    EditStats edit_stats_;

    /**
     * 与光栅后端有关的、只能设一次的那些参数（目前是 Skia 的字形缓存预算）。
     *
     * 为什么要有这么一个"懒做一次"的入口，而不是摆在 init() 里：wasm 宿主不走 init() ——
     *   它是 g_engine.fonts().addFaceFromMemory(...) 直接装字体，于是摆在 init() 里的
     *   设置在那条路上一次都不会执行（预算写成 48 MB，面板读回来仍是默认的 2 MB）。
     *   present 与预热是所有宿主都必经的两条路，而它们又都在"要光栅字形"之前，
     *   所以放在那儿是唯一保证生效的位置。
     */
    void ensureRenderSetup();

    /**
     * 与光栅后端有关的那些"只设一次"的参数设过了没有。
     */
    bool render_setup_done_ = false;

    /**
     * 把两份视图摆给它们各自的类：位置解析读 PositionView，命令层读 EditView。
     *
     * 只在文档重建之后与重排之后各摆一次就够：视图里存的是这些容器的地址，而容器都是引擎的
     * 成员、不换对象（重建是 assign、重排是就地改），地址一直有效。
     * 谁要是把某个容器换成了另一个对象（move 走、重新 new），这里就是唯一的失效点。
     */
    void refreshViews();

    /**
     * 光标与位置解析。选区两端、行别、上下键的列位、光标条宽度全在它里面 ——
     * 引擎这一层只留"读文档产物"与"协调重排"（见 projectCaret 与移动族的转发）。
     */
    CaretResolver caret_;

    /**
     * 命令层：一次编辑怎么变成一条事务（插入、删除、分段、合并、缩进、结构操作）。
     *
     * 它只认 EditView 与 EditHost 两个接口 —— 引擎在这里的角色是"事务的出口"与
     * "视图的持有者"，编辑本身的做法（策略、步骤、撤销快照、光标落点）全在它里面。
     */
    EditCommand edit_;

    /**
     * 光标这一拍画不画（闪烁相位与 showCaret 共同决定）。
     */
    bool caret_visible_ = true;

    /**
     * 本次呈现的窗口（见 windowFirstPage / windowPageCount）。
     *
     * 每页各有一张位图，页在画布上的位置由页号直接定，于是"窗口开到哪"只影响
     *   "这一拍要保证哪几页是新鲜的"，不影响任何像素的落点。
     */
    int win_first_page_ = 0;

    /**
     * 本次呈现覆盖的页数（见 windowPageCount）。
     */
    int win_page_count_ = 0;

    /**
     * 视口锚点（见 setViewportAnchor）。count <= 0 = 整篇。
     */
    int vp_first_page_ = 0;

    /**
     * 视口锚点钉了几页（<= 0 = 没钉，按整篇）。
     */
    int vp_count_ = 0;

    /**
     * 本次呈现写过的页（绝对页号，升序去重）。见 dirtyPages()。
     */
    std::vector<int> last_dirty_pages_;

    /**
     * 页内容指纹：本次 relayout 的结果 / 已画成像素的那一版。
     *
     * 真指纹一律清掉 kFpDirtyBit —— 那一位留给"已知变了"的哨兵（markPagesChangedFrom），
     * 于是"这一页变了没有"永远只有一个判据：new != base。
     */
    std::vector<uint64_t> page_fp_new_;

    /**
     * 已经画成像素的那一版页指纹。
     */
    std::vector<uint64_t> page_fp_prev_;

    /**
     * 本次排版算出来的页指数。
     */
    int page_fp_new_count_ = 0;

    /**
     * 已画那一版的页指数。
     */
    int page_fp_prev_count_ = 0;

    /**
     * 行内容指纹：与页指纹同源、同一把尺子，只是分桶到行（下标 = LineBox::line）。
     *
     * 它存在的唯一理由是把"这一页要重画"细化成"这一页的哪几行要重画"：
     * 一页约 1000 个字形，而敲一个字只动其中一两行的字形 —— 页指纹粒度下，
     * 改一行与改整页同价（实测字形光栅 10~14 ms，其中九成是白画的）。
     *
     * 指纹必须含行盒几何（block_lo/hi、inline_origin）：字形一样但位置挪了
     * 也算变了 —— 那是"后缀整体平移"那一类。
     */
    std::vector<uint64_t> line_fp_new_;

    /**
     * 已经画成像素的那一版行指纹。
     */
    std::vector<uint64_t> line_fp_prev_;

    /**
     * 已画那一版的行数（行指纹数组的规模）。
     */
    int line_fp_prev_count_ = 0;

    /**
     * 已画进页位图的那一版里，每一行的块轴范围（页内磅），按行下标。
     * 重画一条行带时要靠它算"擦到哪儿"：上边界取新旧两版里更靠上的那个，
     * 否则行往下挪之后旧墨迹会留在原地（幽灵字）。下边界同理取更靠下的。
     */
    std::vector<float> line_lo_prev_;

    /**
     * 已画那一版每一行的块轴下界（页内磅）。
     */
    std::vector<float> line_hi_prev_;

    /**
     * 本次重画那条行带的页内磅范围（computeBand 写、rasterIntoBitmap 读）。
     * 放成员是为了不把两个 float 在 lambda 之间传来传去。
     */
    float band_lo_scratch_ = 0.0f;

    /**
     * 本次重画那条行带的上界（页内磅）。
     */
    float band_hi_scratch_ = 0.0f;

    /**
     * 导出用的两块临时位图（都只在真被读到时才分配）。
     *
     * page_export_：一页大小，"内容 + 瞬态"的合成落点；
     * frame_export_：窗口大小，把各页拼起来给"扫画面"那类判据用。
     * 它们是派生物、不是第二个真相：下一次 present 就作废，重拼时唯一的数据源是页位图。
     */
    mutable Bitmap page_export_;

    /**
     * 单页导出图当前装的是哪一页（-1 = 没装）。
     */
    mutable int page_export_page_ = -1;

    /**
     * 窗口导出图（读时现场拼，present 作废）。
     */
    mutable Bitmap frame_export_;

    /**
     * 窗口导出图还作不作数。
     */
    mutable bool frame_export_valid_ = false;

    /**
     * relayout() 置位、presentInternal() 消费。
     * 用它把"排版记账"和"光栅记账"分开：一次 relayout 后面可能跟很多次
     * repaint（光标闪烁每 530ms 就一次），只有第一次该认领那笔排版成本。
     */
    bool layout_pending_ = false;

    /**
     * 表单：控件区间集合与编辑模式。
     *
     * 模式只决定"光标能落在哪、哪些命令可用"这一层策略（见 edit_capability），它属于
     * 控件这一侧的口径而不是文档：同一棵树切到不同模式，块流与可见性都可能跟着变。
     */
    ControlSet controls_;

    /**
     * 当前编辑模式（见 setEditMode）。
     */
    EditMode mode_ = EditMode::Document;

    /**
     * 累计被策略拒掉的编辑次数。
     */
    long long blocked_edits_ = 0;
    /**
     * 每一份修订对应的两份选区快照（编辑前 / 编辑后）。历史本体在 session_ 里，
     * 这里只补它记不了的那一半 —— 撤销与重做都要把光标摆回按下去之前的位置。
     *
     * 与 session_ 的撤销栈同步裁剪：超上限时丢最旧的，与"丢更早的状态"同一口径。
     */
    struct SelectionRecord {
        /**
         * 编辑前的选区。
         */
        Range before;

        /**
         * 编辑后的选区。
         */
        Range after;
    };

    /**
     * 每一份修订对应的两份选区快照（与撤销栈同步裁剪）。
     */
    std::vector<SelectionRecord> sel_history_;

    /**
     * 当前处在哪一格历史（与 session_ 的撤销位置同步）。
     */
    size_t sel_pos_ = 0;

    /**
     * 上一次排版时各控件的只读位（1 = 只读）。
     *
     * 只读位只改括号颜色，不进块哈希 —— 页指纹看不见它。而括号现在画在页位图里
     * （见 rasterControlMarks），所以 relayout 里比一次这个数组：变了就作废页缓存。
     * 不比的话，"把字段改成只读"之后括号还是蓝色，直到别的原因让那一页重光栅。
     */
    std::vector<uint8_t> mark_readonly_prev_;
};

}  // namespace heditor
