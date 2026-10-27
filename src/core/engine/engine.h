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
#include "core/engine/image_store.h"
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
     * 双击选词：按落点所在字符的类别向两侧扩边（空白、字母数字、汉字、标点各成一类）。
     *
     * 没有分词器，连续汉字整串选中 —— 这是对"词"最接近的近似，也符合病历里
     * "双击挑一个术语"的实际用法；英文按字母数字串，标点按连续同类。
     *
     * @param x_pt_doc 文档横坐标（磅）
     * @param y_pt_doc 文档纵坐标（磅）
     * @return true = 选中了一个非空区间
     */
    bool selectWordAtPt(float x_pt_doc, float y_pt_doc);

    /**
     * 双击选词并重画。
     *
     * @param x_pt_doc 文档横坐标（磅）
     * @param y_pt_doc 文档纵坐标（磅）
     * @return true = 选中了一个非空区间
     */
    bool selectWordAtPtAndRender(float x_pt_doc, float y_pt_doc);

    /**
     * 三击选段：表格外选整段内容，表格内退化成选整格（与 Word 的三击一致）。
     *
     * @param x_pt_doc 文档横坐标（磅）
     * @param y_pt_doc 文档纵坐标（磅）
     * @return true = 选中了；空段落返回假（没有可选内容，宿主按落光标处理）
     */
    bool selectParaAtPt(float x_pt_doc, float y_pt_doc);

    /**
     * 三击选段并重画。
     *
     * @param x_pt_doc 文档横坐标（磅）
     * @param y_pt_doc 文档纵坐标（磅）
     * @return true = 选中了
     */
    bool selectParaAtPtAndRender(float x_pt_doc, float y_pt_doc);

    /**
     * 把当前文字选区搬到这个点上（一次事务：删除 + 插入，撤销只一步）。
     *
     * 目标落在选区内部、或选区含块级元素（表格）时拒绝 —— 整表插进段落中间没有语义。
     *
     * @param x_pt_doc 文档横坐标（磅）
     * @param y_pt_doc 文档纵坐标（磅）
     * @return true = 搬了
     */
    bool moveSelectionToPt(float x_pt_doc, float y_pt_doc);

    /**
     * 搬选区 + 重排 + 呈现。
     * @param x_pt_doc 文档横坐标（磅）
     * @param y_pt_doc 文档纵坐标（磅）
     * @return true = 搬了
     */
    bool moveSelectionToPtAndRender(float x_pt_doc, float y_pt_doc);

    /**
     * 把一张图片搬到落点上（同样一次事务）。
     *
     * 拖动时同时给两种数：落点（x / y，指针在哪）与位移（dx / dy，从按下到松手走了多远）。
     *   浮动图吃位移 —— 鼠标抓在图身哪一点都行，整张图跟着指针走；行内图吃落点 ——
     *   它插进文字流的哪个位置，只能由指针那一点命中出来。两件事不能合成一个数：
     *   把位移加到图的原点上，落点会落在图自己身上（从右边缘起手时偏了整整一张图）。
     * @param index 图片下标
     * @param x_pt_doc 文档横坐标（磅）
     * @param y_pt_doc 文档纵坐标（磅）
     * @param relative true = 这是一次拖动（浮动图改用 dx / dy）
     * @param dx_pt 横向位移（磅）
     * @param dy_pt 纵向位移（磅）
     * @return true = 搬了
     */
    bool moveImageToPt(int index, float x_pt_doc, float y_pt_doc, bool relative = false,
                       float dx_pt = 0.0f, float dy_pt = 0.0f);

    /**
     * 搬图片 + 重排 + 呈现。
     * @param index 图片下标
     * @param x_pt_doc 文档横坐标（磅）
     * @param y_pt_doc 文档纵坐标（磅）
     * @param relative true = 这是一次拖动（浮动图改用 dx / dy）
     * @param dx_pt 横向位移（磅）
     * @param dy_pt 纵向位移（磅）
     * @return true = 搬了
     */
    bool moveImageToPtAndRender(int index, float x_pt_doc, float y_pt_doc, bool relative = false,
                                float dx_pt = 0.0f, float dy_pt = 0.0f);

    /**
     * 改一张图的版式：0 行内、1 四周型、2 上下型、3 衬于文字下方、4 浮于文字上方。
     *
     * 后四档都是浮动（anchor = Float）；"上下型"与"四周型"参与断行，后两档只叠在文字上。
     * 一次一步撤销；同一个档位重复设置不产生修订。
     *
     * @param index 图片下标（文档顺序）
     * @param layout 版式档位
     * @return true = 改了（幂等时也成立）
     */
    bool setImageLayout(int index, int layout);

    /**
     * 反算一张图相对锚段第一行行顶与内容区左边的偏移（行内 -> 浮动时用）。
     *
     * 反算的目标是"位置不动"：改环绕方式只该改文字的流法，不该让图跳到段首。
     * @param id 图片节点
     * @param rects 它当前的几何（页内磅，images() 里那一条）
     * @param dx 写回横向偏移（磅）
     * @param dy 写回纵向偏移（磅）
     * @return 反算成功
     */
    bool floatOffsetsFor(NodeId id, const std::vector<PageRect>& rects, float* dx, float* dy) const;

    /**
     * 改版式 + 重排 + 呈现。浮动与否会改断行，所以必须重排。
     * @param index 图片下标
     * @param layout 版式档位
     * @return true = 改了
     */
    bool setImageLayoutAndRender(int index, int layout);

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
     * 光标此刻的流位置（派生）。整格选区时它是钉住的那一格末尾，与 caret() 同源。
     *
     * @return 光标的全局流位置
     */
    FlowPos caretFlowPos() const;

    /**
     * 光标此刻是不是一个可插入位置 —— 它决定画不画那根竖线。
     *
     * 选区与光标是两件事：选区可以盖住只读区（核对上文、复制），光标不能停在打不进字的
     * 地方。表单模式只认可编辑集合内的位置（含"只读控件里嵌可编辑控件"那一档交集），
     * 只读模式一个都不认。它只回答"画不画"，位置本身由 confineCaret 收束。
     *
     * @return true = 光标该画出来
     */
    bool caretInsertable() const;

    /**
     * 这一拍到底画不画光标（相位、可插入位置、图片选区三件事的交集）。
     *
     * 单独成一处是因为它有两个读者：呈现时的瞬态层与编辑账（探针读 caretVisible）。两处
     * 各写一份的话，"账上说画了、画面上没有"迟早分叉。
     *
     * @return true = 这一拍该画
     */
    bool caretShown() const;

    /**
     * 一个控件的内容流区间（闭区间）。不在流里（隐藏、摘除）返回假。
     *
     * @param index 控件下标
     * @param lo 写回内容起点（开括号之后那一格）
     * @param hi 写回内容末尾（闭括号自己那一格）
     * @return true = 这个控件在流里
     */
    bool controlContentSpan(int index, FlowPos* lo, FlowPos* hi) const;

    /**
     * 某个流位置落在哪个控件的内容区间里。嵌套时取最内层 —— 起点最大者，同起点取跨度短者。
     *
     * @param f 流位置
     * @return 控件下标；-1 = 不在任何控件的内容里
     */
    int innermostControlAt(FlowPos f) const;

    /**
     * 某个流位置能不能放插入点（与光标此刻在哪儿无关）。
     *
     * 表单模式要求它落在某个控件的内容区间里，而且那一层的祖先链上既没有业务只读的、
     *   也没有"内容只是投影"的（值不在内容里，正文位置没有意义）；
     * 文档模式没有限制，只读模式一个位置都不认。
     *
     * @param f 流位置
     * @return true = 可以放插入点
     */
    bool insertableAtFlow(FlowPos f) const;

    /**
     * 这个控件的内容是不是作者打的字（值在内容里）。
     *
     * 值在记录上的控件（选择类、下拉、日期）内容只是投影：删除要整块、光标不停、
     *   Tab 也不跳过。这三件事按值形态判，不占用 read_only —— 那个位留给业务只读，
     *   两者混用会让"设了只读"与"内容只是投影"分不开。
     * @param index 控件下标
     * @return true = 内容可编辑（值不在内容里时为 false）
     */
    bool controlContentEditable(int index) const;

    /**
     * 这个流位置是不是落在"光标屏障"控件的内容里（四种选择控件）。
     *
     * 与 capability 里的同名判断分工不同：那边收一份调用方的流索引（给命令层用），
     * 这里用引擎自己的那一份（给光标收约束用），两者口径必须一致。
     * @param f 流位置
     * @return true = 落在屏障内容里
     */
    bool barrierAtFlow(FlowPos f) const;

    /**
     * 包住 f 的那个屏障控件的整块区间（含两枚括号）。
     *
     * 光标被屏障挡住时要贴到块的外面，而"外面"只能由整块区间的两端给出：
     * 内容区间的两端仍在块里。
     * @param f 流位置
     * @param first 写回开括号那一格
     * @param end 写回闭括号之后那一格
     * @return true = f 落在某个屏障的内容里
     */
    bool barrierSpanAt(FlowPos f, FlowPos* first, FlowPos* end) const;

    /**
     * 光标被挡住时怎么选落点，取决于是哪一种动作把它带到那里的。
     */
    enum class CaretConfine {
        /**
         * 一次落点（点画布）：夹到离点击处最近的那一侧。同一个位置点两次结果必须一样 ——
         * 按"来的那一侧"取对面的话，点同一处会在控件左右来回跳。
         */
        Place,
        /**
         * 一次移动（方向键、Home/End、Tab）：夹到"来的那一侧"的对面 —— 贴回原侧的话
         * 方向键在控件边上原地不动，永远过不去。
         */
        Step,
    };

    /**
     * 把光标收进可插入位置（表单模式的落点规则）。
     *
     * 目标落在控件之外时夹回"移动之前所在控件"的内容区间，视觉上就是停在字段边界上；
     * 夹不进去（移位之前就不在任何字段里）就整下不动。程序化定位不经过这里。
     *
     * @param before 这一步移动之前的流位置
     * @param why 这一步是落点还是移动（只影响被屏障挡住时贴哪一侧）
     * @return true = 光标位置真的变了
     */
    bool confineCaret(FlowPos before, CaretConfine why = CaretConfine::Step);

    /**
     * 一次落点的唯一实现：投影 → 夹到可插入位置 → 记下"拖选可以从这里起手"。
     *
     * 按下的那一点若落在屏障里，先存进 drag_anchor_，等拖选第一次搬焦点端时放到锚端上。
     * @param para 段号
     * @param offset 段内码点偏移
     * @param side 侧别（-1 = 没有侧别信息）
     * @param line_aff 行别（-1 = 按流位置自动判）
     */
    void placeCaretFromClick(int para, size_t offset, int side, int line_aff);

    /**
     * 拖选第一次搬焦点端时，把锚端放回按下那一刻的位置。
     *
     * 锚端可以停在控件里（焦点端不行）：从选择类的标签上按下往右拖，选区就该从那个字
     * 开始。待定值只用一次，被改短之后已不在屏障里的就丢掉。
     */
    void applyDragAnchor();

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
     *
     * 直接读 caretFieldHint（按光标的流位置查控件内容区间，见 ControlSpans）——
     * 不再有"侧别推不出来时按文字区间回落"那一层：流位置是确定的，没有推不出来的情形。
     *
     * @return 控件下标；-1 = 不在任何控件里
     */
    int focusedControlIndex() const {
        const int hint = caretFieldHint();
        return hint >= 0 ? hint : -1;
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
     * 撤销栈里还有几步。宿主用它做禁用态，判据用它验"一次拖拽只留一步"。
     * @return 步数
     */
    int undoDepth() const;

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
     * 表格结构命令的动词。
     *
     * 一条枚举管住六条命令：它们共享"光标在哪一格""只在文档模式放行""一批步骤一次提交"
     * 这三件事，只有动词不同。分成六个入口时这四句话要抄六遍，抄漏一遍就是一个能改只读
     * 文档的后门。
     */
    enum class TableOp {
        InsertRowAbove,     // 在光标所在行之上插一行
        InsertRowBelow,     // 在光标所在行之下插一行
        RemoveRow,          // 删掉光标所在行
        InsertColumnLeft,   // 在光标所在格左侧插一列
        InsertColumnRight,  // 在光标所在格右侧插一列
        RemoveColumn,       // 删掉光标所在列
        MergeCells,         // 把整格选区合并成一格
        SplitCell,          // 把光标所在格拆回它覆盖的格位
        RemoveTable         // 删掉光标所在的那张表（不止清空内容）
    };

    /**
     * 跑一条表格结构命令。光标必须落在表格里，否则拒绝（"删一行"的主语是光标所在的那一行）。
     *
     * @param op 动词
     * @return true = 改成了
     */
    bool tableOp(TableOp op);

    /**
     * 这条表格结构命令现在做不做得成。
     *
     * 与 tableOp 同一条判定：模式、光标在不在表里、结构上的前置条件都算。宿主据此置灰，
     *   不必自己重算"只剩一行不能删"那几条 —— 重算出来的那一份迟早与引擎分叉。
     * @param op 动词
     * @return true = 现在执行会成功
     */
    bool canTableOp(TableOp op) const;

    /**
     * 表格结构命令 + 重排 + 呈现。
     * @param op 动词
     * @return true = 改成了
     */
    bool tableOpAndRender(TableOp op);

    /**
     * 改光标所在格的记录（垂直对齐）。
     * @param patch 要改的项
     * @return true = 改成了（幂等时也成立）
     */
    bool setCellSpec(const CellPatch& patch);

    /**
     * 改光标所在格的记录 + 重排 + 呈现。
     * @param patch 要改的项
     * @return true = 改成了
     */
    bool setCellSpecAndRender(const CellPatch& patch);

    /**
     * 改光标所在行的记录（标题行、跨页断行、行前分页、行高）。
     * @param patch 要改的项
     * @return true = 改成了（幂等时也成立）
     */
    bool setRowSpec(const RowPatch& patch);

    /**
     * 改光标所在行的记录 + 重排 + 呈现。
     * @param patch 要改的项
     * @return true = 改成了
     */
    bool setRowSpecAndRender(const RowPatch& patch);

    /**
     * 改光标所在表格的记录（宽度、水平位置）。
     * @param patch 要改的项
     * @return true = 改成了（幂等时也成立）
     */
    bool setTableSpec(const TablePatch& patch);

    /**
     * 改光标所在表格的记录 + 重排 + 呈现。
     * @param patch 要改的项
     * @return true = 改成了
     */
    bool setTableSpecAndRender(const TablePatch& patch);

    /**
     * 当前水印（第一节的记录）。没有文档时返回默认（不画）。
     *
     * @return 水印记录
     */
    WatermarkSpec watermark() const;

    /**
     * 改水印：整份记录一起换，一步撤销。记录没变时不产生修订。
     *
     * @param w 新的水印记录
     * @return true = 改成了（幂等时也成立）
     */
    bool setWatermark(const WatermarkSpec& w);

    /**
     * 改水印 + 呈现。水印只画进页位图、不参与断行，所以只重光栅不重排 ——
     * 光栅签名里含水印摘要，改完所有页自动作废（见 rasterSignature）。
     *
     * @param w 新的水印记录
     * @return true = 改成了
     */
    bool setWatermarkAndRender(const WatermarkSpec& w);

    /**
     * 光标所在表格的现状（表格 / 行 / 格三层记录 + 整格选区的范围）。
     *
     * 属性面板一次读全：分三次问会拿到三个不同时刻的状态，而"表宽 + 行高 + 格对齐"
     * 本来就是同一张表单上的三组字段。光标不在表格里时 inTable 为假。
     *
     * @return JSON（键见 docs/api/payloads.json 的 tableState）
     */
    std::string tableStateJson() const;

    /**
     * 点所在的表格栅格：表的横向范围、列边界与这一页的行片段。
     *
     * 拖拽列边界 / 行边界要用它把指针位置换算成"哪一条线、拖到哪儿"。列边界是相对表左边
     * 的磅值（长度 = 列数 + 1），行片段是页内磅（一行跨页时每页各有一条）。
     *
     * @param x_pt_doc 文档坐标 x（磅）
     * @param y_pt_doc 文档坐标 y（磅）
     * @return JSON（键见 docs/api/payloads.json 的 tableGrid）
     */
    std::string tableGridJson(float x_pt_doc, float y_pt_doc) const;

    /**
     * 落点指示：这一点上"松手会把东西插在哪"的竖条矩形（页内像素）。
     *
     * 拖动搬运（选区或图片）时画它。只读 —— 不改光标、不动选区：正在被搬的那一段
     * 必须保持选中，落了光标就等于把它丢下，拖到一半东西就没了。
     *
     * @param x_pt_doc 文档坐标 x（磅）
     * @param y_pt_doc 文档坐标 y（磅）
     * @return JSON（键见 docs/api/payloads.json 的 dropSpot；没有落点时为 null）
     */
    std::string dropSpotJson(float x_pt_doc, float y_pt_doc) const;

    /**
     * 这一点是不是压在表格内部的某条格线上（拖列宽 / 行高用）。
     *
     * 只有"内部"格线算：表格的左、右、下三条边是表格自己的宽高，不是两条相邻列（行）
     * 之间的分界，拖它们没有"两边和不变"可说。判据与命中同一条 —— 命中在按下那一刻
     * 只算一次，格线判定跟着它一起回来。
     *
     * @param x_pt_doc 文档坐标 x（磅）
     * @param y_pt_doc 文档坐标 y（磅）
     * @param table 写回这张表的身份
     * @param col 写回列边界序号（0 表示没有；n 表示第 n 条内部列边界）
     * @param row 写回行边界序号（-1 表示没有；n 表示第 n 行与第 n+1 行之间）
     * @return true = 压在某条内部格线上
     */
    bool tableGridLineAt(float x_pt_doc, float y_pt_doc, uint64_t* table, int* col, int* row) const;

    /**
     * 拖拽结束：把一张表的全部列宽钉成显式值（<= 0 的那一列保持自动）。
     *
     * @param handle 表格身份（tableGridJson 给的 table）
     * @param widths 每列宽度（磅），按网格列序
     * @return true = 改成了
     */
    bool setTableColWidths(uint64_t handle, const std::vector<float>& widths);

    /**
     * 同上 + 重排 + 呈现。
     *
     * @param handle 表格身份
     * @param widths 每列宽度（磅）
     * @return true = 改成了
     */
    bool setTableColWidthsAndRender(uint64_t handle, const std::vector<float>& widths);

    /**
     * 拖拽结束：改一行的高度（<= 0 回到按内容）。
     *
     * @param handle 表格身份
     * @param row 逻辑行号
     * @param h_pt 新高度（磅）
     * @return true = 改成了
     */
    bool setTableRowHeight(uint64_t handle, int row, float h_pt);

    /**
     * 同上 + 重排 + 呈现。
     *
     * @param handle 表格身份
     * @param row 逻辑行号
     * @param h_pt 新高度（磅）
     * @return true = 改成了
     */
    bool setTableRowHeightAndRender(uint64_t handle, int row, float h_pt);

    /**
     * 一个点上压着什么（右键菜单的判定）。
     *
     * 优先级是"最具体的那一个"：图片（行内原子，画在最上层）→ 控件 → 表格 → 正文。
     * 图片先在前面是因为它压在文字上：点图的一半在字上，用户要的是图。
     *
     * @param x_pt_doc 文档坐标 x（磅）
     * @param y_pt_doc 文档坐标 y（磅）
     * @return JSON（键见 docs/api/payloads.json 的 contextTarget）
     */
    std::string contextTargetJson(float x_pt_doc, float y_pt_doc) const;

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
     * 文档里一张图片的几何与状态：宿主定位选中框、算拖拽落点、读尺寸都用它。
     */
    struct ImageBox {
        /**
         * 文档顺序的下标（selectImage / resizeImage 收它）。
         */
        int index = -1;

        /**
         * 节点 id（编辑之后仍然有效）。
         */
        NodeId id = kInvalidNodeId;

        /**
         * 资源 id（文档里存的那一个）。
         */
        std::string resource_id;

        /**
         * 它落在哪几页（跨页时多段），页内磅。
         */
        std::vector<PageRect> rects;

        /**
         * 显示宽度（磅）。
         */
        float w_pt = 0.0f;

        /**
         * 显示高度（磅）。
         */
        float h_pt = 0.0f;

        /**
         * 像素在不在资源表里（不在时画面上是占位框）。
         */
        bool resource_ready = false;

        /**
         * 版式档位：0 行内、1 四周型、2 上下型、3 衬于文字下方、4 浮于文字上方。
         * 宿主菜单的回显读它。
         */
        int layout = 0;
    };

    /**
     * 图片上的一次命中。
     */
    struct ImageHit {
        /**
         * 图片下标；-1 = 没点到图片。
         */
        int index = -1;

        /**
         * 把手序号（0 左上、1 上、2 右上、3 右、4 右下、5 下、6 左下、7 左）；-1 = 点在图上。
         */
        int handle = -1;
    };

    /**
     * 注册一张图片的像素。文档只存资源 id，换台机器打开时由宿主重新注册。
     *
     * 重复注册同一个 id 表示"这张图换了"：资源版本上调，页位图因此重光栅 ——
     * 文档与页指纹都没变，光栅签名里混了资源表的变更计数才判得出来。
     *
     * @param id 资源 id
     * @param width 像素宽
     * @param height 像素高
     * @param rgba 未预乘的 RGBA（行优先）
     * @param bytes 字节数（必须等于 width * height * 4）
     * @param error 写回失败原因（尺寸非法、字节数不符、超出上限）
     * @return true = 收下了
     */
    bool registerImage(const std::string& id, int width, int height, const uint8_t* rgba,
                       size_t bytes, std::string* error);

    /**
     * 注册像素 + 立刻重光栅呈现（宿主上传完图片就要看得见）。
     *
     * @param id 资源 id
     * @param width 像素宽
     * @param height 像素高
     * @param rgba 未预乘的 RGBA（行优先）
     * @param bytes 字节数
     * @param error 写回失败原因
     * @return true = 收下了
     */
    bool registerImageAndRepaint(const std::string& id, int width, int height, const uint8_t* rgba,
                                 size_t bytes, std::string* error);

    /**
     * 删掉一张图片的像素。文档里对它的引用不变，画面上退回占位框。
     * @param id 资源 id
     * @return true = 真的删掉了一张
     */
    bool dropImage(const std::string& id);

    /**
     * 删掉像素 + 重光栅呈现。
     * @param id 资源 id
     * @return true = 真的删掉了一张
     */
    bool dropImageAndRepaint(const std::string& id);

    /**
     * 资源表里的张数与字节数（诊断与容量核算用）。
     * @return 张数
     */
    int imageCount() const { return images_.count(); }

    /**
     * 资源表占的像素字节数。
     * @return 字节数
     */
    size_t imageBytes() const { return images_.bytes(); }

    /**
     * 在光标处插一张图片（行内原子）。光标摆到图片之后。
     *
     * 只在文档模式放行：插图是设计动作。插进去的图片落在当前段里，与文字同流，
     * 所以断行、跨页、撤销与增量排版全部走现成那条路。
     *
     * @param spec 图片记录（资源 id、原始像素尺寸与显示尺寸）
     * @return true = 插进去了
     */
    bool insertImage(const ImageSpec& spec);

    /**
     * 把显示尺寸按落点所在的单元格限宽。
     *
     * 落在格里的图按版心宽度排会横穿整张表（宿主只知道版心，格宽是排版算出来的）。
     *   超出时按比例缩到格宽，保持高宽比；高度是自动的时候不必改它，由排版按比例推。
     *
     * @param spec 调用方给的图片记录
     * @return 可能被收窄的记录（不在格里、或本来就不超宽时原样返回）
     */
    ImageSpec clampImageToContainer(const ImageSpec& spec) const;

    /**
     * 插图片 + 重排 + 呈现。
     * @param spec 图片记录
     * @return true = 插进去了
     */
    bool insertImageAndRender(const ImageSpec& spec);

    /**
     * 列出文档里全部图片（文档顺序）。
     * @return 每张图一条几何与状态
     */
    std::vector<ImageBox> images() const;

    /**
     * 按文档顺序取一张图的几何。
     * @param index 下标
     * @param out 写回几何
     * @return true = 下标有效
     */
    bool imageBox(int index, ImageBox* out) const;

    /**
     * 命中：文档坐标（磅）落在哪一张图的哪一部分上。
     *
     * 把手优先于图身：把手有一半长在图外，点在图外那一半上时若判成"没点到"，
     * 边角就拖不动。返回的序号与绘制时用的是同一份几何（见 imageHandleCenters）。
     *
     * @param x_pt_doc 文档坐标 x（磅）
     * @param y_pt_doc 文档坐标 y（磅）
     * @return 命中的图片与把手；没点到图片时 index 为 -1
     */
    ImageHit imageAtPt(float x_pt_doc, float y_pt_doc) const;

    /**
     * 拖拽移动的起手判定：这一点按下去能不能开始拖、拖的是什么。
     *
     * 不吃坐标而吃已经算好的命中结果：按下那一刻的每一件事（能不能拖、要不要落光标、
     * 落在哪一格的边距带上）都是同一个瞬间的问题，命中只该算一次。分成两条命令问，
     * 宿主就先问一条、再问一条，两次往返之后才轮到落光标 —— 拖选与把手都来不及起手。
     *
     * @param p 命中出来的位置；para 为 -1 表示这一点没有文字落点（例如只命中了图）
     * @param side 命中的侧别（与落点同一个身份）
     * @param img 同一时刻的图片命中（imageAtPt 的结果）
     * @return 0 不能拖 / 1 拖当前文字选区 / 2 拖已选中的那张图
     */
    int dragKindAt(const TextPos& p, int side, const ImageHit& img) const;

    /**
     * 选中一张图片（画面上出拖拽框与八个把手）。
     *
     * 图片选区与文本选区互斥：任何光标定位、选区改动、文档提交都会清掉它。
     * 只在文档模式放行 —— 表单与只读模式里点图片只是把光标落到它旁边。
     *
     * @param index 图片下标
     * @return true = 选中了
     */
    bool selectImage(int index);

    /**
     * 选中图片 + 呈现（拖拽框是瞬态层，必须重贴一次才看得见）。
     * @param index 图片下标
     * @return true = 选中了
     */
    bool selectImageAndRender(int index);

    /**
     * 取消图片选中。
     */
    void clearImageSelection();

    /**
     * 取消图片选中 + 呈现。
     * @return true = 之前选中着一张
     */
    bool clearImageSelectionAndRender();

    /**
     * 当前选中的图片下标；没有选中时 -1。
     * @return 图片下标
     */
    int selectedImage() const;

    /**
     * 改一张图片的显示尺寸（磅）。幂等：尺寸没变时返回 true 但不产生步骤。
     *
     * 尺寸落成绝对长度（作者意图），按像素与 DPI 折算的 Auto 就此被钉住 ——
     * 拖过把手之后那张图的显示尺寸不再随 DPI 变动，这是"我把它拖成这么大"的字面意思。
     *
     * @param index 图片下标
     * @param w_pt 新宽度（磅，必须为正）
     * @param h_pt 新高度（磅，必须为正）
     * @param continue_run 拖拽的后续每一帧传 true（并进上一步撤销）；第一帧传 false
     * @return true = 改成了（或者本来就是这个尺寸）
     */
    bool resizeImage(int index, float w_pt, float h_pt, bool continue_run = false);

    /**
     * 改尺寸 + 重排 + 呈现。图片尺寸改的是行盒几何，整段与后续内容都要跟着动。
     * @param index 图片下标
     * @param w_pt 新宽度（磅）
     * @param h_pt 新高度（磅）
     * @param continue_run 拖拽的后续每一帧传 true；第一帧传 false
     * @return true = 改成了
     */
    bool resizeImageAndRender(int index, float w_pt, float h_pt, bool continue_run = false);

    /**
     * 拖拽改尺寸时的预览：只记"松手会变成多大"，不动文档、不重排。
     *
     * 拖动过程中每一帧都真改尺寸的话，每帧都要重排 + 重光栅该页 —— 放大一张图时
     * 肉眼可见地卡，而且拖到一半看到的已经是最终画面，反而看不出框在跟手。
     * 这里只更新瞬态层上的那个框（与选中框同一层），松手时宿主再发一次 resizeImage。
     *
     * @param index 图片下标
     * @param w_pt 预览宽度（磅，必须为正）
     * @param h_pt 预览高度（磅，必须为正）
     * @param handle 正在拖的把手（0 左上顺时针）：决定框往哪个方向长
     * @return true = 预览变了（需要重贴一格）
     */
    bool previewImageResize(int index, float w_pt, float h_pt, int handle);

    /**
     * 收起尺寸预览（松手、换选区、文档提交时都要收）。
     * @return true = 收掉了一份确实存在的预览
     */
    bool clearImageResizePreview();

    /**
     * 预览 + 呈现（只重贴瞬态层，页位图仍按缓存）。
     * @param index 图片下标
     * @param w_pt 预览宽度（磅）
     * @param h_pt 预览高度（磅）
     * @param handle 把手序号
     * @return true = 预览变了
     */
    bool previewImageResizeAndRender(int index, float w_pt, float h_pt, int handle);

    /**
     * 丢掉拖动预览并让版面回到文档里的位置（取消拖动时用）。
     *
     * 拖动预览改的是版面本身（位移进了浮动图表），所以"丢掉预览"不是擦掉一个框，
     *   而是重排一次 —— 不重排的话图会停在松手前那一处，文档里却没有这个位移。
     * @return true = 确实有一份预览被丢掉
     */
    bool clearImagePreviewAndRender();

    /**
     * 拖动浮动图的预览：图按位移跟手，不动文档、不重排。
     *
     * 浮动图不占行盒，拖动它可以只挪这一张图的绘制位置（文字保持松手前的位置），
     * 于是拖动期间没有一次整篇重排。松手由宿主发一次 moveImage 落到文档，届时才绕排。
     * 行内图不走这条路：它在文字流里，挪一下就要重排。
     *
     * @param index 图片下标
     * @param dx_pt 相对松手前位置的横向位移（磅）
     * @param dy_pt 同上，纵向
     * @return true = 预览变了（需要重画那一页）
     */
    bool previewImageMove(int index, float dx_pt, float dy_pt);

    /**
     * 拖动预览期间这一张图的有效位置：叠加瞬态位移。
     *
     * 绘制、命中、选中框与自审都从这一处取位置 —— 各写一份的话，拖动中框会与图错开。
     * @param pf 已放置的浮动图
     * @param x 出参：有效左沿（页内磅），可为空
     * @param y 出参：有效上沿（页内磅），可为空
     */
    void imagePreviewPos(const PlacedFloat& pf, float* x, float* y) const;

    /**
     * 一个控件的对外描述：类型、值、选项与策略。
     *
     * 它是"给宿主看的那一份"，不是模型：不含节点指针，也不含设计期属性的完整记录。
     * 值在这里是快照 —— 要改值走 setControlValue，改完再读一次。
     */
    struct ControlView {
        /**
         * 文档顺序下标（toggleControl / setControlValue 收它）。
         */
        int index = -1;

        /**
         * 节点 id（编辑之后仍然有效）。
         */
        NodeId id = kInvalidNodeId;

        /**
         * 业务编码。
         */
        std::string key;

        /**
         * 业务类型（text / number / radioButton / ...）。
         */
        std::string controlType;

        /**
         * 值形态。
         */
        ValueShape shape = ValueShape::Text;

        /**
         * 当前值（文本类的值在内容里，见 content）。
         */
        ControlValue value;

        /**
         * 内容的纯文本（文本类的值就是它；其余是显示文字）。
         */
        std::string content;

        /**
         * 提示文字（值非空时不显示；只读与打印态也不画）。
         */
        std::string placeholder;

        /**
         * 内容不可编辑。
         */
        bool readonly = false;

        /**
         * 必填。
         */
        bool required = false;

        /**
         * 选项表（选择类与下拉；没有选项时为空）。
         */
        std::vector<ControlOption> options;

        /**
         * 下拉允许自由输入。
         */
        bool free_input = false;

        /**
         * 多选（复选 / 复选组）。
         */
        bool multi = false;

        /**
         * 单控件的标签（组控件为空：它的文案在选项表里）。
         */
        std::string label;

        /**
         * 互斥组名（空 = 不与任何人互斥）。
         */
        std::string group;

        /**
         * 选择类记号的颜色（alpha 0 = 跟随文字颜色）。
         */
        Color glyph_color;

        /**
         * 日期显示格式串（空 = 默认格式）。
         */
        std::string format;

        /**
         * 日期能选中哪些单位（位掩码，见 kDateUnitXXX）。
         */
        uint16_t date_units = kDateUnitsAll;
    };

    /**
     * 列出文档里全部控件（文档顺序）。
     * @return 每个控件一条描述
     */
    std::vector<ControlView> controlViews() const;

    /**
     * 改一个控件的定义（选项表、标签、互斥组、自由输入、日期格式与单位、业务编码）。
     *
     * 只改 patch 里给出的项；改完按新定义重建内容，并把指向已删选项的值一起清掉。
     * 只在文档模式放行 —— 改定义是设计动作。
     *
     * @param index 控件下标
     * @param patch 要改的项
     * @return true = 改了（幂等时也成立）
     */
    bool setControlSpec(int index, const ControlPatch& patch);

    /**
     * 改定义 + 重排 + 呈现。选项表变短会让内容变短，整段与后续内容都要跟着动。
     * @param index 控件下标
     * @param patch 要改的项
     * @return true = 改了
     */
    bool setControlSpecAndRender(int index, const ControlPatch& patch);

    /**
     * 改一个控件的值，并重建它的显示内容。
     *
     * @param index 控件下标
     * @param value 新值
     * @return true = 改了（幂等时也成立）
     */
    bool setControlValue(int index, const ControlValue& value);

    /**
     * 改值 + 重排 + 呈现。
     * @param index 控件下标
     * @param value 新值
     * @return true = 改了
     */
    bool setControlValueAndRender(int index, const ControlValue& value);

    /**
     * 切换一个控件的第 option 个选项（单控件传 -1）。互斥与多选规则在命令层落地。
     * @param index 控件下标
     * @param option 选项下标；单控件传 -1
     * @return true = 改了
     */
    bool toggleControl(int index, int option);

    /**
     * 切换 + 重排 + 呈现。
     * @param index 控件下标
     * @param option 选项下标；单控件传 -1
     * @return true = 改了
     */
    bool toggleControlAndRender(int index, int option);

    /**
     * 文档坐标（磅）落在某个组控件的第几个选项上。
     *
     * 只回答"点了哪个选项"：控件本身由 hitTestPt 的归属给出。不在组控件上时返回 -1。
     * @param x_pt_doc 文档坐标 x（磅）
     * @param y_pt_doc 文档坐标 y（磅）
     * @return 选项下标；不在组控件上为 -1
     */
    int controlOptionAtPt(float x_pt_doc, float y_pt_doc) const;

    /**
     * 同上，但落点已经由调用方量好（命中测试的同一次结果）。
     *
     * 宿主一次点击只会做一次命中：再来一次坐标查询等于把命中测试整个跑两遍。
     * @param p 落点（段 + 段内偏移）
     * @param side 落点侧别（hitTestPt 的同一份结果）
     * @return 选项下标；不在组控件上为 -1
     */
    int controlOptionAtPos(TextPos p, int side) const;

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
     * 已注册的字体清单，宿主按它填字体下拉框、按名字选字体。
     *
     * 名字由字体文件自己给（族名 / PostScript 名），不由宿主声明 —— 声明的那一份迟早
     * 与文件对不上，而"界面上写着思源黑体、排出来是别的字体"没有任何报错。
     *
     * @return JSON 数组，每项 { index, family, postScript, file, lazy }
     */
    std::string fontsJson() const;

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
     * 全文非空白码点数（界面上那个"字数"）。
     *
     * 空白不计（见 isSpaceCodepoint），控件内容与表格里的字都算 —— 它们就是流里的文字。
     * @return 字数
     */
    size_t documentWordCount() const;

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
                     Range selection_after, rev::MergeRun merge) override;

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
    void projectCaret(int para, size_t offset, int side, bool extend = false, int line_aff = -1);

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

        /**
         * 这一页上的图片拖拽框与把手（细边矩形）。与选区同一层：呈现时叠、导出时不叠 ——
         * 打印出来带一圈把手是错的。
         */
        std::vector<Rect> image_ui;
    };

    /**
     * "页位图上画出来是什么"的签名：换缩放、换页度量、换底色、换图片像素都算一次变化。
     * 签名一变，全部页位图作废（判据在 pageBitmapFresh）—— 新增光栅相关配置项时只要
     * 加进它，不必维护一张"记得清缓存"的清单。
     * @return 签名
     */
    uint64_t rasterSignatureNow() const;

    /**
     * 文档里的图片节点，按文档顺序。按修订代号缓存 —— 命中测试每次按下与拖动都会问它，
     * 而一次走树在两千段的文档上是几十微秒，拖选时按帧计就是白花的。
     * @return 图片节点 id（文档一改就重建）
     */
    const std::vector<NodeId>& imageNodeIds() const;

    /**
     * 撤销/重做之后把"选中的是哪张图"放回去：节点还在就重新选中，已删掉就什么都不做。
     *
     * 撤销改尺寸之后图仍该是选中的（否则拖动框消失、文本光标冒到图右边），而恢复选区
     * 那一步会清掉图片选中 —— 这一处把那个副作用按需抵消。
     * @param id 撤销之前选中的图片节点
     */
    void restoreImageSelection(NodeId id);

    /**
     * 这一页上的图片拖拽框与把手（页内像素矩形）。没选中图片、或不在文档模式时为空。
     * @param page 页号
     * @param out 写回矩形（追加前会清空）
     */
    void collectImageUi(int page, std::vector<Rect>* out) const;

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
     * 某一页上的表格片段摘要：表的位置、列边界与这一页各片段的上下沿。
     *
     * 它是"这一页的格线长什么样"的最小充分输入：绘制读的就是这几个数。
     * @param page 页号
     * @return 摘要；这一页没有表时等于 kFnvBasis
     */
    uint64_t tablePieceDigest(int page) const;

    /**
     * 一页上的浮动图摘要：几何（页内框）与环绕方式。
     *
     * 绘制读的就是这几个数（见 engine_paint.cpp 的 emitFloats）。不混图片内容 ——
     *   换资源由光栅签名里的资源代际负责，那一层会整窗作废。
     * @param page 页号
     * @returns 摘要（没有浮动图的页等于 FNV 基值）
     */
    uint64_t floatPieceDigest(int page) const;

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
     * 只把一页标成"与已画那一版不同"，其余页一个字节都不碰。
     *
     * 用在"只有这一页变了"的地方：衬于文字下方 / 浮于文字上方那种图不占版面，
     *   挪它只影响它盖住的那一页。按 markPagesChangedFrom 标的话，这一页之后
     *   每一页都会重光栅、每一页位图都回传一次。
     * @param page 页号（越界或负值忽略）
     */
    void markPageDirty(int page);

    /**
     * 把当前行指纹与行盒几何存成"已画进页位图的那一版"。
     * 与 page_fp_prev_ 同一时机调用：页像素变了就要跟着换。
     */
    void snapshotLineGeometry();

    /**
     * 一页上每一行的行带边界（页内磅，下标 = 行下标；只写这一页的行）。
     *
     * 行带按"块轴上相邻的行组"取中线，不按数组相邻项：表格一行的几个格 y 相同算一组，
     *   而高格的第二行之后数组里紧跟着的是另一格的第一行（在它上面）。见实现注释。
     * @param page 页号
     * @param lo 非空时写入各行带的上沿
     * @param hi 非空时写入各行带的下沿
     */
    void pageLineBands(int page, std::vector<float>* lo, std::vector<float>* hi) const;

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
     * 这个字形是不是选择类控件的记号（方框 / 圆圈那一个码点）。
     *
     * 判的是"它正好落在某个选项的记号位置上"，不是"这个字长得像圆圈"：正文里打的 ○
     * 照旧由字体画，只有控件里那一枚才由引擎按记号色重画。
     * @param pg 字形
     * @param checked 写回选中态
     * @param square 写回"方框还是圆圈"
     * @param color 写回记号色（alpha 0 = 跟随文字颜色）
     * @return true = 是记号
     */
    bool choiceMarkerOf(const PlacedGlyph& pg, bool* checked, bool* square, Color* color) const;

    /**
     * 画一枚选择类记号：方框（选中时带对勾）或圆圈（选中时圆心一个实心点），颜色取记号色。
     *
     * 两族都是自己画的，不走字体字形：字体里没有 ☑，而"方框里一个对勾"是这个控件唯一
     * 说得清的选中态；圆圈若借字体字形，它的直径取决于字体，与方框并排放着大小不一
     * （同一个字号下来回跳），颜色也只能跟正文。自绘之后两者共用同一个外径基准。
     * @param target 页位图
     * @param pg 字形（位置、字号、推进）
     * @param checked 选中态
     * @param square true = 方框（复选），false = 圆圈（单选）
     * @param color 记号色（alpha 0 = 用字形自己的颜色）
     * @param dy 页内纵向偏移（像素）
     */
    void paintChoiceMarker(Bitmap& target, const PlacedGlyph& pg, bool checked, bool square,
                           Color color, float dy) const;

    /**
     * 画水印（衬在正文下方的一行半透明文字，按节记录绘制）。
     *
     * 它不参与断行、不进流位置，所以只画进页位图；分带重画时按字形纵向位置筛。
     *
     * @param target 光栅目标（页位图）
     * @param page_index 页号（水印每页都有，页号只用于诊断）
     * @param band_lo_pt 带下界（磅），可为空
     * @param band_hi_pt 带上界（磅），可为空
     */
    void rasterWatermark(Bitmap& target, int page_index, const float* band_lo_pt,
                         const float* band_hi_pt);

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
     * 几何全部从排版的行盒派生（占位簇的位置 + 该簇的括号字体/行盒基线）：
     * 排版说占位在哪、用哪个字体，就画在哪 —— 空字段（一个字形都没有）也画得出来。
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
     * 浮动图的排版输入（编译产物）。非空时排版只能整篇重排 —— 浮动框的纵向位置取决于
     * 它前面排了多高，续排拿不到那个上下文（见 layoutDocumentRangeWithTables 的 floats）。
     */
    std::vector<LayoutFloat> floats_;

    /**
     * 叠加了拖动位移的那一份浮动图表（只在拖动浮动图时有内容）。
     *
     * 排版吃的是它，于是 placed_floats 直接就是预览位置 —— 文字当场绕开，不用等松手。
     *   复制一份而不是就地改 floats_：后者是编译产物，取消拖动要恢复原样。
     */
    std::vector<LayoutFloat> floats_preview_;

    /**
     * 本次排版要吃的浮动图表：拖动预览中返回叠加过位移的副本，其余时候返回编译产物。
     * @returns 浮动图表（引用，生命周期到下一次排版）
     */
    const std::vector<LayoutFloat>& floatsForLayout();

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
     * 图片资源表（宿主注册的像素）。文档只存 id，这里存字节。
     */
    ImageStore images_;

    /**
     * 图片节点表与它对应的修订代号。
     */
    mutable std::vector<NodeId> image_ids_;

    /**
     * image_ids_ 是按哪一版修订算出来的。
     */
    mutable uint64_t image_ids_gen_ = 0;

    /**
     * image_ids_ 算过没有（代号初值 0 与会话的初值撞车，单靠代号分不出"没算过"）。
     */
    mutable bool image_ids_valid_ = false;

    /**
     * 选中的图片（kInvalid = 没选中）。
     *
     * 它是一份对象选区，与文本选区互斥：光标一动、选区一改、文档一提交就清掉 ——
     * 两套选区同时存在的话，"按删除键删的是哪一段"没有答案。
     */
    NodeId image_sel_ = kInvalidNodeId;

    /**
     * 按下那一刻落在屏障里的位置（-1 = 没有）。
     *
     * 光标不能停在控件里，但选区可以从里面起手：按下的那一点先记下来，等拖选第一次搬
     * 焦点端时把它放回锚端，选区才从按下的那个字开始，而不是从控件边界开始。
     * 只活到那次搬运为止；别的光标写入都会清掉它（见 confineCaret）。
     */
    FlowPos drag_anchor_ = -1;

    /**
     * 拖拽改尺寸时的预览（瞬态，不进文档）。
     *
     * 为什么不在拖动过程中真的改尺寸：每一帧都提交一次会重排 + 重光栅该页，放大图时
     *   肉眼可见地卡。拖动只画框（这一份几何），松手才发一次真实的 resizeImage。
     * 只对选中的那一张有意义；松手、换选区、文档提交都清掉。
     */
    struct ImagePreview {
        /**
         * 预览中的图片节点（kInvalid = 没有预览）。
         */
        NodeId id = kInvalidNodeId;

        /**
         * 预览的显示尺寸（磅）。
         */
        float w_pt = 0.0f;

        /**
         * 同上，高度。
         */
        float h_pt = 0.0f;

        /**
         * 正在拖的把手序号（0 左上顺时针）：决定预览框往哪个方向长。
         */
        int handle = -1;

        /**
         * 拖动移动的横向瞬态位移（磅）。改尺寸预览不用这一项。
         */
        float dx_pt = 0.0f;

        /**
         * 同上，纵向。
         */
        float dy_pt = 0.0f;

        /**
         * 这一次预览是否已经由排版吃进去了。
         *
         * 拖动浮动图走的是"真重排"：位移写进排版输入，placed_floats 就是预览位置 ——
         *   绘制、命中、选中框全读那一边，不能再叠一次位移（叠两次图会飞出一倍远）。
         *   改尺寸预览则是"只画框不动版面"，那一份仍然靠叠加。
         */
        bool placed = false;
    };

    /**
     * 当前预览。
     */
    ImagePreview image_preview_;

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
     * 每页的表格片段摘要（下标 = 页号）。
     *
     * 页上的格线不是行盒画出来的，行指纹看不到"跨页切片的底边从行底挪到了页底"这种变化；
     * 而这类变化往往伴随某一行也在动，于是行带只盖住文字那一条，页底的边框留在旧位置。
     * 摘要变了的页一律整页重画（见 computeBand）。
     */
    std::vector<uint64_t> table_fp_new_;

    /**
     * 已经画成像素的那一版表格片段摘要。
     */
    std::vector<uint64_t> table_fp_prev_;

    /**
     * 已画那一版的页数（表格摘要数组的规模）。
     */
    int table_fp_prev_count_ = 0;

    /**
     * 每页的浮动图摘要（下标 = 页号）。
     *
     * 浮动图不占行盒，行带看不见它：把一张图从行内改成四周型，锚段那几行确实变了，
     *   可图本身落在行带之外 —— 行带只重画那一条，图上那一块留着旧像素（"图还在老
     *   位置"）；把图改回行内时同理，旧图永远擦不掉。摘要变了的页一律整页重画。
     */
    std::vector<uint64_t> float_fp_new_;

    /**
     * 已经画成像素的那一版浮动图摘要。
     */
    std::vector<uint64_t> float_fp_prev_;

    /**
     * 已画那一版的页数（浮动图摘要数组的规模）。
     */
    int float_fp_prev_count_ = 0;

    /**
     * 本次重画那条行带的页内磅范围（computeBand 写、rasterIntoBitmap 读）。
     * 放成员是为了不把两个 float 在 lambda 之间传来传去。
     */
    float band_lo_scratch_ = 0.0f;

    /**
     * 算行带时按页写下的逐行上沿（下标 = 行下标）。
     */
    std::vector<float> band_lo_scratch_all_;

    /**
     * 算行带时按页写下的逐行下沿（下标 = 行下标）。
     */
    std::vector<float> band_hi_scratch_all_;

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
