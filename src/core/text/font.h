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

namespace ced {

/**
 * 每磅用多少个 HarfBuzz scale 单位。256 让 1/256 磅 ≈ 0.005 px（@96dpi），足够精确。
 */
inline constexpr float kFontScalePerPt = 256.0f;

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

    hb_face_t* face() const { return face_; }
    const std::string& path() const { return path_; }
    bool valid() const { return face_ != nullptr; }
    unsigned glyphCount() const;

private:
    bool buildFromData(const std::string& name);

    std::string path_;
    std::vector<uint8_t> data_;
    hb_blob_t* blob_ = nullptr;
    hb_face_t* face_ = nullptr;
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
class FontLibrary {
public:
    /**
     * 返回字体索引；失败返回 -1
     */
    int addFace(const std::string& path);
    int addFaceFromMemory(const std::string& name, const uint8_t* data, size_t len);

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
};

}  // namespace ced
