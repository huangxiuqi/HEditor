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
#include <tuple>
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
 * @return 那个静态键（render 层据此从 hb_face 反查数据源）
 */
hb_user_data_key_t* fontSourceKey();

/**
 * 字体文件（进程内只加载一次）
 */
class FontFace {
public:

    FontFace() = default;

    /**
     * 释放 face、blob 与数据源。
     */
    ~FontFace();
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;

    /**
     * 从磁盘整份装进来。
     * @param path 字体文件路径
     * @return true = 装载成功
     */
    bool loadFromFile(const std::string& path);

    /**
     * 从内存加载。用于 WASM：字体由浏览器 fetch 下来再交进来，
     * 而不是打进 wasm 包（生产环境字体本来就来自服务端/CDN）。
     * @param name 名字（仅用于报错与 facePath）
     * @param data 字体字节
     * @param len 字节数
     * @return true = 装载成功
     */
    bool loadFromMemory(const std::string& name, const uint8_t* data, size_t len);

    /**
     * 按需模式：磁盘上留原文件，内存里只放骨架（小表）与块缓存。
     *
     * HarfBuzz 拿到的是骨架（排版要读的表一张不少，不足 1 MB），Skia 的 FreeType
     * 后端经过数据源按需读轮廓与变体数据。整份字体里按字形可寻址的大表占九成以上，
     * 所以这条路的常驻降到骨架加块缓存那么多。
     * @param path 字体文件路径
     * @return true = 骨架装载成功
     */
    bool loadFromFileLazy(const std::string& path);

    /**
     * HarfBuzz 的 face。
     * @return face 句柄；未装载时为 nullptr
     */
    hb_face_t* face() const { return face_; }

    /**
     * 装载时的路径（内存装载时是给的名字）。
     * @return 路径
     */
    const std::string& path() const { return path_; }

    /**
     * 装好了没有。
     * @return true = face 非空
     */
    bool valid() const { return face_ != nullptr; }

    /**
     * 字形总数。
     * @return 字形数
     */
    unsigned glyphCount() const;

    /**
     * 字体自己说的名字（OpenType name 表）。
     *
     * 存进文档的字体标识必须比文件名稳：文件可以改名，字体本身不能。PostScript 名
     * （name ID 6）是 OpenType 里唯一性有规范保证的那一个，所以它当标识，族名只给人看。
     * 骨架模式也读得到 —— name 表很小，按需装载时留在骨架里（见 FontSource::inSkeleton）。
     * @return PostScript 名；读不到时是空串
     */
    const std::string& postScriptName() const { return ps_name_; }

    /**
     * 族名（给人看，不当标识）。
     * @return 族名；读不到时是空串
     */
    const std::string& familyName() const { return family_name_; }

    /**
     * 按需模式的数据源；整份装载时为空。render 层靠它按需取字节。
     * @return 按需模式的数据源；整份装载时为 nullptr
     */
    FontSource* source() const { return source_.get(); }

private:

    /**
     * 用已就位的 data_ 造 blob 与 face。
     * @param name 名字（name 表读不到时用它）
     * @return true = 造出来了
     */
    bool buildFromData(const std::string& name);

    /**
     * 读 OpenType name 表里的两个名字。失败时留空串（有些子集化的字体把这几个 ID 剥掉了）。
     */
    void readNames();

    /**
     * 装载路径（内存装载时是给的名字）。
     */
    std::string path_;

    /**
     * PostScript 名（name ID 6）。
     */
    std::string ps_name_;

    /**
     * 族名（name ID 1）。
     */
    std::string family_name_;

    /**
     * 整份装载时的字体字节；按需模式为空。
     */
    std::vector<uint8_t> data_;

    /**
     * HarfBuzz 的 blob，引用 data_ 或按需数据源。
     */
    hb_blob_t* blob_ = nullptr;

    /**
     * HarfBuzz 的 face。
     */
    hb_face_t* face_ = nullptr;

    /**
     * 按需模式的数据源；整份装载时为空。
     */
    std::unique_ptr<FontSource> source_;
};

/**
 * 某个具体字号下的字体实例
 */
class FontInstance {
public:

    FontInstance() = default;

    /**
     * 释放 hb_font。
     */
    ~FontInstance();
    FontInstance(const FontInstance&) = delete;
    FontInstance& operator=(const FontInstance&) = delete;

    /**
     * 移动构造：接管对方的手柄，对方置空。
     * @param other 被移动的对象
     */
    FontInstance(FontInstance&& other) noexcept { moveFrom(other); }

    /**
     * 移动赋值：先释放自己的，再接管对方的手柄。
     * @param other 被移动的对象
     * @return *this
     */
    FontInstance& operator=(FontInstance&& other) noexcept {
        if (this != &other) {
            destroy();
            moveFrom(other);
        }
        return *this;
    }

    /**
     * weight 用于可变字体（Noto Sans SC VF）；静态字体上无副作用。
     * @param face HarfBuzz face
     * @param size_pt 字号（磅）
     * @param weight 字重（可变字体用，静态字体上无副作用）
     * @return true = 实例造出来了
     */
    bool init(hb_face_t* face, float size_pt, float weight = 400.0f);

    /**
     * HarfBuzz 的 font。
     * @return font 句柄
     */
    hb_font_t* hb() const { return font_; }

    /**
     * 这个实例的字号。
     * @return 字号（磅）
     */
    float sizePt() const { return size_pt_; }

    /**
     * 每 em 多少个字体单位。
     * @return units per em
     */
    unsigned upem() const;

    /**
     * 排版磅 <-> HarfBuzz scale 单位
     * @param v HarfBuzz scale 单位
     * @return 磅（除以 kFontScalePerPt）
     */
    float scaleUnitsToPt(int32_t v) const { return static_cast<float>(v) / kFontScalePerPt; }

    /**
     * 横排行高参考（ascent/descent，单位：磅）
     * @return 基线以上多少（磅）
     */
    float ascenderPt() const;

    /**
     * 横排行高参考的下降部分。
     * @return 基线以下多少（磅）
     */
    float descenderPt() const;

private:

    /**
     * 释放 hb_font 并置空。
     */
    void destroy();

    /**
     * 接管对方的手柄，对方置空。
     * @param other 被移动的对象
     */
    void moveFrom(FontInstance& other);

    /**
     * HarfBuzz 的 font（init 之后才非空）。
     */
    hb_font_t* font_ = nullptr;

    /**
     * 字号（磅）。
     */
    float size_pt_ = 12.0f;
};

/**
 * 字体按需读的账（所有数据源之和）。
 *
 * 判"按需是否划算"就靠这几个数：命中率高、落存储次数远小于字形数，才说明块缓存
 * 真的在挡；常驻字节是这条路换来的收益。整份装载的脸没有数据源，对总账没有贡献。
 */
struct FontCacheTotals {
    /**
     * 块缓存的命中次数。
     */
    size_t hits = 0;

    /**
     * 块缓存的未命中次数。
     */
    size_t misses = 0;

    /**
     * 落到存储上读了几次。
     */
    size_t storage_reads = 0;

    /**
     * 从存储读进来的累计字节。
     */
    size_t storage_bytes = 0;

    /**
     * 当前常驻的块字节。
     */
    size_t resident_bytes = 0;

    /**
     * 真失败的次数（与因文件尾短读的 eof_reads 分开）。
     */
    size_t read_failures = 0;

    /**
     * 因请求越过文件尾而短读的次数。正常终止，不是失败。
     */
    size_t eof_reads = 0;

    /**
     * 存储读花掉的毫秒。
     */
    double io_ms = 0.0;
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
     * @param path 字体文件路径
     * @return 字体索引；失败为 -1
     */
    int addFace(const std::string& path);

    /**
     * 按需装一张脸：磁盘上留原文件，内存里只放骨架与块缓存。
     * @param path 字体文件路径
     * @return 字体索引；失败为 -1
     */
    int addFaceLazy(const std::string& path);

    /**
     * 从内存装一张脸（WASM 用）。
     * @param name 名字（仅用于报错与 facePath）
     * @param data 字体字节
     * @param len 字节数
     * @return 字体索引；失败为 -1
     */
    int addFaceFromMemory(const std::string& name, const uint8_t* data, size_t len);

    /**
     * 所有按需数据源的统计之和（整份装载的脸不计）。
     * @return 各数据源统计之和
     */
    FontCacheTotals cacheTotals() const;

    /**
     * 换块缓存的配置（块大小与块数）。已有的按需脸当场换（连同清空块与统计），
     * 之后新装的脸也用它 —— 否则"换配置再重装"会得到一张用旧配置的脸。
     * @param block_size 块大小（字节）
     * @param max_blocks 每张脸最多几块
     */
    void setCacheConfig(size_t block_size, size_t max_blocks);

    /**
     * 装了几张脸。
     * @return 脸的个数
     */
    int faceCount() const { return static_cast<int>(faces_.size()); }

    /**
     * 按索引取脸。
     * @param index 字体索引
     * @return 脸；越界为 nullptr
     */
    FontFace* face(int index);

    /**
     * 按索引取脸（只读）。
     * @param index 字体索引
     * @return 脸；越界为 nullptr
     */
    const FontFace* face(int index) const;

    /**
     * 按索引取脸的路径。
     * @param index 字体索引
     * @return 路径；越界为空串
     */
    const std::string& facePath(int index) const;

    /**
     * 取（或创建）指定字号与字重下的实例。失败返回 nullptr。
     *
     * 字重进缓存键：同一个字号下 400 与 700 是两份实例（可变字体的 wght 轴不同），
     *   合成粗体那种"绘制时再加粗"的做法做不到字宽跟着变，排版与字形就会错位。
     * @param face_index 字体索引
     * @param size_pt 字号（磅）
     * @param weight 字重（可变字体用；静态字体上无副作用）
     * @return 实例；失败为 nullptr
     */
    FontInstance* instance(int face_index, float size_pt, float weight = 400.0f);

    /**
     * 缓存里的实例数。
     * @return 实例个数
     */
    size_t instanceCount() const { return instances_.size(); }

private:

    /**
     * 装进来的脸，下标就是字体索引。
     */
    std::vector<std::unique_ptr<FontFace>> faces_;

    /**
     * 造出来的实例，持有它们的所有权（cache_ 只存裸指针）。
     */
    std::vector<std::unique_ptr<FontInstance>> instances_;

    /**
     * key: (face_index, 字号 * 4 取整, 字重 * 4 取整) —— 都是 0.25 粒度
     */
    std::map<std::tuple<int, int, int>, FontInstance*> cache_;

    /**
     * 新装的按需脸用这套块缓存配置（setCacheConfig 会改它，见那里的说明）。
     * 默认值与 FontBlockCache 的构造参数一致 —— 依据记在那里。
     */
    size_t cache_block_size_ = 4u * 1024u;

    /**
     * 每张按需脸最多几块（新装的用它）。
     */
    size_t cache_max_blocks_ = 1024u;
};

}  // namespace heditor
