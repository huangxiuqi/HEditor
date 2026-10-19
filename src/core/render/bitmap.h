/**
 * 光栅目标（帧缓冲）。
 *
 * 只有一个实现 bitmap_skia.cpp：绘制经 SkCanvas、字形经 SkFont 与 FreeType，
 * 像素落在同一块 RGBA 缓冲里（frame / frameBytes / copyRectFrom 的契约不变）。
 * 自写扫描线光栅器已退役 —— 两套后端对同一条排版结果不可能逐像素一致，
 * 跨后端比较没有判据意义；合成一套之后 native 单测跑的就是线上那条路径。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "core/text/font.h"
#include "core/utils/types.h"

namespace heditor {

class Bitmap {
public:
    Bitmap();
    explicit Bitmap(int w, int h);
    ~Bitmap();
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;
    Bitmap(Bitmap&& other) noexcept;
    Bitmap& operator=(Bitmap&& other) noexcept;

    void reset(int w, int h);
    void clear(Color c);

    int width() const { return w_; }
    int height() const { return h_; }
    size_t byteSize() const { return px_.size(); }
    uint8_t* data() { return px_.data(); }
    const uint8_t* data() const { return px_.data(); }

    void fillRect(const Rect& r, Color c);
    void strokeRect(const Rect& r, Color c, float thickness);

    /**
     * 整幅按行平移 dy 像素（dy > 0 向下、< 0 向上），露出来的那一条填零。
     *
     * 为什么要有它：视口窗口整体挪 k 页时，帧与底图里重叠的那几页像素其实是同一批，
     *   只是位置变了。搬一次 memmove 就能把它们留住，而重新光栅一遍要付整页的
     *   字形钱（3 页量到 32 ms，其中字形 15 ms）。露出来的那一条是垃圾，
     *   调用方必须紧接着把它写掉（贴新页），不能直接上屏。
     * 实现放头文件是因为它与光栅后端无关（只是搬字节），而两个后端只有一个会被
     *   编进目标 —— 放在 .cpp 里会出现"native 有、wasm 没有"的链接失败。
     * 记账（painted_px_）不动：它搬的是像素、不是"画"像素。
     */
    void shiftVertical(int dy) {
        if (dy == 0 || w_ <= 0 || h_ <= 0) return;
        const size_t row_bytes = static_cast<size_t>(w_) * 4u;
        uint8_t* p = px_.data();
        if (dy >= h_ || dy <= -h_) {
            std::memset(p, 0, row_bytes * static_cast<size_t>(h_));
            return;
        }
        if (dy > 0) {
            std::memmove(p + row_bytes * static_cast<size_t>(dy), p,
                         row_bytes * static_cast<size_t>(h_ - dy));
            std::memset(p, 0, row_bytes * static_cast<size_t>(dy));
        } else {
            const int d = -dy;
            std::memmove(p, p + row_bytes * static_cast<size_t>(d),
                         row_bytes * static_cast<size_t>(h_ - d));
            std::memset(p + row_bytes * static_cast<size_t>(h_ - d), 0,
                        row_bytes * static_cast<size_t>(d));
        }
    }

    /**
     * 按 c.a 的透明度把颜色混进 r 覆盖的区域（保留底下已经画好的内容）。
     *
     * 与 fillRect 分开的理由只有一条：选区是盖在文字上的，实心填充会把字一起抹掉，
     * 而"选中之后字还看得见、只是泛蓝"才是选区的样子。混合按整数算并四舍五入，
     * 不同路径（缓存 / 直接光栅）走的是同一段代码，所以帧仍然逐字节可比。
     * 结果的 alpha 保持不透明：位图是最终画面，不是图层。
     */
    void blendRect(const Rect& r, Color c);

    /**
     * 把 src 中 r 覆盖的区域原样拷贝到本图的同一位置。
     * 用途：帧缓冲 = 底图（base）+ 光标。光标移动时只需把"旧光标矩形"和
     * "新光标矩形"两块从底图贴回来，再画新光标 —— 这就是局部重绘的全部机制。
     * 尺寸不一致时直接不动（宁可不画，也不能写错行跨度）。
     */
    void copyRectFrom(const Bitmap& src, const Rect& r);

    /**
     * 真实写入量取证
     * 为什么必须由光栅器自己记账，而不是让上层"估算脏矩形面积"：
     * 估算是意图，记账是事实。两者在不一致的实现里会差出量级，
     * 而"到底有没有真做增量"正是靠这个数字来证伪的。
     * drawGlyph 只记实际写下去的像素（覆盖率 > 阈值的那些），不按包围盒虚增。
     * 但它是写入次数，不是去重后的脏面积：同一块像素被写两遍就记两次。
     * 所以它可能略大于 w×h，这是设计如此 —— 见 engine.cpp 记账处的两条恒等式。
     * 用途决定了口径：它要证明的是"真的没写整页"，而写入次数是上界，
     * 上界都小两个数量级，结论才硬。
     */
    size_t paintedPixels() const { return painted_px_; }
    void resetPaintedPixels() {
        painted_px_ = 0;
        painted_glyphs_ = 0;
        glyph_bounds_ms_ = 0.0;
    }

    /**
     * 本次提交里"只为了记账"花掉的时间（毫秒）。
     *
     * 记账本身不画任何像素：它要把每个字形的包围盒取出来，好让 paintedPixels 有个上界。
     *   而取包围盒在 Skia 侧要逐字形进一遍字形缓存（自写后端不记包围盒，恒为 0）。
     * 单列这一项的理由：它和"画字"混在同一个 drawGlyphs 里，合成一个数就永远分不清
     *   "字形贵"与"记账贵"—— 而这两者的修法正好相反（一个要动光栅，一个要动记账口径）。
     */
    double glyphBoundsMs() const { return glyph_bounds_ms_; }

    /**
     * 本次提交给光栅器的字形个数。
     *
     * 与 paintedPixels 配对使用：一个说"写了多少像素"，一个说"画了几个字"。
     * 判"字形光栅贵不贵"要看它们的商（每字形多少微秒）——
     *   只看总耗时是分不出"字多"与"每个字贵"的，而这两者的修法完全不同。
     */
    size_t paintedGlyphs() const { return painted_glyphs_; }

    /**
     * 画一个字形。
     * pen_x / pen_y  设备像素坐标下的"字形原点"（横排=基线起点；竖排=竖排原点）
     * units_to_px    从 HarfBuzz scale 单位到设备像素的换算系数
     * rotate_deg     绕字形原点顺时针旋转的角度。0=正立；90=侧倒（蒙文竖排）。
     * 旋转必须在这里做：字体的轮廓是横排形态，
     * "整段侧倒"属于呈现层，不属于整形层。
     */
    void drawGlyph(hb_font_t* font, uint32_t glyph_id, float pen_x, float pen_y, float units_to_px,
                   Color color, float rotate_deg = 0.0f);

    /**
     * 批量画字形：一次调用画一整段（同字体、同颜色、同朝向）。
     *
     * 为什么要有批量入口：光栅器每画一个字形都要付一笔固定开销 —— Skia 侧是
     * "新建 SkPaint、装配一遍字形运行、进字形缓存"，自写侧是"查缓存键"。单字形
     * 调用时这笔全摊在一个字上：一页约 1200 个字形实测要 10 ms 上下，大头正是它。
     *
     * ids / xs / ys 都是长度 count 的数组；xs / ys 是设备像素坐标下的字形原点。
     * 调用方负责把同一 font / color / rotate 的字形攒成一段，且不要打乱顺序：
     * 数组里的次序就是绘制次序，重叠的字形（组合符、括号贴内容）靠它决定压盖。
     * rotate_deg != 0（侧倒竖排）时本实现退化成逐字形 —— 旋转是每个字形各绕自己的
     * 原点转，一次调用表达不了，而那条路很少见。
     */
    void drawGlyphs(hb_font_t* font, const uint32_t* ids, const float* xs, const float* ys,
                    int count, float units_to_px, Color color, float rotate_deg = 0.0f);

    /**
     * 与另一块 Bitmap 共用字体表（Skia 侧的 SkTypeface / SkFont）。
     *
     * 为什么非共用不可：Skia 的字形缓存按 strike 规格索引，而规格里含字体对象的身份。
     *   两块 Bitmap 各建一份 SkTypeface，缓存就分成互不相见的两份 —— 往其中一份预热
     *   对另一份毫无帮助，而症状只是"没变快"，什么都看不出来。
     */
    void shareFonts(const Bitmap& other);

    /**
     * 字形 mask 缓存的字节预算（0 = 不动，用后端自己的默认值），以及当前的占用与上限。
     *
     * 为什么要这条口子：Skia 的全局 strike 缓存默认上限只有 2 MB，而它清理时是按
     *   "整份 strike"删的（SkStrikeCache::internalPurge 从 LRU 尾巴上摘对象，一次至少
     *   放掉已用的四分之一）。本项目里通常只有一两个字体规格，也就是只剩下一个 strike
     *   —— 一旦越过 2 MB，清理就把整份缓存一次抹掉，下一个字形又从头冷光栅。
     *   对一整页上千个汉字、每页都要重光栅的滚动场景，这条等于"缓存从不起作用"；
     *   而它的症状只是"没变快"，看不出来是哪一层的问题。
     */
    void setGlyphCacheBudget(size_t bytes);
    size_t glyphCacheUsedBytes() const;
    size_t glyphCacheBudgetBytes() const;

private:
    int w_ = 0;
    int h_ = 0;
    std::vector<uint8_t> px_;
    size_t painted_px_ = 0;      // 自上次 resetPaintedPixels() 以来实际写入的像素数
    size_t painted_glyphs_ = 0;  // 同一口径下的字形个数（见 paintedGlyphs）
    double glyph_bounds_ms_ = 0.0;  // 同一口径下"记账取包围盒"花掉的时间（见 glyphBoundsMs）

    /**
     * Skia 后端状态。用 pimpl 有两个理由：bitmap.h 是公共头，每个 include 它的
     * 编译单元不该被迫吃进 Skia 的头与它那套编译宏；且 Skia 的 typeface/font
     * 缓存是可增长容器，放进头里会让 Bitmap 的定义随 Skia 版本变化（ABI 不稳）。
     */
    struct SkiaImpl;
    struct SkiaFonts;
    std::unique_ptr<SkiaImpl> sk_;
};

}  // namespace heditor
