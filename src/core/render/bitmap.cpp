/**
 * 自写光栅后端（默认）。打开 HEDITOR_USE_SKIA 时整个文件退位，由 bitmap_skia.cpp 接管。
 * 在标出了 HEDITOR_USE_SKIA 的构建里，这个文件仍然参与编译（CMake 不挑文件），
 * 但内容被这一个 #if 掏空 —— 好处是"两个后端"的边界是显式的：
 * 看这个文件的第一行就知道自己是不是在编译它。
 */
#include "core/render/bitmap.h"

#if !defined(HEDITOR_USE_SKIA)

#include <hb.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace heditor {

Bitmap::Bitmap() = default;
Bitmap::Bitmap(int w, int h) { reset(w, h); }
Bitmap::~Bitmap() = default;

Bitmap::Bitmap(Bitmap&& other) noexcept
    : glyph_cache_(std::move(other.glyph_cache_)),
      w_(other.w_),
      h_(other.h_),
      px_(std::move(other.px_)),
      painted_px_(other.painted_px_),
      painted_glyphs_(other.painted_glyphs_),
      cov_scratch_(std::move(other.cov_scratch_)),
      row_scratch_(std::move(other.row_scratch_)) {
    other.w_ = 0;
    other.h_ = 0;
    other.painted_px_ = 0;
    other.painted_glyphs_ = 0;
}

Bitmap& Bitmap::operator=(Bitmap&& other) noexcept {
    if (this != &other) {
        glyph_cache_ = std::move(other.glyph_cache_);
        w_ = other.w_;
        h_ = other.h_;
        px_ = std::move(other.px_);
        painted_px_ = other.painted_px_;
        painted_glyphs_ = other.painted_glyphs_;
        cov_scratch_ = std::move(other.cov_scratch_);
        row_scratch_ = std::move(other.row_scratch_);
        other.w_ = 0;
        other.h_ = 0;
        other.painted_px_ = 0;
        other.painted_glyphs_ = 0;
    }
    return *this;
}

namespace {

/**
 * 曲线离散化精度（每段曲线切成多少条折线）。@2x 缩放下 8/12 段足够平滑。
 */
constexpr int kQuadSegments = 8;
constexpr int kCubicSegments = 12;

/**
 * 纵向超采样数：覆盖率按 1/kSubSamples 行高累积，压低笔画边缘的锯齿。
 */
constexpr int kSubSamples = 4;

struct Segment {
    float x0, y0, x1, y1;
    int dir;  // +1 向下，-1 向上
};

/**
 * 把 HarfBuzz 的 draw 回调收集成折线轮廓。
 * 坐标约定（已确认）：
 * hb_font_draw_glyph 给出的轮廓坐标是 y 轴向上 的（和 hb_glyph_extents_t 一致）。
 * 实测 NotoSansSC 在 12pt 下某个汉字的轮廓 y ∈ [-0.96, +10.11] 磅 —— 正数在上面。
 * 而本实现的帧缓冲是 y 轴向下的，所以这里必须取负。取错了整篇文字会上下镜像。
 * 侧倒（蒙文竖排，rotate_deg=90）：
 * 把横排形态的字形顺时针转 90°。横排下局部→屏幕的映射是 (u, -v)（u 向右、v 向上），
 * 顺时针转 90° 后 (u, -v) ↦ (v, u)，即
 * 屏幕偏移 = (y, x)     —— 字形的"上"变成屏幕的"右"，字形的"右"变成屏幕的"下"。
 * 于是整行文字沿列向下排，而字身侧躺在列里。
 */
struct PathCollector {
    float pen_x = 0.0f;
    float pen_y = 0.0f;
    float k = 1.0f;  // scale 单位 -> 设备像素
    bool sideways = false;
    std::vector<std::vector<Point>> contours;

    Point map(float x, float y) const {
        if (sideways) return Point{pen_x + y * k, pen_y + x * k};
        return Point{pen_x + x * k, pen_y - y * k};
    }

    void beginContour(float x, float y) { contours.push_back({map(x, y)}); }

    void append(float x, float y) {
        if (contours.empty())
            beginContour(x, y);
        else
            contours.back().push_back(map(x, y));
    }

    void moveTo(float x, float y) { beginContour(x, y); }

    void lineTo(float x, float y) { append(x, y); }

    void quadTo(float cx, float cy, float x, float y) {
        if (contours.empty()) return;
        const Point p0 = contours.back().back();
        const Point p1 = map(cx, cy);
        const Point p2 = map(x, y);
        for (int i = 1; i <= kQuadSegments; ++i) {
            const float t = static_cast<float>(i) / kQuadSegments;
            const float mt = 1.0f - t;
            contours.back().push_back(Point{mt * mt * p0.x + 2 * mt * t * p1.x + t * t * p2.x,
                                            mt * mt * p0.y + 2 * mt * t * p1.y + t * t * p2.y});
        }
    }

    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
        if (contours.empty()) return;
        const Point p0 = contours.back().back();
        const Point a1 = map(c1x, c1y);
        const Point a2 = map(c2x, c2y);
        const Point p3 = map(x, y);
        for (int i = 1; i <= kCubicSegments; ++i) {
            const float t = static_cast<float>(i) / kCubicSegments;
            const float mt = 1.0f - t;
            const float b0 = mt * mt * mt, b1 = 3 * mt * mt * t, b2 = 3 * mt * t * t,
                        b3 = t * t * t;
            contours.back().push_back(Point{b0 * p0.x + b1 * a1.x + b2 * a2.x + b3 * p3.x,
                                            b0 * p0.y + b1 * a1.y + b2 * a2.y + b3 * p3.y});
        }
    }

    void closePath() {}
};

/**
 * 注意参数位置：HarfBuzz 的 draw 回调签名是
 * (dfuncs, draw_data, state, ..., user_data)
 * draw_data 是 hb_font_draw_glyph() 每次调用传进来的指针（本实现的 PathCollector）；
 * 末尾的 user_data 是 hb_draw_funcs_set_*_func() 注册时绑定的。
 * 两者都是 void*，取错就是空指针解引用。
 */
void onMoveTo(hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, float x, float y, void*) {
    static_cast<PathCollector*>(draw_data)->moveTo(x, y);
}
void onLineTo(hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, float x, float y, void*) {
    static_cast<PathCollector*>(draw_data)->lineTo(x, y);
}
void onQuadTo(hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, float cx, float cy, float x,
              float y, void*) {
    static_cast<PathCollector*>(draw_data)->quadTo(cx, cy, x, y);
}
void onCubicTo(hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, float c1x, float c1y, float c2x,
               float c2y, float x, float y, void*) {
    static_cast<PathCollector*>(draw_data)->cubicTo(c1x, c1y, c2x, c2y, x, y);
}
void onClosePath(hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, void*) {
    static_cast<PathCollector*>(draw_data)->closePath();
}

hb_draw_funcs_t* drawFuncs() {
    static hb_draw_funcs_t* funcs = nullptr;
    if (!funcs) {
        funcs = hb_draw_funcs_create();
        hb_draw_funcs_set_move_to_func(funcs, onMoveTo, nullptr, nullptr);
        hb_draw_funcs_set_line_to_func(funcs, onLineTo, nullptr, nullptr);
        hb_draw_funcs_set_quadratic_to_func(funcs, onQuadTo, nullptr, nullptr);
        hb_draw_funcs_set_cubic_to_func(funcs, onCubicTo, nullptr, nullptr);
        hb_draw_funcs_set_close_path_func(funcs, onClosePath, nullptr, nullptr);
    }
    return funcs;
}

/**
 * 把一个像素区间 [xa, xb] 按解析覆盖率累加进 cov（weight = 1/kSubSamples）
 * 这里只允许写 [0, w) 之内 —— 调用方（drawGlyph）据此把逐像素那一遍
 * 限制在字形自己的 x 范围内，见那里的注释。
 */
void addSpan(float* cov, int w, float xa, float xb, float weight) {
    if (xb <= xa) return;
    xa = std::max(xa, 0.0f);
    xb = std::min(xb, static_cast<float>(w));
    if (xb <= xa) return;

    const int ia = static_cast<int>(std::floor(xa));
    const int ib = static_cast<int>(std::ceil(xb)) - 1;
    if (ib < 0 || ia >= w) return;
    const int iaC = std::max(ia, 0);
    const int ibC = std::min(ib, w - 1);

    if (iaC == ibC) {
        cov[iaC] += (xb - xa) * weight;
        return;
    }
    cov[iaC] += (static_cast<float>(iaC + 1) - xa) * weight;
    for (int i = iaC + 1; i < ibC; ++i) cov[i] += weight;
    cov[ibC] += (xb - static_cast<float>(ibC)) * weight;
}

}  // namespace

void Bitmap::reset(int w, int h) {
    w_ = std::max(w, 0);
    h_ = std::max(h, 0);
    px_.assign(static_cast<size_t>(w_) * h_ * 4, 0);
}

/**
 * 把 count 个像素的颜色写进 dst（stride 4，RGB 顺序）。只用来造"一行"。
 */
static inline void makeRow(uint8_t* dst, int count, Color c) {
    for (int i = 0; i < count; ++i) {
        dst[i * 4 + 0] = c.r;
        dst[i * 4 + 1] = c.g;
        dst[i * 4 + 2] = c.b;
        dst[i * 4 + 3] = c.a;
    }
}

void Bitmap::clear(Color c) {
    if (w_ <= 0 || h_ <= 0) return;
    ensureRowScratch();
    makeRow(row_scratch_.data(), w_, c);
    const size_t row_bytes = static_cast<size_t>(w_) * 4;
    uint8_t* base = px_.data();
    for (int y = 0; y < h_; ++y) {
        std::memcpy(base + static_cast<size_t>(y) * row_bytes, row_scratch_.data(), row_bytes);
    }
}

void Bitmap::fillRect(const Rect& r, Color c) {
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
    const int x1 = std::min(w_, static_cast<int>(std::ceil(r.x + r.w)));
    const int y1 = std::min(h_, static_cast<int>(std::ceil(r.y + r.h)));
    if (x1 <= x0 || y1 <= y0) return;
    painted_px_ += static_cast<size_t>(x1 - x0) * static_cast<size_t>(y1 - y0);

    const int seg = x1 - x0;
    ensureRowScratch();
    makeRow(row_scratch_.data(), seg, c);

    const size_t row_bytes = static_cast<size_t>(w_) * 4;
    const size_t seg_bytes = static_cast<size_t>(seg) * 4;
    const size_t x_off = static_cast<size_t>(x0) * 4;
    uint8_t* base = px_.data();
    for (int y = y0; y < y1; ++y) {
        std::memcpy(base + static_cast<size_t>(y) * row_bytes + x_off, row_scratch_.data(),
                    seg_bytes);
    }
}

void Bitmap::copyRectFrom(const Bitmap& src, const Rect& r) {
    // 尺寸必须一致：两图的行跨度不同时，"同位置拷贝"这件事本身就没定义。
    if (src.w_ != w_ || src.h_ != h_) return;
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
    const int x1 = std::min(w_, static_cast<int>(std::ceil(r.x + r.w)));
    const int y1 = std::min(h_, static_cast<int>(std::ceil(r.y + r.h)));
    if (x1 <= x0 || y1 <= y0) return;
    painted_px_ += static_cast<size_t>(x1 - x0) * static_cast<size_t>(y1 - y0);
    const size_t row_bytes = static_cast<size_t>(x1 - x0) * 4;
    for (int y = y0; y < y1; ++y) {
        const size_t off = (static_cast<size_t>(y) * w_ + static_cast<size_t>(x0)) * 4;
        std::memcpy(px_.data() + off, src.px_.data() + off, row_bytes);
    }
}

void Bitmap::strokeRect(const Rect& r, Color c, float thickness) {
    fillRect(Rect{r.x, r.y, r.w, thickness}, c);
    fillRect(Rect{r.x, r.y + r.h - thickness, r.w, thickness}, c);
    fillRect(Rect{r.x, r.y, thickness, r.h}, c);
    fillRect(Rect{r.x + r.w - thickness, r.y, thickness, r.h}, c);
}

/**
 * float 的精确位模式。缓存键里用它而不是做量化 ——
 * 量化会改变 cov（分箱变了），也就放弃了"缓存路径与光栅路径逐字节相同"这条判据。
 */
static inline uint32_t fbits(float f) {
    uint32_t b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

size_t GlyphCache::KeyHash::operator()(const Key& k) const {
    // FNV-1a 折叠所有键位。字形的键很窄（6 个字段），不需要更强的哈希。
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    mix(reinterpret_cast<uintptr_t>(k.font));
    mix(k.glyph_id);
    mix(k.scale_bits);
    mix(k.fracx_bits);
    mix(k.fracy_bits);
    mix(k.sideways ? 1u : 0u);
    return static_cast<size_t>(h);
}

/**
 * 把一份 mask 用 color 混合到 (x0, y0)。混合公式、跳过阈值、painted_px_ 的
 * 计数方式必须与"直接光栅"那条路径逐字相同 —— 这不是重复代码，而是
 * 判据本身：只要两条路径的算术有任何差别，"PNG 逐字节不变"就不再能证明缓存是等价的。
 */
void Bitmap::blitMask(const GlyphMask& m, int x0, int y0, Color color) {
    if (m.w <= 0 || m.h <= 0) return;
    const float alphaScale = static_cast<float>(color.a) / 255.0f;
    for (int j = 0; j < m.h; ++j) {
        const int y = y0 + j;
        if (y < 0 || y >= h_) continue;
        uint8_t* row = px_.data() + static_cast<size_t>(y) * w_ * 4;
        const float* src = m.cov.data() + static_cast<size_t>(j) * static_cast<size_t>(m.w);
        for (int i = 0; i < m.w; ++i) {
            const int x = x0 + i;
            if (x < 0 || x >= w_) continue;
            const float a = clampf(src[i], 0.0f, 1.0f) * alphaScale;
            if (a <= 0.0015f) continue;
            ++painted_px_;  // 与光栅路径同口径：只记真的写下去的像素
            uint8_t* p = row + x * 4;
            const float inv = 1.0f - a;
            p[0] = static_cast<uint8_t>(color.r * a + p[0] * inv + 0.5f);
            p[1] = static_cast<uint8_t>(color.g * a + p[1] * inv + 0.5f);
            p[2] = static_cast<uint8_t>(color.b * a + p[2] * inv + 0.5f);
            p[3] = 255;
        }
    }
}

/**
 * 光栅一个字形，产出覆盖率 mask。
 * out->x0/y0 是绝对设备像素坐标（不是相对 pen 的），调用方按 pen 的整数部分差平移。
 * *cacheable 输出"本次光栅有没有被画布裁剪过" —— 裁剪过的 mask 依赖画布尺寸，
 * 不可跨帧复用（见 drawGlyph 里两条判据的注释）。
 */
bool Bitmap::rasterizeGlyph(hb_font_t* font, uint32_t glyph_id, float pen_x, float pen_y,
                            float units_to_px, float rotate_deg, GlyphMask* out, bool* cacheable) {
    if (cacheable) *cacheable = false;
    if (!font || w_ <= 0 || h_ <= 0) return false;

    PathCollector collector;
    collector.pen_x = pen_x;
    collector.pen_y = pen_y;
    collector.k = units_to_px;
    // 目前只有 0°（正立）与 90°（侧倒）两种；其它角度按最接近的处理。
    collector.sideways = std::fabs(rotate_deg - 90.0f) < 45.0f;
    hb_font_draw_glyph(font, glyph_id, drawFuncs(), &collector);
    if (collector.contours.empty()) return false;  // 空白字形（如空格）——没有轮廓可填

    // 构建边表
    //
    // x 范围也要一起统计。下面那一遍逐像素是按像素写的，如果不限定 x，
    //   它就变成"每个字形都把整帧宽度扫一遍"—— 实测这是绘制耗时的绝对大头：
    //   4 页 @1.5× 共 1022 个字形、131 ms，即 128 µs/字形；而一个 1.5× 汉字
    //   在设备像素里也就 20 列左右。按 893 列扫，等于 98% 的循环是空转。
    //   覆盖率在 [floor(minX), ceil(maxX)] 之外可证为 0（所有 span 端点都落在
    //   字形自己的轮廓 x 区间内），所以裁掉不改变任何一个像素的结果 ——
    //   这一点由"优化前后 4 页 PNG 逐字节相同"来证伪，不是靠推理保证。
    std::vector<Segment> edges;
    float minY = 1e30f, maxY = -1e30f;
    float minX = 1e30f, maxX = -1e30f;
    for (const auto& contour : collector.contours) {
        const size_t n = contour.size();
        if (n < 2) continue;
        for (size_t i = 0; i < n; ++i) {
            const Point& a = contour[i];
            const Point& b = contour[(i + 1) % n];  // 隐式闭合
            // x 范围取所有轮廓点，不只是非水平边的端点：
            //   纯水平边的 x 也要算进去（虽然它自己不产生边）。
            minX = std::min(minX, std::min(a.x, b.x));
            maxX = std::max(maxX, std::max(a.x, b.x));
            if (a.y == b.y) continue;
            Segment s;
            s.x0 = a.x;
            s.y0 = a.y;
            s.x1 = b.x;
            s.y1 = b.y;
            s.dir = (b.y > a.y) ? 1 : -1;
            edges.push_back(s);
            minY = std::min(minY, std::min(a.y, b.y));
            maxY = std::max(maxY, std::max(a.y, b.y));
        }
    }
    if (edges.empty()) return false;

    // 未被画布裁剪的原始窗口
    const int rawX0 = static_cast<int>(std::floor(minX)) - 1;
    const int rawX1 = static_cast<int>(std::ceil(maxX)) + 1;
    const int rawY0 = static_cast<int>(std::floor(minY));
    const int rawY1 = static_cast<int>(std::ceil(maxY)) + 1;

    // 只有完全落在画布内的光栅才允许进缓存。
    //   只要 addSpan 里的 max(xa, 0) 或 min(xb, w) 生效过一次，
    //   cov 的值就带上了画布尺寸的痕迹 —— 那份 mask 拿到别处复用会与
    //   "重新光栅"给出不同的数字。宁可本次不缓存，也不能让缓存改变结果。
    const bool uncut = (rawX0 >= 0) && (rawX1 <= w_) && (rawY0 >= 0) && (rawY1 <= h_);

    const int y0 = std::max(0, rawY0);
    const int y1 = std::min(h_, rawY1);
    if (y1 <= y0) return false;

    // x 窗口：多留 1 px 余量，把浮点取整的边界情况也圈进来。
    // 这个窗口只影响"扫哪些列"，不影响"每列的值"。
    const int xa0 = std::max(0, rawX0);
    const int xb0 = std::min(w_, rawX1);
    if (xb0 <= xa0) return false;

    const float weight = 1.0f / static_cast<float>(kSubSamples);

    // 覆盖率缓冲成员复用，不再每个字形 new 一个 w_ 长的 vector。
    //   而且传给 addSpan 的宽度参数仍然是 w_（整帧宽）—— 刻意与旧实现保持
    //   同一个 clamp 上界，这样"未裁剪"的那批字形算出来的 cov 与旧版本逐位相同，
    //   "PNG 逐字节不变"才是一条有意义的判据。
    if (static_cast<int>(cov_scratch_.size()) < w_) {
        cov_scratch_.resize(static_cast<size_t>(w_));
    }
    float* cov = cov_scratch_.data();
    std::vector<std::pair<float, int>> hits;

    // mask 的头部信息必须先写好
    //
    // 这里出过一个"什么都对、就是一片空白"的 bug，值得留一段说明：
    //   覆盖率缓冲改成成员复用之后，cov 变成每行开头清一次（只清 [xa0, xb0)
    //   这一段），而搬进 mask 的 memcpy 还在整个 y 循环之外。
    //   于是每一行都被填上了"最后一行"的覆盖率 —— 而最后一行恰好是
    //   rawY1-1 = ceil(maxY)，比墨迹还低一行，几乎恒为空。
    //   症状极其隐蔽：mask 的尺寸、位置、cacheable 全对，唯独 cov 恒为 0；
    //   blitMask 里 a <= 0.0015f 把所有像素都跳过 → 一个字都画不出来。
    //   而"缓存路径与直接光栅逐字节相同"这类断言照样全绿（两边都空，当然相等），
    //   连 PNG 的 SHA-256 都是稳定的。只有"新页上有没有墨迹"这种像素级断言
    //   才抓得住它。
    //   → 所以现在改成：每行算完立刻搬进 mask。开销一样（同一块缓冲），
    //     但语义变成"逐行落定"，不可能再出现跨行串值。
    out->w = xb0 - xa0;
    out->h = y1 - y0;
    out->x0 = xa0;
    out->y0 = y0;
    out->pen_ix = static_cast<int>(std::floor(pen_x));
    out->pen_iy = static_cast<int>(std::floor(pen_y));
    out->cov.assign(static_cast<size_t>(out->w) * static_cast<size_t>(out->h), 0.0f);

    const size_t row_floats = static_cast<size_t>(out->w);
    for (int y = y0; y < y1; ++y) {
        // 只清要用的那一段，不是整帧宽度
        std::fill(cov + xa0, cov + xb0, 0.0f);

        for (int s = 0; s < kSubSamples; ++s) {
            const float sy = static_cast<float>(y) +
                             (static_cast<float>(s) + 0.5f) / static_cast<float>(kSubSamples);
            hits.clear();
            for (const Segment& e : edges) {
                const float lo = std::min(e.y0, e.y1);
                const float hi = std::max(e.y0, e.y1);
                if (sy < lo || sy >= hi) continue;
                const float t = (sy - e.y0) / (e.y1 - e.y0);
                hits.emplace_back(e.x0 + t * (e.x1 - e.x0), e.dir);
            }
            if (hits.size() < 2) continue;
            std::sort(hits.begin(), hits.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });

            int winding = 0;
            for (size_t i = 0; i + 1 < hits.size(); ++i) {
                winding += hits[i].second;
                if (winding != 0) {
                    addSpan(cov, w_, hits[i].first, hits[i + 1].first, weight);
                }
            }
        }

        // 逐行落定（见上面那段说明）。整行全 0 也要原样搬 ——
        //   blitMask 对 a <= 0.0015 的像素本来就会跳过，两条路径的结果仍然一致。
        std::memcpy(out->cov.data() + static_cast<size_t>(y - y0) * row_floats, cov + xa0,
                    sizeof(float) * row_floats);
    }

    if (cacheable) *cacheable = uncut;
    return true;
}

void Bitmap::drawGlyph(hb_font_t* font, uint32_t glyph_id, float pen_x, float pen_y,
                       float units_to_px, Color color, float rotate_deg) {
    if (!font || w_ <= 0 || h_ <= 0) return;
    ++painted_glyphs_;

    GlyphCache::Key k;
    const bool use_cache = (glyph_cache_ != nullptr);
    if (use_cache) {
        // frac() 必须进键。cov 是按 floor() 分箱算的，
        //   pen_x = 10.3 与 10.7 得到的是两套不同的值，并非平移关系。
        const float fx = pen_x - std::floor(pen_x);
        const float fy = pen_y - std::floor(pen_y);
        k.font = font;
        k.glyph_id = glyph_id;
        k.scale_bits = fbits(units_to_px);
        k.fracx_bits = fbits(fx);
        k.fracy_bits = fbits(fy);
        k.sideways = std::fabs(rotate_deg - 90.0f) < 45.0f;
    }

    if (use_cache) {
        if (const GlyphMask* m = glyph_cache_->find(k)) {
            const int dx = static_cast<int>(std::floor(pen_x)) - m->pen_ix;
            const int dy = static_cast<int>(std::floor(pen_y)) - m->pen_iy;
            const int tx = m->x0 + dx;
            const int ty = m->y0 + dy;
            // 目标必须完全在画布内。部分出界时本次绘制本该被裁剪，而缓存里的值
            // 是未裁剪的 —— 退回下面现光栅，让 addSpan 自己按画布裁一遍。
            if (tx >= 0 && ty >= 0 && tx + m->w <= w_ && ty + m->h <= h_) {
                blitMask(*m, tx, ty, color);
                return;
            }
        }
    }

    GlyphMask m;
    bool cacheable = false;
    if (!rasterizeGlyph(font, glyph_id, pen_x, pen_y, units_to_px, rotate_deg, &m, &cacheable)) {
        return;
    }
    // 只在"本次光栅没被裁剪"时才存入 —— 被裁过的 mask 依赖画布尺寸。
    if (use_cache && cacheable) glyph_cache_->insert(k, m);
    blitMask(m, m.x0, m.y0, color);
}

/**
 * 批量画字形。
 *
 * 这一侧刻意只是逐个转调 drawGlyph：自写光栅器的固定开销本来就小
 *   （一次字形缓存的哈希查找），省不下什么；Skia 侧那笔"装配一遍字形运行"
 *   才是大头（见 bitmap_skia.cpp）。
 * 两条入口的存在是为了让调用方能按段调用，而不是让这里也长出第二套光栅。
 */
void Bitmap::drawGlyphs(hb_font_t* font, const uint32_t* ids, const float* xs, const float* ys,
                        int count, float units_to_px, Color color, float rotate_deg) {
    if (!font || !ids || !xs || !ys) return;
    for (int i = 0; i < count; ++i) {
        drawGlyph(font, ids[i], xs[i], ys[i], units_to_px, color, rotate_deg);
    }
}

}  // namespace heditor

#endif  // !defined(HEDITOR_USE_SKIA)
