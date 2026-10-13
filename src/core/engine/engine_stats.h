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
    double paint_fill_ms = 0.0;     // 铺面积：页间隙背景 + 页底 + 页边框
    double paint_glyph_ms = 0.0;    // 字形：hb 抽轮廓 + 扫描线光栅（或命中字形缓存后只做 blit）
    double paint_present_ms = 0.0;  // 帧缓冲搬移：reset + 从底图贴回 + 光标

    /**
     * 字形缓存取证
     * 这两个数字是"字形缓存到底有没有用"的唯一判据，缺一不可：
     * 命中率高 + 耗时降   → 缓存吃掉了真正的成本；
     * 命中率高 + 耗时没降 → 瓶颈根本不在字形光栅，别再往这儿投资；
     * 命中率低            → 亚像素相位太散（键设计问题），
     * 这时"再快的光栅器"能帮的也有限。
     */
    long long glyph_cache_hits = 0;
    long long glyph_cache_misses = 0;
    int glyph_cache_entries = 0;
    double total_ms = 0.0;
    int frame_width = 0;
    int frame_height = 0;
    float zoom = 1.0f;
};

/**
 * 一次编辑/呈现之后的可证伪计数。
 * 这一批数字存在的唯一理由：让"到底有没有做增量"变成一个可以当场证伪的
 * 事实，而不是一句"感觉挺流畅"。
 * 两层增量各有自己的判据：
 * 光栅（页指纹决定重画哪几页）看 repainted_pixels / repainted_pages；
 * 排版（段级续排）看 relaid_paragraphs / relaid_lines 与 layout_was_full ——
 * 在全量重排时 relaid_lines 等于 total_lines，做增量之后才真的小于它。
 */
struct EditStats {
    /**
     * 布局
     * 这一组只在"本轮真的重排过"时才有意义。
     * layout_ran 是那个开关：光标移动、光标闪烁这两条路径不重排，
     * 此时 relaid_lines / layout_ms 都必须是 0 —— 否则面板上会出现
     * "移动光标重排了 12 行、耗时 3ms"这种假账（数字是真的，归属是错的）。
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
    double rebuild_flow_ms = 0.0;      // FlowIndex 重建（按节点记流位置）
    double rebuild_flowpos_ms = 0.0;   // 段的流区间递推
    double rebuild_copy_ms = 0.0;      // 块流副本（排版吃的 blocks_ 那一份）
    double rebuild_controls_ms = 0.0;  // 控件视图导出

    /**
     * 上屏粒度：本次真正重画的页数，以及文档总页数。
     *
     * total_pages 是文档页数，不是帧里那几页 —— 帧只装视口窗口（见
     * EditorEngine::setViewportAnchor），而"脏页 / 总页"这个比值的分母必须是整篇，
     * 否则它会恒等于 1，读起来像"只有一页脏"。帧里那几页看 window_pages。
     */
    int repainted_pages = 0;
    int total_pages = 0;
    /**
     * 帧（= 宿主那张画布）覆盖的页数，以及它的首页。
     */
    int window_pages = 0;
    int window_first_page = 0;
    /**
     * 本次真正写过的像素数（由 Bitmap 记账，非上层估算），以及写入区域的包围盒（设备像素）。
     */
    long long repainted_pixels = 0;
    int repainted_x = 0;
    int repainted_y = 0;
    int repainted_w = 0;
    int repainted_h = 0;
    int frame_w = 0;
    int frame_h = 0;

    /**
     * 耗时拆分（毫秒）：本次排版、本次绘制、以及这一次操作的总计。
     */
    double layout_ms = 0.0;
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
    size_t caret_index = 0;   // 全文码点下标
    /**
     * 光标侧别：在这个偏移上跨过了几枚边界括号（0 = 在所有括号之前）。
     * 与 caret_offset 合起来才是光标的完整位置 —— 同一个偏移上有好几个插入点。
     */
    int caret_side = 0;
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
