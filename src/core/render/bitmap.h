/**
 * 光栅目标（一张 RGBA 位图：页位图、导出图、审计参照都是它）。
 *
 * 只有一个实现 bitmap_skia.cpp：绘制经 SkCanvas、字形经 SkFont 与 FreeType，
 * 像素落在同一块 RGBA 缓冲里（frame / frameBytes 的契约不变）。
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

    /**
     * 造一块空位图（0 x 0）。
     */
    Bitmap();

    /**
     * 按尺寸造一块位图，像素清零。
     * @param w 宽（设备像素）
     * @param h 高（设备像素）
     */
    explicit Bitmap(int w, int h);

    /**
     * 释放像素与后端状态。
     */
    ~Bitmap();
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;

    /**
     * 移动构造：接管对方的像素与后端状态，对方置空。
     * @param other 被移动的对象
     */
    Bitmap(Bitmap&& other) noexcept;

    /**
     * 移动赋值：先释放自己的，再接管对方的。
     * @param other 被移动的对象
     * @return *this
     */
    Bitmap& operator=(Bitmap&& other) noexcept;

    /**
     * 改尺寸并按新尺寸清空。
     * @param w 宽（设备像素）
     * @param h 高（设备像素）
     */
    void reset(int w, int h);

    /**
     * 整块填成一种颜色。
     * @param c 颜色
     */
    void clear(Color c);

    /**
     * 位图宽度。
     * @return 宽（设备像素）
     */
    int width() const { return w_; }

    /**
     * 位图高度。
     * @return 高（设备像素）
     */
    int height() const { return h_; }

    /**
     * 像素数据的字节数（宽 x 高 x 4）。
     * @return 字节数
     */
    size_t byteSize() const { return px_.size(); }

    /**
     * 像素数据（RGBA，行优先）。
     * @return 可写指针
     */
    uint8_t* data() { return px_.data(); }

    /**
     * 像素数据（只读）。
     * @return 只读指针
     */
    const uint8_t* data() const { return px_.data(); }

    /**
     * 实心填充一个矩形。
     * @param r 矩形（设备像素）
     * @param c 颜色
     */
    void fillRect(const Rect& r, Color c);

    /**
     * 画一个矩形边框。
     * @param r 矩形（设备像素）
     * @param c 颜色
     * @param thickness 线宽（设备像素）
     */
    void strokeRect(const Rect& r, Color c, float thickness);

    /**
     * 把一块 RGBA 像素按目标矩形缩放绘制（行内图片走这一条）。
     *
     * key 与 version 只服务后端缓存：同一个 key 换了 version 就重建那一份解码结果。
     * 传 key 为 0 表示不缓存。坐标是设备像素，越界由画布自带的裁剪兜住。
     * @param key 资源身份（0 = 不缓存）
     * @param version 资源版本
     * @param rgba 像素（预乘 alpha，行优先）
     * @param src_w 源宽
     * @param src_h 源高
     * @param dst 目标矩形（设备像素）
     */
    void drawImage(uint64_t key, uint32_t version, const uint8_t* rgba, int src_w, int src_h,
                   const Rect& dst);

    /**
     * 丢掉后端为这个资源缓存的解码结果。
     *
     * 资源从表里删掉时调它，缓存才不会比资源表活得更久 —— 那是一份按 key 存的图，
     * 键是资源身份的哈希，删了资源就再没人能找回它。
     * @param key 资源身份
     */
    static void forgetImage(uint64_t key);

    /**
     * 实心填充一个三角形（顶点是设备像素）。
     *
     * 只为"字形里根本没有的图形"而存在（编辑标记里换行符的箭头）。顶点必须由调用方
     * 对齐到整数像素：实现不开抗锯齿，因为分带与整带的画布高度不同会让 AA 的覆盖率差出
     * 一两个色阶，而连页接缝的判据是逐字节相同。
     * @param x0 第一个顶点的 x
     * @param y0 第一个顶点的 y
     * @param x1 第二个顶点的 x
     * @param y1 第二个顶点的 y
     * @param x2 第三个顶点的 x
     * @param y2 第三个顶点的 y
     * @param c 颜色
     */
    void fillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c);

    /**
     * 按 c.a 的透明度把颜色混进 r 覆盖的区域（保留底下已经画好的内容）。
     *
     * 与 fillRect 分开的理由只有一条：选区是盖在文字上的，实心填充会把字一起抹掉，
     * 而"选中之后字还看得见、只是泛蓝"才是选区的样子。混合按整数算并四舍五入，
     * 不同路径（缓存 / 直接光栅）走的是同一段代码，所以帧仍然逐字节可比。
     * 结果的 alpha 保持不透明：位图是最终画面，不是图层。
     * @param r 矩形（设备像素）
     * @param c 颜色（用它的 alpha 决定混合比例）
     */
    void blendRect(const Rect& r, Color c);

    /**
     * 把 src 的 src_rect 区域搬到本图的 (dst_x, dst_y)：源与目标是不同尺寸的两块图。
     *
     * 用途：页位图 -> 导出图这种"源小、目标大且原点不同"的搬运。
     * 落点必须是整数像素：页在导出图里的位置由页号乘步距定，本来就是整数，
     *   这里不做任何采样 —— 一旦按浮点落点插值，字就先糊一层。
     * 越界按目标裁剪（窗口最后一槽可能贴着帧底），源侧也按自身范围裁剪。
     * @param src 源位图
     * @param src_rect 源图里要搬的区域（设备像素）
     * @param dst_x 落点左上角 x
     * @param dst_y 落点左上角 y
     */
    void blitFrom(const Bitmap& src, const Rect& src_rect, int dst_x, int dst_y);

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
     * @return 写入次数（上界，不是去重后的面积）
     */
    size_t paintedPixels() const { return painted_px_; }

    /**
     * 把写入量取证的三项计数清零（每次提交前调一次）。
     */
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
     * @return 毫秒（只算"取包围盒"那一笔）
     */
    double glyphBoundsMs() const { return glyph_bounds_ms_; }

    /**
     * 本次提交给光栅器的字形个数。
     *
     * 与 paintedPixels 配对使用：一个说"写了多少像素"，一个说"画了几个字"。
     * 判"字形光栅贵不贵"要看它们的商（每字形多少微秒）——
     *   只看总耗时是分不出"字多"与"每个字贵"的，而这两者的修法完全不同。
     * @return 字形个数（写入次数口径）
     */
    size_t paintedGlyphs() const { return painted_glyphs_; }

    /**
     * 画一个字形。
     * pen_x / pen_y  设备像素坐标下的"字形原点"（横排=基线起点；竖排=竖排原点）
     * units_to_px    从 HarfBuzz scale 单位到设备像素的换算系数
     * rotate_deg     绕字形原点顺时针旋转的角度。0=正立；90=侧倒（蒙文竖排）。
     * 旋转必须在这里做：字体的轮廓是横排形态，
     * "整段侧倒"属于呈现层，不属于整形层。
     * @param font 字体实例
     * @param glyph_id 字形下标
     * @param pen_x 字形原点 x（设备像素）
     * @param pen_y 字形原点 y（设备像素）
     * @param units_to_px scale 单位到像素的换算系数
     * @param color 颜色
     * @param rotate_deg 绕原点顺时针旋转的角度
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
     * @param font 字体实例
     * @param ids 字形下标数组
     * @param xs 原点 x 数组
     * @param ys 原点 y 数组
     * @param count 字形个数
     * @param units_to_px 换算系数
     * @param color 颜色
     * @param rotate_deg 旋转角度（非 0 时退化成逐字形）
     * @param italic 斜体：对这批字形做合成倾斜（字体没有斜体面时的通用做法）
     */
    void drawGlyphs(hb_font_t* font, const uint32_t* ids, const float* xs, const float* ys,
                    int count, float units_to_px, Color color, float rotate_deg = 0.0f,
                    bool italic = false);

    /**
     * 与另一块 Bitmap 共用字体表（Skia 侧的 SkTypeface / SkFont）。
     *
     * 为什么非共用不可：Skia 的字形缓存按 strike 规格索引，而规格里含字体对象的身份。
     *   两块 Bitmap 各建一份 SkTypeface，缓存就分成互不相见的两份 —— 往其中一份预热
     *   对另一份毫无帮助，而症状只是"没变快"，什么都看不出来。
     * @param other 另一块位图
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
     * @param bytes 预算字节数（0 = 不动，用后端默认值）
     */
    void setGlyphCacheBudget(size_t bytes);

    /**
     * 字形 mask 缓存当前占用。
     * @return 字节数
     */
    size_t glyphCacheUsedBytes() const;

    /**
     * 字形 mask 缓存当前的预算。
     * @return 字节数（0 = 用后端默认值）
     */
    size_t glyphCacheBudgetBytes() const;

private:

    /**
     * 宽（设备像素）。
     */
    int w_ = 0;

    /**
     * 高（设备像素）。
     */
    int h_ = 0;

    /**
     * 像素数据（RGBA，行优先）。
     */
    std::vector<uint8_t> px_;

    /**
     * 自上次重置以来实际写入的像素数。
     */
    size_t painted_px_ = 0;

    /**
     * 同一口径下的字形个数。
     */
    size_t painted_glyphs_ = 0;

    /**
     * 同一口径下"记账取包围盒"花掉的时间。
     */
    double glyph_bounds_ms_ = 0.0;

    /**
     * Skia 后端的实现（定义在 bitmap_skia.cpp）。
     *
     * 用 pimpl 有两个理由：bitmap.h 是公共头，每个 include 它的编译单元不该被迫吃进
     *   Skia 的头与它那套编译宏；且 Skia 的 typeface/font 缓存是可增长容器，放进头里
     *   会让 Bitmap 的定义随 Skia 版本变化（ABI 不稳）。
     */
    struct SkiaImpl;

    /**
     * 两块位图共享的字体表（SkTypeface / SkFont）。
     */
    struct SkiaFonts;

    /**
     * 后端状态；纯粹的空位图（0 x 0）时为空。
     */
    std::unique_ptr<SkiaImpl> sk_;
};

}  // namespace heditor
