/**
 * 引擎度量：给宿主与取证用的可证伪计数。
 *
 * 字段的注释写的是"这个数字用来回答什么问题"，不是"它是什么" —— 名字已经说了是什么。
 * 每个数字背后都有一条判据，删掉一个字段往往意味着删掉一条判据，改之前先看注释。
 */
#pragma once

#include <cstddef>

namespace heditor {

struct EngineStats {
    int page_count = 0;
    int glyph_count = 0;
    int placed_line_count = 0;
    int cluster_count = 0;
    int face_count = 0;
    int font_instance_count = 0;
    /**
     * 字体回退取证：跑完整条链后仍是 .notdef 的字形数 / 切出的 run 数。
     */
    int missing_glyph_count = 0;
    int font_run_count = 0;
    /**
     * 字体按需读的账：命中、未命中、落存储的次数与字节、存储耗时、当前常驻。
     *
     * 用途是判"按需是否走得通"：命中率高说明块缓存真的在挡，落存储的次数应当远小于
     * 字形数；常驻字节是这条路换来的收益（整份装载时恒为零，因为没有数据源）。
     */
    long long font_cache_hits = 0;
    long long font_cache_misses = 0;
    long long font_storage_reads = 0;
    long long font_storage_bytes = 0;
    double font_io_ms = 0.0;
    long long font_resident_bytes = 0;
    /**
     * 存储读真失败的次数。与 font_storage_eofs 分开：文件尾的短读不算失败。
     *
     * 这两个数存在的理由是把"静默"变成"可见"：失败与文件尾用同一个零字节返回值时，
     * 字体读不到在页面上只表现为某些字不出现，而从任何统计里都看不出来。
     */
    long long font_storage_failures = 0;
    /**
     * 因请求越过文件尾而短读的次数（正常终止，不是失败）。
     */
    long long font_storage_eofs = 0;
    double shape_ms = 0.0;
    double layout_ms = 0.0;
    double paint_ms = 0.0;
    /**
     * 页指纹记账的耗时（每次重排后都要算一遍）。
     *
     * 单独拎出来是因为它不随排版长短变化：一次 0.06 ms 的小编辑，页指纹可能还要 0.2 ms，
     * 而它是 O(字形总数) 的。混进 layout_ms 就会得出"增量排版没省多少"的错误结论，
     * 只看 layout_ms 又会漏掉一半成本。增量化之后（只重算受影响页）这个数应当随脏页数走，
     * 而不是随文档大小走。
     */
    double page_fp_ms = 0.0;
    /**
     * 绘制的三段分解。存在的理由：它是"要不要引入 GPU / 第三方光栅器"的判据。
     * 实测（4 页、1022 字形、帧面积从 0.51 Mpx 到 8.12 Mpx 共 16 倍）：
     * paint 只从 110 ms 涨到 173 ms（1.57 倍）—— 成本不随面积走。
     * 所以"加像素吞吐"（GPU、更快的 fill/memcpy）在这里是打空的；
     * 必须先把这三段分开，才知道钱到底花在哪一段。
     */
    double paint_fill_ms = 0.0;   // 铺面积：页间隙背景 + 页底 + 页边框
    double paint_glyph_ms = 0.0;  // 字形：hb 抽轮廓 + 扫描线光栅（或命中字形缓存后只做 blit）
    /**
     * 字形那一段里"只为记账取包围盒"花掉的时间。
     *
     * 它是 paint_glyph_ms 的一部分（记账在 drawGlyphs 内部），单列出来是因为这一笔
     *   不画任何像素：要判断"字形贵"还是"记账贵"，只能看这两个数谁大。
     * 自写后端不取包围盒，恒为 0。
     */
    double paint_glyph_bounds_ms = 0.0;
    double paint_present_ms = 0.0;  // 帧缓冲搬移：reset + 从底图贴回 + 光标

    /**
     * 上面那一笔的六段分解。它是"paint 减掉铺面积与字形"，所以只是个残差 ——
     * 残差里装着六件不相干的事，合成一个数看不出该动哪一块，只能分开量。
     *
     * 分开量之后答案很干净：2000 段文档上那 2.0 ms 全落在"覆盖层"那一段（空转呈现
     *   时铺面积/字形/帧贴回/记账都是 0.00），根因是表单标记遍历 layout_.lines 全篇
     *   （4005 行、每行再过一遍簇），与窗口无关 —— 1 页文档同一条路径只有 0.00 ms。
     *   上一轮"读代码挑嫌疑犯"挑中了 snapshotLineGeometry，加判据重编后纹丝不动；
     *   这就是那条纪律：先插计时点分开量，别读代码猜。
     * 六段两两首尾相接，并起来正好是 paint_ms；写底图那两段（铺面积、字形）
     *   包在 paint_swap_ms 里面，不是与它并列。
     */
    double paint_pre_ms = 0.0;      // 入口准备：字形缓存预算 + 两份记账清零 + 几何取值
    double paint_swap_ms = 0.0;     // 换窗判断与页循环：写底图、平移复用、新露那条带
    double paint_blit_ms = 0.0;     // 帧贴回：把脏矩形从底图搬进帧
    double paint_overlay_ms = 0.0;  // 覆盖层：选区混合、光标填充、表单标记
    double paint_ledger_ms = 0.0;   // 记账：脏矩形并集、脏页清单、待上屏框
    double paint_report_ms = 0.0;   // 上报：光标数字、表单归属、排版与文档统计
    /**
     * 本轮光栅到底图的字形个数。与 paint_glyph_ms 配对：一个说花了多久、
     * 一个说画了几个字 —— 判"字形光栅贵不贵"要看两者的商，
     * 只看总耗时是分不出"字多"与"每个字贵"的，而这两者的修法完全不同。
     * 记底图不记帧：字形只画在底图上，帧那边是从底图搬像素过来的。
     */
    int painted_glyphs = 0;

    /**
     * 字形缓存的占用与上限（字节）。Skia 的 strike 缓存是进程内全局一份，
     * 上限由 EditorEngine::ensureRenderSetup 设（见 kGlyphCacheBudgetBytes）。
     * 这两个数回答的是：
     *   占用贴着上限 ⇒ 随时会被整份抹掉，"预热了却不见变快"的成因就在这里。
     */
    size_t glyph_cache_bytes = 0;
    size_t glyph_cache_limit = 0;
    double total_ms = 0.0;
    int frame_width = 0;
    int frame_height = 0;
    float zoom = 1.0f;
};

/**
 * 一次编辑/呈现的可证伪计数。
 * 这一批数字存在的唯一理由：让"到底有没有做增量"变成一个可以当场证伪的
 * 事实，而不是一句"感觉挺流畅"。
 * 两层增量各有自己的判据：
 * 光栅（页指纹决定重画哪几页）看 repainted_pixels / repainted_pages；
 * 排版（段级续排）看 relaid_paragraphs / relaid_lines 与 layout_was_full ——
 * 在全量重排时 relaid_lines 等于 total_lines，做增量之后才真的小于它。
 *
 * 字段由产生它的那一步发布，不统一挂在呈现上：排版账在排版返回时写好，文档规模在
 * 块流重建时写好，呈现只写它自己产生的那些（像素、帧、光标）。挂在呈现上时，
 * 编辑之后、呈现之前读到的是上一轮，而多一次呈现又会把刚写好的数字抹掉。
 */
struct EditStats {
    /**
     * 布局
     * layout_ran 回答"本轮有没有重排"：只有呈现能回答，所以按拍结算。两次呈现之间
     * 没有别的结算点，因此 relayout() 也直接置位。
     * 其余几格是最近一次排版的账，由 relayout() 发布、呈现不改写。读的时候两者要一起看：
     * layout_ran 为 false 时，这些数字描述的是上一轮 —— 它们不是假账，只是不属于这一拍。
     */
    bool layout_ran = false;
    int relaid_paragraphs = 0;
    int reused_paragraphs = 0;  // 后缀被复用的段数（没重排）
    /**
     * 后缀是平移复用的（不是原样搬回）：行结构变了，于是后缀整体沿块轴挪了位置、
     * 页码与全局行号重编过，但断行结果与字形的行内位置一个字节都没重算。
     * 与 reused_paragraphs 一起看才有意义：复用 99 段 + 平移 = "99 段没重排，
     * 但它们的位置变了"，面板据此解释"为什么脏页比 1 多"。
     */
    bool suffix_shifted = false;
    int total_paragraphs = 0;
    int relaid_lines = 0;
    int total_lines = 0;
    bool layout_was_full = false;
    int dirty_page_count = 0;  // 内容指纹变了、需要重画的页数

    /**
     * 重建树产物（每次提交都做）的耗时拆分。
     *
     * 四项都是 O(文档)，而排版已经是增量的 —— 所以这几个数是"编译这一层有没有做到增量"的
     * 可证伪证据：真增量之后它们该随改动规模走，不是随文档长度走。
     * 拆到四项而不是合成一个数：3000 段实测下它们各有量级（流索引比编译还贵），
     * 合成一个就分不清该优化谁。
     */
    double rebuild_compile_ms = 0.0;
    /**
     * 本轮有多少块是从上一轮整块搬过来的（不重算文本与格式）。
     *
     * 报它而不是只报耗时：耗时会被更快的机器掩盖，这个数不会 —— "一次编辑之后 1999 块
     * 原样复用"是结构判据，"投影用了 0.4 ms"只是这一次的运气。
     */
    int rebuild_reused = 0;
    double rebuild_flow_ms = 0.0;     // FlowIndex 重建（按节点记流位置）
    double rebuild_flowpos_ms = 0.0;  // 段的流区间递推
    double rebuild_copy_ms = 0.0;     // 块流副本（排版吃的 blocks_ 那一份）
    /**
     * 这一轮产物是怎么重建的，以及局部路径被否决的原因。
     *
     * 没有这两格就只能看耗时反推走了哪条路，而耗时在同一台机器上能差一倍（同一段代码
     *   两次量到 1.2 与 2.0 ms）—— 那不足以分清"走到了局部路径但没省下来"与"根本没走到"。
     * path 取 0 = 整树编译、1 = 只编一段、2 = 分段局部重建；
     * reject 是分段局部重建里第几条判据否决的（0 = 没有否决），编号与实现里的顺序一致。
     */
    int rebuild_path = 0;
    int rebuild_reject = 0;
    double rebuild_controls_ms = 0.0;  // 控件视图导出

    /**
     * 上屏粒度：本次真正重光栅的页数，以及文档总页数。
     *
     * total_pages 是文档页数，不是窗口里那几页 —— 呈现只保证视口窗口内的页是新鲜的
     * （见 EditorEngine::setViewportAnchor），而"脏页 / 总页"这个比值的分母必须是整篇，
     * 否则它会恒等于 1，读起来像"只有一页脏"。窗口里那几页看 window_pages。
     */
    int repainted_pages = 0;
    int total_pages = 0;
    /**
     * 本次重画的那条行带（页内像素 y；band_y1 < 0 = 本轮没走行带路径）。
     * 与 repainted_pixels 一起看：后者说"要传多少"，这两个说"传的是哪一条"。
     * 页粒度重画时它就是整页（y0=0、y1=页高）。
     */
    int band_y0 = -1;
    int band_y1 = -1;
    /**
     * 帧（= 宿主那张画布）覆盖的页数，以及它的首页。
     */
    int window_pages = 0;
    int window_first_page = 0;
    /**
     * 本次要上屏的像素数（各页脏矩形面积之和，合并之后的），以及它们在窗口坐标下的包围盒。
     *
     * 口径是"宿主得重传多少"，与 pageDirtyRects 完全一致 —— 旧版记的是"帧缓冲写了多少次"，
     * 那个数随渲染目标一起消失了。上界仍然成立：它证明的是"真的没写整页"。
     */
    long long repainted_pixels = 0;
    int repainted_x = 0;
    int repainted_y = 0;
    int repainted_w = 0;
    int repainted_h = 0;
    int frame_w = 0;
    int frame_h = 0;

    /**
     * 耗时拆分（毫秒）：最近一次排版、本轮页指纹、本轮绘制，以及这一拍的总计。
     *
     * 页指纹算进 total_ms：它和排版同属一次操作。10000 段上它是 21 ms，比排版本身
     * 贵一个数量级 —— 漏掉它，总计就与感受到的卡对不上，也看不出该优化哪一段。
     * 总计只累加这一拍真正发生过的段：没有重排时不加排版与页指纹 —— 排版账是
     * 最近一次的，直接相加会让光标闪烁每 530ms 重复上报一次排版耗时。
     */
    double layout_ms = 0.0;
    double page_fp_ms = 0.0;
    double paint_ms = 0.0;
    double total_ms = 0.0;

    /**
     * 光标位置与文档规模（逻辑坐标，与视觉行列无关）。
     */
    int caret_para = 0;
    size_t caret_offset = 0;  // 段内码点偏移
    /**
     * 光标所在页（绝对页号；-1 = 找不到行盒）。
     * 帧只装窗口内的页之后，这是"光标在不在此刻的窗口里"的唯一判据 ——
     * 不在窗口里则光标画不出来、点也点不中，宿主据此挪窗口。
     */
    int caret_page = -1;
    size_t caret_index = 0;  // 全文码点下标
    /**
     * 光标侧别：在这个偏移上跨过了几枚边界括号（0 = 在所有括号之前）。
     * 与 caret_offset 合起来才是光标的完整位置 —— 同一个偏移上有好几个插入点。
     */
    int caret_side = 0;
    /**
     * 光标是否落在竖排的段里。方向键的物理映射由它决定：竖排的行内轴是竖直的，
     * 上下走字、左右换列（见宿主的 keyToOp）。
     */
    bool caret_vertical = false;
    /**
     * 光标所在段的列序是否反向（中日竖数列自右向左为真，蒙文为假）。
     * 方向键的左右换列靠它定方向：两种竖排的列序相反，只按"竖排"映射会把蒙文按反。
     */
    bool caret_block_reversed = false;
    size_t doc_codepoints = 0;
    bool caret_visible = false;
    /**
     * 光标在帧像素坐标下的矩形。宿主据此摆 IME 候选框、
     * 或把原生 textarea 覆盖层挪到光标处（档 3）。
     */
    int caret_x_px = 0;
    int caret_y_px = 0;
    int caret_w_px = 0;
    int caret_h_px = 0;

    /**
     * 表单（区间标注 + 编辑策略）
     * blocked_edits 是累计值，其余是本轮快照。
     * 它存在的理由：策略生效时编辑会静默返回 false，
     * 宿主看到的现象是"打字没反应" —— 那和"程序卡了/坏了"在观感上无法区分。
     * 有了这个计数，"按了 5 次键一个字没进去"就有了一个可断言的说法：
     * 文档长度不变 且 blocked_edits += 5。
     */
    int edit_mode = 0;  // 0=文档模式 1=表单模式 2=只读模式
    int control_count = 0;
    int caret_control = -1;  // 光标最内层所在的控件；-1 = 不在任何控件内
    long long blocked_edits = 0;
};

}  // namespace heditor
