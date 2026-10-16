/**
 * Skia 光栅后端。定义 HEDITOR_USE_SKIA 时由它实现 Bitmap 的全部绘制入口。
 *
 * 对外契约不变：像素仍写在 Bitmap::px_ 里，frame / frameBytes / copyRectFrom /
 * paintedPixels 的语义全部保留，宿主感知不到换了引擎。内部换成：绘制走 SkCanvas；
 * 字形走 SkFont::drawGlyphs 加 FreeType（自写扫描线、4 倍纵向超采样、曲线离散化退役）；
 * 字形缓存交给 Skia 的 SkGlyphCache，GlyphMask 只作接口占位（见 bitmap.h）。
 *
 * 坐标与字号（最容易错的一处）：按 hb_font_set_scale(font, S, S) 的约定，
 * S = size_pt * kFontScalePerPt，设计单位 d 的设备像素为 d * S / upem * units_to_px。
 * 要让 1 em 等于 size_pt 磅乘 zoom 成立，SkFont 的 size 应取 S * units_to_px（不是
 * upem*units_to_px 或 size_pt 乘 zoom）；S 从 hb_font_get_scale() 读，故不会随之失准。
 */
#include "core/render/bitmap.h"

#if defined(HEDITOR_USE_SKIA)

#include "core/text/font.h"
#include "core/text/font_source.h"

#include <hb.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkGraphics.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSpan.h"
#include "include/core/SkStream.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_empty.h"

#include <hb-ot.h>

namespace heditor {

namespace {

/**
 * float 的精确位模式。字号是键的一部分，必须按位比较而不是量化 ——
 * 量化会让"同一字号"被拆成多份，重复建 SkFont（每次都要走 FreeType 的 face 设置）。
 */
inline uint32_t fbits(float f) {
    uint32_t b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

inline SkColor toSk(Color c) { return SkColorSetARGB(c.a, c.r, c.g, c.b); }

/**
 * hb_blob_t 的释放回调：把引用交给 SkData 管，不拷贝字体数据。
 * 一份 CJK 字体动辄 2 MB，而这张 typeface 会跟着 Bitmap 活到进程结束
 * （Bitmap 里两张图共享一份），所以按"拷贝一次"来算也不贵 ——
 * 但 MakeWithProc 是零拷贝且同样安全，没有理由多复制一份。
 */
void releaseHbBlob(const void* /*ptr*/, void* ctx) {
    hb_blob_destroy(static_cast<hb_blob_t*>(ctx));
}

/**
 * 按需字体流：把 FreeType 的读请求转给字体数据源。
 *
 * getMemoryBase 故意返回空。Skia 的 FreeType 端口据此在内存与流之间选后者
 * （判定处见 Skia 的 src/ports/SkFontHost_FreeType.cpp 里 openFace 的两条分岔），
 * 读取于是变成 FT_Stream 的 seek 加 read，而不是把整份字体钉在内存里。
 * duplicate 与 fork 都共享同一个数据源，块缓存与统计因此也是共享的。
 */
class CachedFontStream : public SkStreamAsset {
public:
    explicit CachedFontStream(FontSource& src, uint64_t pos = 0) : src_(src), pos_(pos) {}

    size_t read(void* buffer, size_t size) override {
        if (buffer == nullptr || size == 0) return 0;
        if (pos_ >= src_.size()) return 0;  // 文件尾：流契约允许短读，不是错误
        const uint64_t left = src_.size() - pos_;
        size_t want = size;
        if (static_cast<uint64_t>(want) > left) want = static_cast<size_t>(left);
        size_t got = 0;
        const FontRead r = src_.readAt(pos_, buffer, want, &got);
        // 只有存储层真失败才返回零字节；文件尾那一种按实际读到的字节数返回。
        if (r == FontRead::Error) return 0;
        pos_ += got;
        return got;
    }

    bool isAtEnd() const override { return pos_ >= src_.size(); }
    bool rewind() override { pos_ = 0; return true; }
    size_t getPosition() const override { return static_cast<size_t>(pos_); }

    bool seek(size_t position) override {
        if (static_cast<uint64_t>(position) > src_.size()) return false;
        pos_ = position;
        return true;
    }

    bool move(long offset) override {
        const int64_t next = static_cast<int64_t>(pos_) + static_cast<int64_t>(offset);
        if (next < 0 || static_cast<uint64_t>(next) > src_.size()) return false;
        pos_ = static_cast<uint64_t>(next);
        return true;
    }

    size_t getLength() const override { return static_cast<size_t>(src_.size()); }

private:
    /**
     * 副本从数据开头开始（Skia 对 duplicate 的约定）。
     */
    SkStreamAsset* onDuplicate() const override { return new CachedFontStream(src_); }
    /**
     * fork 停在当前位置（Skia 对 fork 的约定）。
     */
    SkStreamAsset* onFork() const override { return new CachedFontStream(src_, pos_); }

    FontSource& src_;
    uint64_t pos_ = 0;
};

/**
 * 从 hb_face 反查它的按需数据源。整份装载的脸没有挂，返回空。
 */
FontSource* fontSourceOf(hb_face_t* face) {
    if (face == nullptr) return nullptr;
    return static_cast<FontSource*>(hb_face_get_user_data(face, fontSourceKey()));
}

/**
 * 侧倒判定：与 bitmap.cpp 里 PathCollector 完全一致的规则
 * （只有 0° 与 90° 两种，其它角度取最接近的）。
 */
inline bool isSideways(float rotate_deg) { return std::fabs(rotate_deg - 90.0f) < 45.0f; }

/**
 * 变体坐标：hb_font 到 Skia typeface 的桥。弄错的代价是整页中文变成 Thin。
 *
 * NotoSansSC-VF 的 fvar 里 wght 轴 min=100、默认=100、max=900，即默认实例是 Thin(100)
 * 而非 Regular(400)。引擎在 FontInstance::init 里显式钉了 wght=400（可变字体必须钉，
 * 否则度量会漂），故自写光栅器路径画出来是 Regular；Skia 路径若直接 makeFromData 不带
 * 变体参数，FreeType 取默认实例 100，整页中文其实是 Thin。症状（同文本、14pt@1.5=21px、
 * 同色，只换后端）：自写器平均灰 90、逐列墨迹 8~18 像素，Skia 平均灰 163、逐列 4~8 像素，
 * 两后端字形包围盒一致 —— 是笔画细了，不是画小了。
 *
 * 结论：凡是从 hb_font 过桥到 Skia typeface，变体坐标必须一起带过去。下面按 OpenType
 * 规则把归一化坐标还原成设计坐标（design = default + t * (t >= 0 ? max - default : min)）。
 */
bool hbVariationsToSkia(hb_font_t* hbf, hb_face_t* face,
                        std::vector<SkFontArguments::VariationPosition::Coordinate>* out,
                        uint64_t* fingerprint) {
    out->clear();
    *fingerprint = 0;
    if (!hbf || !face) return false;

    unsigned n_coords = 0;
    const int* coords = hb_font_get_var_coords_normalized(hbf, &n_coords);
    if (!coords || n_coords == 0) return false;  // 静态字体：没有变体要带

    hb_ot_var_axis_info_t axes[16];
    unsigned n_axes = 16;
    const unsigned total = hb_ot_var_get_axis_infos(face, 0, &n_axes, axes);
    if (total == 0 || n_axes == 0) return false;

    const unsigned n = std::min(n_coords, n_axes);
    out->reserve(n);
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    for (unsigned i = 0; i < n; ++i) {
        const hb_ot_var_axis_info_t& ax = axes[i];
        const float t = static_cast<float>(coords[i]) / 16384.0f;  // HB 归一化刻度
        const float base = ax.default_value;
        const float span = (t >= 0.0f) ? (ax.max_value - base) : (base - ax.min_value);
        const float design = base + t * span;
        out->push_back(SkFontArguments::VariationPosition::Coordinate{ax.tag, design});
        mix(static_cast<uint64_t>(ax.tag));
        mix(static_cast<uint64_t>(fbits(design)));
    }
    *fingerprint = h;
    return !out->empty();
}

}  // namespace

/**
 * Skia 侧的状态
 */
struct Bitmap::SkiaImpl {
    /**
     * 装订到 Bitmap::px_ 上的位图与画布。installPixels 不拷贝、不持有，
     * 所以 px_ 一旦重新分配（reset）就必须重新装订 —— 见 Bitmap::reset()。
     */
    SkBitmap bm;
    std::unique_ptr<SkCanvas> canvas;

    /**
     * 字体表（字体管理器 + 两张缓存）。跨 Bitmap 共享，见 Bitmap::shareFonts。
     */
    std::shared_ptr<SkiaFonts> fonts_shared;

    /**
     * 批量画字形的转存缓冲。留着复用是为了不让"批量"本身变成每段两次 malloc ——
     * 一段上千个字形时，分配开销能盖过省下来的那笔固定开销。
     * 存的是 Skia 自己的类型（SkGlyphID / SkPoint），所以放在这里而不是 Bitmap 里。
     */
    std::vector<SkGlyphID> glyph_scratch;
    std::vector<SkPoint> pos_scratch;
    std::vector<SkRect> bounds_scratch;

    /**
     * 取（或建）hb 字体对应的 SkFont。
     */
    const SkFont* fontFor(hb_font_t* hbf, float units_to_px);
};

/**
 * 字体表。单独拎出来是为了能被多块 Bitmap 共享 —— Skia 的字形缓存按 strike 规格
 * 索引，而规格里含 SkTypeface 的身份，各建一份就是把缓存切成互不相见的几份。
 */
struct Bitmap::SkiaFonts {
    sk_sp<SkFontMgr> font_mgr;  // SkFontMgr_New_Custom_Empty()：内建字体为 0，全部由我们喂

    /**
     * (hb_face_t*, 变体坐标指纹) → SkTypeface。
     * 用 hb_face_t* 当键是安全的：FontFace 一张脸活到进程结束，指针不会复用。
     * 指纹不能省：同一个 face 上 wght=400 与 wght=700 是两张不同的脸，
     * 只按 face 缓存会把第一次那张错发给后面所有字重（与变体默认值同一类错误）。
     */
    struct TfKey {
        const void* face = nullptr;
        uint64_t vars = 0;
        bool operator==(const TfKey& o) const { return face == o.face && vars == o.vars; }
    };
    struct TfKeyHash {
        size_t operator()(const TfKey& k) const {
            return static_cast<size_t>(reinterpret_cast<uintptr_t>(k.face) * 1099511628211ull ^
                                       (k.vars + 0x9e3779b97f4a7c15ull));
        }
    };
    std::unordered_map<TfKey, sk_sp<SkTypeface>, TfKeyHash> typefaces;

    /**
     * (SkTypeface*, 字号) → SkFont
     */
    struct FontKey {
        const SkTypeface* tf = nullptr;
        uint32_t em_px_bits = 0;
        bool operator==(const FontKey& o) const { return tf == o.tf && em_px_bits == o.em_px_bits; }
    };
    struct FontKeyHash {
        size_t operator()(const FontKey& k) const {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&h](uint64_t v) {
                h ^= v;
                h *= 1099511628211ull;
            };
            mix(reinterpret_cast<uintptr_t>(k.tf));
            mix(k.em_px_bits);
            return static_cast<size_t>(h);
        }
    };
    std::unordered_map<FontKey, SkFont, FontKeyHash> fonts;
};

const SkFont* Bitmap::SkiaImpl::fontFor(hb_font_t* hbf, float units_to_px) {
    if (!fonts_shared || !fonts_shared->font_mgr || !hbf) return nullptr;
    SkiaFonts& fs = *fonts_shared;
    SkFontMgr* font_mgr = fs.font_mgr.get();
    hb_face_t* face = hb_font_get_face(hbf);
    if (!face) return nullptr;

    // 先把变体坐标取出来 —— 这是"中文会不会变成 Thin"的分水岭，
    //   详细推理见文件上方 hbVariationsToSkia 的注释。
    std::vector<SkFontArguments::VariationPosition::Coordinate> vcoords;
    uint64_t vfp = 0;
    const bool has_vars = hbVariationsToSkia(hbf, face, &vcoords, &vfp);

    const SkiaFonts::TfKey tkey{face, has_vars ? vfp : 0};
    sk_sp<SkTypeface>& tf = fs.typefaces[tkey];
    if (!tf) {
        // 变体坐标两条路都要：带变体只能走带 SkFontArguments 的重载，
        //   而 makeFromData 没有接收变体位置的那一个。
        SkFontArguments args;
        if (has_vars) {
            SkFontArguments::VariationPosition pos;
            pos.coordinates = vcoords.data();
            pos.coordinateCount = static_cast<int>(vcoords.size());
            args.setVariationDesignPosition(pos);
        }
        FontSource* src = fontSourceOf(face);
        if (src != nullptr) {
            // 按需模式：流的内存基址为空，Skia 于是把读取交给 FT_Stream，
            //   轮廓与变体数据用到哪块读哪块。数据源由 FontFace 持有到进程结束，
            //   流的生命周期由 Skia 管，两边都不会先走。
            tf = font_mgr->makeFromStream(std::make_unique<CachedFontStream>(*src), args);
        } else {
            hb_blob_t* blob = hb_face_reference_blob(face);  // 返回新引用，归我们
            if (!blob) return nullptr;
            const unsigned len = hb_blob_get_length(blob);
            if (len == 0) {
                hb_blob_destroy(blob);
                return nullptr;
            }
            const char* p = hb_blob_get_data(blob, nullptr);
            sk_sp<SkData> data = SkData::MakeWithProc(p, len, releaseHbBlob, blob);
            tf = font_mgr->makeFromStream(SkMemoryStream::Make(std::move(data)), args);
        }
        if (!tf) return nullptr;
    }

    // 字号：S(=hb scale) × units_to_px，见文件顶部那段推导。
    int sx = 0;
    int sy = 0;
    hb_font_get_scale(hbf, &sx, &sy);
    const float em_px = static_cast<float>(sx) * units_to_px;
    if (!(em_px > 0.0f)) return nullptr;

    const SkiaFonts::FontKey key{tf.get(), fbits(em_px)};
    auto it = fs.fonts.find(key);
    if (it != fs.fonts.end()) return &it->second;

    SkFont f(tf, em_px);
    // 下面几个开关的取值理由。
    //
    // hinting 取 kSlight（不是 kNone）：它对应 FreeType 的 FT_LOAD_TARGET_LIGHT，
    //   只做纵向 hinting，横向仍走亚像素定位（Android 的 FreeType+Skia 同此配置）。
    //   本实现的横向位置来自 HarfBuzz 的推进，光栅化只负责纵向对齐，两者不冲突。
    //   注意 hinting 不是"字浅"的原因：同一段文本只把 kNone 换成 kSlight，平均灰
    //   158 到 163（几乎没动）；字浅的真正原因是变体坐标没带过去（见上面的
    //   hbVariationsToSkia），修正后平均灰从 163 降到 108，与自写光栅器的 90 对齐。
    f.setHinting(SkFontHinting::kSlight);
    f.setEmbeddedBitmaps(false);   // 不用内嵌点阵：那是 12px 以下的补救手段
    f.setForceAutoHinting(false);  // 强制 auto-hint 会改字形，汉字上没必要
    f.setBaselineSnap(false);      // 位置由外部给定，不吸附基线
    f.setSubpixel(true);           // 与 HarfBuzz 的小数推进对齐
    // setLinearMetrics 只影响推进（SkFontHost_FreeType.cpp 里只读
    // linearHoriAdvance），不影响光栅化。这里开不开都行 —— 位置本来就由
    // HarfBuzz 给定，Skia 算出来的 advance 本实现不读。
    f.setLinearMetrics(true);
    // 灰度抗锯齿。不能用 kSubpixelAntiAlias（LCD 子像素）：那会让每个像素
    // 只有 R 或 B 一个通道被写，而本实现的帧是给 canvas.putImageData 的整块 RGBA。
    f.setEdging(SkFont::Edging::kAntiAlias);

    return &fs.fonts.emplace(key, f).first->second;
}

/**
 * 生命周期：构造、析构与移动全部交给编译器默认实现。
 */
Bitmap::Bitmap() = default;
Bitmap::Bitmap(int w, int h) { reset(w, h); }
Bitmap::~Bitmap() = default;
Bitmap::Bitmap(Bitmap&& other) noexcept = default;
Bitmap& Bitmap::operator=(Bitmap&& other) noexcept = default;

void Bitmap::reset(int w, int h) {
    w_ = std::max(w, 0);
    h_ = std::max(h, 0);
    px_.assign(static_cast<size_t>(w_) * static_cast<size_t>(h_) * 4u, 0);

    if (!sk_) {
        sk_ = std::make_unique<SkiaImpl>();
        // 空字体管理器 + FreeType 后端：字体全部由宿主喂进来（生产环境本就如此），
        // 不依赖任何"系统字体目录"，这在 wasm 上是唯一能选的形态。
        sk_->fonts_shared = std::make_shared<SkiaFonts>();
        sk_->fonts_shared->font_mgr = SkFontMgr_New_Custom_Empty();
    }

    // 重新装订。px_ 刚刚 assign 过，地址可能变了；SkBitmap 拿的是裸指针，
    //   不重装订就会画到已释放的内存上。这是整个文件里最容易出静默错误的一处。
    sk_->canvas.reset();
    sk_->bm.reset();
    if (w_ > 0 && h_ > 0 && sk_->fonts_shared && sk_->fonts_shared->font_mgr) {
        const SkImageInfo info =
            SkImageInfo::Make(w_, h_, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        // 行跨度 = w_*4：与 frame() 交给宿主的口径一致，中间不留 padding。
        if (sk_->bm.installPixels(info, px_.data(), static_cast<size_t>(w_) * 4u)) {
            sk_->canvas = std::make_unique<SkCanvas>(sk_->bm);
        }
    }
}

/**
 * 共用字体表。字形缓存按 strike 规格索引、规格里含 SkTypeface 的身份，
 *   所以只有换了字体表，缓存才真的跟着共用（见 Bitmap::shareFonts）。
 */
void Bitmap::shareFonts(const Bitmap& other) {
    if (!sk_ || !other.sk_ || !other.sk_->fonts_shared) return;
    sk_->fonts_shared = other.sk_->fonts_shared;
}

/**
 * 字形 mask 缓存的预算 / 占用 / 上限。走 Skia 的全局 strike 缓存（进程内一份，
 *   与是哪个 Bitmap 无关，所以在哪一块图上设都一样）。
 *
 * 默认值是 SkStrikeCache.h 里的 SK_DEFAULT_FONT_CACHE_LIMIT = 2 MB。为什么这个数
 *   在本文档上不够用：一页约 700 个字形、字号 36 px 量级，一个 mask 数百字节，
 *   再乘上亚像素横向相位（setSubpixel(true) 会把同一个字形按 1/4 px 拆成多份 mask），
 *   一页的常驻量就到 MB 量级 —— 越限之后 internalPurge 按整份 strike 删，
 *   一删全没。于是"预热过的那一页"在下一拍就可能是冷的。
 * 这条只在 Skia 后端有意义：自写后端用自己的 GlyphCache（bitmap.cpp 里是空实现）。
 */
void Bitmap::setGlyphCacheBudget(size_t bytes) {
    if (bytes == 0) return;
    SkGraphics::SetFontCacheLimit(bytes);
}

size_t Bitmap::glyphCacheUsedBytes() const {
    return SkGraphics::GetFontCacheUsed();
}

size_t Bitmap::glyphCacheBudgetBytes() const {
    return SkGraphics::GetFontCacheLimit();
}

/**
 * 铺面积
 * 与自写后端同口径记账：都是"请求写入的像素数"，
 * 整块覆盖（矩形）→ 精确面积。这样 engine.cpp 的两条恒等式
 * （光标移动 = 3×光标矩形；整帧重画 = 整帧 + 2×光标矩形）继续成立。
 */
void Bitmap::clear(Color c) {
    if (w_ <= 0 || h_ <= 0 || !sk_ || !sk_->canvas) return;
    // 与自写后端一致：clear 不计入 painted_px_（它是"重建整块底图"，
    // 不是增量写入；engine.cpp 的记账口径就是这么定的）。
    sk_->canvas->clear(toSk(c));
}

void Bitmap::fillRect(const Rect& r, Color c) {
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
    const int x1 = std::min(w_, static_cast<int>(std::ceil(r.x + r.w)));
    const int y1 = std::min(h_, static_cast<int>(std::ceil(r.y + r.h)));
    if (x1 <= x0 || y1 <= y0) return;
    painted_px_ += static_cast<size_t>(x1 - x0) * static_cast<size_t>(y1 - y0);
    if (!sk_ || !sk_->canvas) return;

    SkPaint p;
    p.setColor(toSk(c));
    p.setAntiAlias(false);  // 矩形已经对齐到整数像素，开 AA 只会引入半透明边
    // kSrcOver：本工程的颜色恒为不透明（Color::rgb 把 a 钉在 255），
    // 不透明源 over 任意底 = 直接覆盖，与自写后端的 memcpy 逐位一致。
    p.setBlendMode(SkBlendMode::kSrcOver);
    sk_->canvas->drawRect(SkRect::MakeLTRB(static_cast<SkScalar>(x0), static_cast<SkScalar>(y0),
                                           static_cast<SkScalar>(x1), static_cast<SkScalar>(y1)),
                          p);
}

void Bitmap::strokeRect(const Rect& r, Color c, float thickness) {
    fillRect(Rect{r.x, r.y, r.w, thickness}, c);
    fillRect(Rect{r.x, r.y + r.h - thickness, r.w, thickness}, c);
    fillRect(Rect{r.x, r.y, thickness, r.h}, c);
    fillRect(Rect{r.x + r.w - thickness, r.y, thickness, r.h}, c);
}

/**
 * 按 alpha 混色。与自写后端（bitmap.cpp）逐字节同一段算术。
 *
 * 这里刻意不走 SkCanvas：那条路上颜色是按 premul 参与运算的，同一份 (r,g,b,a)
 * 过一遍会与自写侧差一个舍入档，而"两个后端连像素都算得一样"正是这个工程拿来
 * 互证的东西（见 bitmap.h 里 drawGlyph 那段关于两条路径的口径）。
 * 直接写 px_ 是安全的：sk_->bm 就是 installPixels 装订到这块内存上的。
 */
void Bitmap::blendRect(const Rect& r, Color c) {
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
    const int x1 = std::min(w_, static_cast<int>(std::ceil(r.x + r.w)));
    const int y1 = std::min(h_, static_cast<int>(std::ceil(r.y + r.h)));
    if (x1 <= x0 || y1 <= y0) return;
    painted_px_ += static_cast<size_t>(x1 - x0) * static_cast<size_t>(y1 - y0);

    const int a = c.a;
    const int ia = 255 - a;
    const size_t row_bytes = static_cast<size_t>(w_) * 4u;
    uint8_t* base = px_.data();
    for (int y = y0; y < y1; ++y) {
        uint8_t* p = base + static_cast<size_t>(y) * row_bytes + static_cast<size_t>(x0) * 4u;
        for (int x = x0; x < x1; ++x, p += 4) {
            p[0] = static_cast<uint8_t>((p[0] * ia + c.r * a + 127) / 255);
            p[1] = static_cast<uint8_t>((p[1] * ia + c.g * a + 127) / 255);
            p[2] = static_cast<uint8_t>((p[2] * ia + c.b * a + 127) / 255);
        }
    }
}

/**
 * 这块不过 Skia，是刻意的：copyRectFrom 是"把一块像素原样搬回去"，
 * 是缓冲到缓冲的拷贝，不是绘制。Skia 能做这件事的只有 drawImage（要先建
 * SkImage、走采样器），既比 memcpy 慢又语义更弱（采样器可能引入插值）。
 * 局部重绘靠它，路径上要的是确定性。
 */
void Bitmap::copyRectFrom(const Bitmap& src, const Rect& r) {
    if (src.w_ != w_ || src.h_ != h_) return;
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
    const int x1 = std::min(w_, static_cast<int>(std::ceil(r.x + r.w)));
    const int y1 = std::min(h_, static_cast<int>(std::ceil(r.y + r.h)));
    if (x1 <= x0 || y1 <= y0) return;
    painted_px_ += static_cast<size_t>(x1 - x0) * static_cast<size_t>(y1 - y0);
    const size_t off0 =
        (static_cast<size_t>(y0) * static_cast<size_t>(w_) + static_cast<size_t>(x0)) * 4u;
    if (x0 == 0 && x1 == w_) {
        // 整幅宽：行与行在内存里首尾相接，一次 memcpy 就是整块（见 bitmap.cpp 同名处）。
        std::memcpy(px_.data() + off0, src.px_.data() + off0,
                    static_cast<size_t>(x1 - x0) * 4u * static_cast<size_t>(y1 - y0));
        return;
    }
    const size_t row_bytes = static_cast<size_t>(x1 - x0) * 4u;
    for (int y = y0; y < y1; ++y) {
        const size_t off = off0 + static_cast<size_t>(y - y0) * static_cast<size_t>(w_) * 4u;
        std::memcpy(px_.data() + off, src.px_.data() + off, row_bytes);
    }
}

/**
 * 字形：把 HarfBuzz 的字形描到帧上，含侧倒竖排的旋转。
 *
 * 单个字形走 drawGlyphs（count = 1）：两条入口只能有一条实现，否则
 *   "逐字形画"与"整段画"会各长一套，差一点就是两边对不上的幽灵。
 */
void Bitmap::drawGlyph(hb_font_t* font, uint32_t glyph_id, float pen_x, float pen_y,
                       float units_to_px, Color color, float rotate_deg) {
    drawGlyphs(font, &glyph_id, &pen_x, &pen_y, 1, units_to_px, color, rotate_deg);
}

/**
 * 批量画字形：一段（同字体、同颜色、同朝向）只付一次固定开销。
 *
 * 病根：drawGlyphs 每调用一次都要新建 SkPaint、装配一遍字形运行、
 *   算一遍运行包围盒 —— 单字形调用时这笔固定开销全摊在一个字上。
 *   实测一页约 1200 个字形要 10 ms 上下，而字形本身的 blit 只占一小半。
 *
 * 记账：Skia 不告诉本实现"写了几个像素"，用字形包围盒当上界（与自写后端的
 *   "只记覆盖率超阈值的像素"相比口径放宽了，用途没变：这个数字要证明的是
 *   "没有写整页"，包围盒是上界，上界都小两个数量级，结论依然硬）。
 *   侧倒时包围盒按旋转后的朝向算，所以宽高对调。
 */
void Bitmap::drawGlyphs(hb_font_t* font, const uint32_t* ids, const float* xs, const float* ys,
                        int count, float units_to_px, Color color, float rotate_deg) {
    if (!font || !ids || !xs || !ys || count <= 0 || w_ <= 0 || h_ <= 0 || !sk_ || !sk_->canvas)
        return;
    const SkFont* f = sk_->fontFor(font, units_to_px);
    if (!f) return;
    painted_glyphs_ += static_cast<size_t>(count);

    const bool sideways = isSideways(rotate_deg);
    std::vector<SkGlyphID>& gids = sk_->glyph_scratch;
    std::vector<SkPoint>& pos = sk_->pos_scratch;
    gids.resize(static_cast<size_t>(count));
    pos.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        gids[static_cast<size_t>(i)] = static_cast<SkGlyphID>(ids[i]);
        pos[static_cast<size_t>(i)] = SkPoint::Make(xs[i], ys[i]);
    }

    // 记账：包围盒一次取完（SkFont::getBounds 的批量重载）。
    //   逐字形 getBounds 每调用一次都要进一遍字形缓存（一次全局查找），
    //   上千个字形时这一笔能盖过字形本身的 blit。
    //
    //   单独计时（见 glyphBoundsMs）：这一段不画像素，"值不值得"与光栅那一段是两笔账。
    //     Skia 侧取包围盒要逐字形进一遍字形缓存，而缓存里没有的那个还要生成一次度量 ——
    //     整页上千个字形时它未必比画字便宜，合成一个数就查不出来了。
    {
        const auto t_bounds0 = std::chrono::steady_clock::now();
        std::vector<SkRect>& bxs = sk_->bounds_scratch;
        bxs.resize(static_cast<size_t>(count));
        f->getBounds(SkSpan<const SkGlyphID>(gids.data(), count), SkSpan<SkRect>(bxs.data(), count),
                     nullptr);
        for (int i = 0; i < count; ++i) {
            float bw = bxs[static_cast<size_t>(i)].width();
            float bh = bxs[static_cast<size_t>(i)].height();
            if (sideways) std::swap(bw, bh);
            if (bw > 0.0f && bh > 0.0f) {
                painted_px_ += static_cast<size_t>(std::ceil(bw) + 1.0f) *
                               static_cast<size_t>(std::ceil(bh) + 1.0f);
            }
        }
        glyph_bounds_ms_ +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_bounds0)
                .count();
    }

    SkPaint p;
    p.setColor(toSk(color));
    p.setAntiAlias(true);

    SkCanvas& c = *sk_->canvas;
    const SkPoint origin = SkPoint::Make(0.0f, 0.0f);

    if (sideways) {
        // 侧倒（蒙文竖排）：字体的轮廓是横排形态，"整段侧倒"属于呈现层。
        //   与自写后端同一定义：绕字形原点顺时针转 rotate_deg。
        // 旋转是逐字形各绕自己的原点转，一次 drawGlyphs 只有一个画布变换表达不了
        //   —— 退回逐字形。这条路少见，不值得为它把变换凑成一个矩阵。
        for (int i = 0; i < count; ++i) {
            const SkPoint zero[1] = {SkPoint::Make(0.0f, 0.0f)};
            c.save();
            c.translate(xs[i], ys[i]);
            c.rotate(rotate_deg);
            c.drawGlyphs(SkSpan<const SkGlyphID>(&gids[static_cast<size_t>(i)], 1),
                         SkSpan<const SkPoint>(zero, 1), origin, *f, p);
            c.restore();
        }
        return;
    }

    // 位置直接给绝对设备像素，省掉逐字形的 save/translate ——
    //   一帧上千个字形，这条路径必须是最短的。
    c.drawGlyphs(SkSpan<const SkGlyphID>(gids.data(), count),
                 SkSpan<const SkPoint>(pos.data(), count), origin, *f, p);
}

/**
 * GlyphCache 在 Skia 后端下只是接口占位（drawGlyph 不再查它）。
 * 这个哈希仍要提供，否则 bitmap.cpp 缺席时会有未定义符号 —— 将来若有人
 * 打开 HEDITOR_USE_SKIA 又去调 glyphCache()->find()，至少能链上。
 */
size_t GlyphCache::KeyHash::operator()(const Key& k) const {
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

}  // namespace heditor

#endif  // defined(HEDITOR_USE_SKIA)
