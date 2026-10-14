/**
 * 字体加载与字号实例化。
 * 本实现刻意不走浏览器的 measureText：引擎自己解析字体文件，度量才能跨平台与
 * 跨浏览器一致（打印分页才可复现），WASM 侧也不必依赖 DOM。
 * 缩放约定：HarfBuzz 的 font scale 设为 size_pt * kFontScalePerPt。
 * 于是所有 hb_position_t（advance / offset / 轮廓坐标）的单位都是 1/kFontScalePerPt 磅，
 * 换算成排版磅只需除以 kFontScalePerPt。
 */
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;
struct hb_user_data_key_t;

namespace heditor {

/**
 * 每磅用多少个 HarfBuzz scale 单位。256 让 1/256 磅 ≈ 0.005 px（@96dpi），足够精确。
 */
inline constexpr float kFontScalePerPt = 256.0f;

class FontSource;

/**
 * hb_face 上挂 FontSource 的键。
 *
 * render 层只能从 hb_face 反查数据源（它拿不到 FontFace），两处共用这个静态
 * 实例作为标识。
 */
hb_user_data_key_t* fontSourceKey();

/**
 * 字体文件（进程内只加载一次）
 */
class FontFace {
public:
    FontFace() = default;
    ~FontFace();
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;

    bool loadFromFile(const std::string& path);
    /**
     * 从内存加载。用于 WASM：字体由浏览器 fetch 下来再交进来，
     * 而不是打进 wasm 包（生产环境字体本来就来自服务端/CDN）。
     */
    bool loadFromMemory(const std::string& name, const uint8_t* data, size_t len);
    /**
     * 按需模式：磁盘上留原文件，内存里只放骨架（小表）与块缓存。
     *
     * HarfBuzz 拿到的是骨架（排版要读的表一张不少，不足 1 MB），Skia 的 FreeType
     * 后端经过数据源按需读轮廓与变体数据。整份字体里按字形可寻址的大表占九成以上，
     * 所以这条路的常驻降到骨架加块缓存那么多。
     */
    bool loadFromFileLazy(const std::string& path);

    hb_face_t* face() const { return face_; }
    const std::string& path() const { return path_; }
    bool valid() const { return face_ != nullptr; }
    unsigned glyphCount() const;

    /**
     * 按需模式的数据源；整份装载时为空。render 层靠它按需取字节。
     */
    FontSource* source() const { return source_.get(); }

private:
    bool buildFromData(const std::string& name);

    std::string path_;
    std::vector<uint8_t> data_;
    hb_blob_t* blob_ = nullptr;
    hb_face_t* face_ = nullptr;
    std::unique_ptr<FontSource> source_;
};

/**
 * 某个具体字号下的字体实例
 */
class FontInstance {
public:
    FontInstance() = default;
    ~FontInstance();
    FontInstance(const FontInstance&) = delete;
    FontInstance& operator=(const FontInstance&) = delete;
    FontInstance(FontInstance&& other) noexcept { moveFrom(other); }
    FontInstance& operator=(FontInstance&& other) noexcept {
        if (this != &other) {
            destroy();
            moveFrom(other);
        }
        return *this;
    }

    /**
     * weight 用于可变字体（Noto Sans SC VF）；静态字体上无副作用。
     */
    bool init(hb_face_t* face, float size_pt, float weight = 400.0f);

    hb_font_t* hb() const { return font_; }
    float sizePt() const { return size_pt_; }
    unsigned upem() const;

    /**
     * 排版磅 <-> HarfBuzz scale 单位
     */
    float scaleUnitsToPt(int32_t v) const { return static_cast<float>(v) / kFontScalePerPt; }

    /**
     * 横排行高参考（ascent/descent，单位：磅）
     */
    float ascenderPt() const;
    float descenderPt() const;

private:
    void destroy();
    void moveFrom(FontInstance& other);

    hb_font_t* font_ = nullptr;
    float size_pt_ = 12.0f;
};

/**
 * 字体库：持有字体文件，并按 (face, 字号) 缓存 hb_font_t 实例。
 * 缓存的意义在于——同一个字号在一篇文书里会出现成百上千次，
 * 每次都 hb_font_create 会白白制造大量对象。
 */
/**
 * 字体按需读的账（所有数据源之和）。
 *
 * 判"按需是否划算"就靠这几个数：命中率高、落存储次数远小于字形数，才说明块缓存
 * 真的在挡；常驻字节是这条路换来的收益。整份装载的脸没有数据源，对总账没有贡献。
 */
struct FontCacheTotals {
    size_t hits = 0;
    size_t misses = 0;
    size_t storage_reads = 0;
    size_t storage_bytes = 0;
    size_t resident_bytes = 0;
    double io_ms = 0.0;
};

class FontLibrary {
public:
    /**
     * 返回字体索引；失败返回 -1
     */
    int addFace(const std::string& path);
    /**
     * 按需装一张脸：磁盘上留原文件，内存里只放骨架与块缓存。
     */
    int addFaceLazy(const std::string& path);
    int addFaceFromMemory(const std::string& name, const uint8_t* data, size_t len);

    /**
     * 所有按需数据源的统计之和（整份装载的脸不计）。
     */
    FontCacheTotals cacheTotals() const;

    /**
     * 换块缓存的配置（块大小与块数）。已有的按需脸当场换（连同清空块与统计），
     * 之后新装的脸也用它 —— 否则"换配置再重装"会得到一张用旧配置的脸。
     */
    void setCacheConfig(size_t block_size, size_t max_blocks);

    int faceCount() const { return static_cast<int>(faces_.size()); }
    FontFace* face(int index);
    const FontFace* face(int index) const;
    const std::string& facePath(int index) const;

    /**
     * 取（或创建）指定字号下的实例。失败返回 nullptr。
     */
    FontInstance* instance(int face_index, float size_pt);

    size_t instanceCount() const { return instances_.size(); }

private:
    std::vector<std::unique_ptr<FontFace>> faces_;
    std::vector<std::unique_ptr<FontInstance>> instances_;
    /**
     * key: (face_index, 字号 * 4 取整) —— 允许 0.25pt 粒度
     */
    std::map<std::pair<int, int>, FontInstance*> cache_;
    /**
     * 新装的按需脸用这套块缓存配置（setCacheConfig 会改它，见那里的说明）。
     * 默认值与 FontBlockCache 的构造参数一致 —— 依据记在那里。
     */
    size_t cache_block_size_ = 4u * 1024u;
    size_t cache_max_blocks_ = 768u;
};

}  // namespace heditor
