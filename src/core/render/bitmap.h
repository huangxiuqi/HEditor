/**
 * 光栅目标（帧缓冲）。两个后端共用同一份接口。
 *
 * 默认（不定义 HEDITOR_USE_SKIA）走 bitmap.cpp：自写扫描线光栅器加字形缓存。它现在的价值是
 * 逐字节可复现，作为 Skia 版的对照物（同一文档在两个后端下渲染，PNG 应当接近）。
 * 定义 HEDITOR_USE_SKIA 走 bitmap_skia.cpp：像素仍落在同一个 RGBA 缓冲里（frame /
 * frameBytes / copyRectFrom 的契约不变），绘制经 SkCanvas、字形经 SkFont 与 FreeType。
 * 于是手写扫描线、曲线离散化、4 倍纵向超采样以及 GlyphMask/GlyphCache 全部退役，
 * 对应的是 Skia 的 SkGlyphCache 与 SkScalerContext；换来抗锯齿质量、变体字形与
 * 路径/图像/效果等现成能力。
 *
 * 保留两套的原因：native 侧单测跑旧实现，留一把可对照的尺子。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/text/font.h"
#include "core/utils/types.h"

namespace heditor {

/**
 * 字形光栅缓存（只有非 Skia 后端用）
 * Skia 一旦接手，这个类就只是接口占位：drawGlyph 不再查它，
 * 统计恒为 0。留着是为了让 engine.cpp 一行都不用改
 * （它要调 resetStats / hits / misses，好让 EditStats 的取证口径统一）。
 * 它存在的原因（也是 Skia 后端替掉的东西）：4 页文档 1.5 倍缩放下，绘制耗时的 91.5%
 * 是字形 —— 抽轮廓、曲线离散化（kCubicSegments=12，一个 CJK 字形可上千点）、逐扫描线
 * 遍历全部边且没有 active-edge-table，合计 121 µs/字形，而每帧都要重算一遍。
 * 去重空间很大：中文正文步进是整数像素，同一行字形的亚像素相位完全相同。
 *
 * 缓存键必须含亚像素相位：cov 由每个 span 端点按 floor() 分箱算出，pen_x = 10.3 与 10.7
 * 对同一字形得到的是两套不同的 cov，并非平移关系。Skia 的 SkScalerContext 同理。
 */
struct GlyphMask {
    int w = 0;
    int h = 0;
    int pen_ix = 0;  // 光栅时 pen_x 的整数部分
    int pen_iy = 0;
    int x0 = 0;  // mask 左上角的绝对设备像素 x（= 光栅时的 xa0）
    int y0 = 0;
    /**
     * w*h 行主序覆盖率。与直接光栅得到的值逐位相同（不是量化后的 uint8）。
     */
    std::vector<float> cov;
};

class GlyphCache {
public:
    /**
     * 两个判据，决定某次绘制能不能走缓存。
     *
     * 判据一：光栅时完全没有被画布裁剪 —— 只要 addSpan 里的 max(xa,0) /
     * min(xb,w) 生效过，cov 的值就依赖画布尺寸，这份 mask 不可跨帧复用。
     * 判据二：复用时目标位置完全落在画布内 —— 否则本次绘制本该裁剪，而缓存里的
     * 值是未裁剪的。两条不满足时一律退回直接光栅，结果永远与"没有缓存"一致。
     */
    struct Key {
        const void* font = nullptr;
        uint32_t glyph_id = 0;
        uint32_t scale_bits = 0;  // units_to_px 的位模式（精确比较，不量化）
        uint32_t fracx_bits = 0;  // frac(pen_x) 的位模式
        uint32_t fracy_bits = 0;
        bool sideways = false;
        bool operator==(const Key& o) const {
            return font == o.font && glyph_id == o.glyph_id && scale_bits == o.scale_bits &&
                   fracx_bits == o.fracx_bits && fracy_bits == o.fracy_bits &&
                   sideways == o.sideways;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const;
    };

    /**
     * 上限，防止异常文档（每个字形一个不重复相位）把内存撑爆。
     * 超了就整体清空 —— 简单、不会僵住，也不需要 LRU。
     */
    static constexpr size_t kMaxEntries = 8192;

    const GlyphMask* find(const Key& k) {
        auto it = map_.find(k);
        if (it == map_.end()) {
            ++misses_;
            return nullptr;
        }
        ++hits_;
        return &it->second;
    }
    void insert(const Key& k, GlyphMask m) {
        if (map_.size() >= kMaxEntries) map_.clear();
        map_.emplace(k, std::move(m));
    }

    size_t hits() const { return hits_; }
    size_t misses() const { return misses_; }
    size_t entries() const { return map_.size(); }
    void resetStats() {
        hits_ = 0;
        misses_ = 0;
    }

private:
    std::unordered_map<Key, GlyphMask, KeyHash> map_;
    size_t hits_ = 0;
    size_t misses_ = 0;
};

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
    }

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
     * 多个 Bitmap（帧缓冲 bitmap_ + 底图 base_）应当共享同一份字形缓存：
     * 同一个字形在两块图上光栅出来的 mask 是一模一样的，各存一份纯属浪费。
     * 传 nullptr 表示不做缓存（退回每个字形现光栅）。
     */
    void setGlyphCache(std::shared_ptr<GlyphCache> c) { glyph_cache_ = std::move(c); }
    GlyphCache* glyphCache() const { return glyph_cache_.get(); }

private:
    std::shared_ptr<GlyphCache> glyph_cache_;

    int w_ = 0;
    int h_ = 0;
    std::vector<uint8_t> px_;
    size_t painted_px_ = 0;      // 自上次 resetPaintedPixels() 以来实际写入的像素数
    size_t painted_glyphs_ = 0;  // 同一口径下的字形个数（见 paintedGlyphs）

#if defined(HEDITOR_USE_SKIA)

    /**
     * Skia 后端状态。用 pimpl 有两个理由：bitmap.h 是公共头，不打开 Skia 的宿主
     * （native 侧那几个 target）不该被迫 include Skia 的头或被其编译宏影响；
     * 且 Skia 的 typeface/font 缓存是可增长容器，放进头里会让 Bitmap 的定义
     * 随 Skia 版本变化（ABI 不稳）。
     */
    struct SkiaImpl;
    std::unique_ptr<SkiaImpl> sk_;

#else

    /**
     * 自写光栅后端的状态
     * 光栅一个字形，把覆盖率填进 out（绝对设备像素坐标），返回是否成功。
     * *cacheable 输出"本次光栅有没有被画布裁剪过"—— 裁过的 mask 依赖画布尺寸，
     * 不可跨帧复用（见 GlyphCache 上面那段两条判据）。
     * 与"带缓存"的绘制路径共用同一条光栅实现 —— 两条路径算出来的必须是
     * 同一串浮点数，否则"逐字节相同"这条判据就失去意义了。
     * 非 const：它复用 cov_scratch_ 这个可增长的 scratch 缓冲。
     */
    bool rasterizeGlyph(hb_font_t* font, uint32_t glyph_id, float pen_x, float pen_y,
                        float units_to_px, float rotate_deg, GlyphMask* out, bool* cacheable);

    /**
     * 把一份 mask 用 color 混合到目标位置（左上角绝对像素 = x0+dx, y0+dy）。
     * 写入量照常记进 painted_px_。
     */
    void blitMask(const GlyphMask& m, int x0, int y0, Color color);

    /**
     * 保证 row_scratch_ 至少有 w_ 个像素（w_*4 字节）。只增长不重建。
     */
    void ensureRowScratch() {
        const size_t need = static_cast<size_t>(w_) * 4;
        if (row_scratch_.size() < need) row_scratch_.resize(need);
    }

    /**
     * 字形光栅的覆盖率缓冲，复用。
     * 是 drawGlyph 里的局部 vector，长度还是整帧宽度 —— 每画一个字形
     * 就 new 一次（实测 4 页文档 1022 个字形 = 1022 次分配）。改成成员后只增长不重建。
     * 长度恒为 ≥ w_；drawGlyph 只会用它自己的 x 窗口那一段。
     */
    std::vector<float> cov_scratch_;

    /**
     * 一行像素的复用缓冲（长度 = w_*4），配合 memcpy 铺面积用。
     * 为什么必须走 memcpy： clear()/fillRect() 是
     * for (x) { row[x*4+0]=r; row[x*4+1]=g; row[x*4+2]=b; row[x*4+3]=a; }
     * 这种 stride-4 的逐字节标量写几乎不会被自动向量化，实测只有 ~200 MB/s，
     * 而铺面积是绘制耗时的绝对大头（见 drawGlyph / clear 的注释）。
     * 先造一行、再逐行 memcpy，memcpy 会被内联成 SIMD 存储，收益是数量级的。
     * 逐字节造那一行仍然按 RGB 顺序写，不假设字节序。
     */
    std::vector<uint8_t> row_scratch_;

#endif
};

}  // namespace heditor
