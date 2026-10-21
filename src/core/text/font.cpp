#include "core/text/font.h"

#include "core/text/font_source.h"

#include <hb-ot.h>
#include <hb.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>

namespace heditor {

FontFace::~FontFace() {
    if (face_) hb_face_destroy(face_);
    if (blob_) hb_blob_destroy(blob_);
}

bool FontFace::loadFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    data_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (data_.empty()) return false;
    return buildFromData(path);
}

bool FontFace::loadFromMemory(const std::string& name, const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    data_.assign(data, data + len);
    return buildFromData(name);
}

bool FontFace::loadFromFileLazy(const std::string& path) {
    auto src = std::make_unique<FontSource>();
    if (!src->open(path)) return false;

    std::vector<uint8_t> skeleton;
    if (!src->buildSkeleton(&skeleton)) return false;

    data_ = std::move(skeleton);
    source_ = std::move(src);
    if (!buildFromData(path)) {
        source_.reset();
        return false;
    }
    // 数据源挂在 hb_face 上：render 层手里只有 hb_face（拿不到 FontFace），
    //   它靠这个键反查按需取字节的那条路。销毁交给 FontFace 自己管，所以
    //   destroy 传空 —— 两者的生命周期都是到进程结束，不会先走一个。
    hb_face_set_user_data(face_, fontSourceKey(), source_.get(), nullptr, false);
    return true;
}

hb_user_data_key_t* fontSourceKey() {
    static hb_user_data_key_t key;
    return &key;
}

bool FontFace::buildFromData(const std::string& name) {
    path_ = name;
    // READONLY：数据由本实现自己持有，交给 HarfBuzz 引用，不产生拷贝。
    blob_ = hb_blob_create(reinterpret_cast<const char*>(data_.data()),
                           static_cast<unsigned>(data_.size()), HB_MEMORY_MODE_READONLY, nullptr,
                           nullptr);
    face_ = hb_face_create(blob_, 0);
    if (!face_ || hb_face_get_glyph_count(face_) == 0) {
        if (face_) {
            hb_face_destroy(face_);
            face_ = nullptr;
        }
        return false;
    }
    readNames();
    return true;
}

namespace {

/**
 * 从 name 表读一个名字。HB_LANGUAGE_INVALID 的意思不是"不查语言"，而是"按英文优先"：
 * OpenType 规定每个名字至少有一份英文记录，族名与 PostScript 名都在其中。
 */
std::string nameOfId(hb_face_t* face, hb_ot_name_id_t id) {
    unsigned int size = 0;
    const unsigned int need = hb_ot_name_get_utf8(face, id, HB_LANGUAGE_INVALID, &size, nullptr);
    if (need == 0) return {};
    std::vector<char> buf(static_cast<size_t>(need) + 1, '\0');
    unsigned int cap = need + 1;
    const unsigned int got = hb_ot_name_get_utf8(face, id, HB_LANGUAGE_INVALID, &cap, buf.data());
    if (got == 0) return {};
    return std::string(buf.data(), got);
}

}  // namespace

void FontFace::readNames() {
    if (!face_) return;
    ps_name_ = nameOfId(face_, HB_OT_NAME_ID_POSTSCRIPT_NAME);
    // 排版族名（ID 16）优先：它是不带字重后缀的那一个（"Noto Sans SC"），
    //   ID 1 在有 ID 16 的字体里带的是"族 + 字重"的老式写法。
    family_name_ = nameOfId(face_, HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY);
    if (family_name_.empty()) family_name_ = nameOfId(face_, HB_OT_NAME_ID_FONT_FAMILY);
}

unsigned FontFace::glyphCount() const { return face_ ? hb_face_get_glyph_count(face_) : 0u; }

FontInstance::~FontInstance() { destroy(); }

void FontInstance::destroy() {
    if (font_) {
        hb_font_destroy(font_);
        font_ = nullptr;
    }
}

void FontInstance::moveFrom(FontInstance& other) {
    font_ = other.font_;
    size_pt_ = other.size_pt_;
    other.font_ = nullptr;
}

bool FontInstance::init(hb_face_t* face, float size_pt, float weight) {
    if (!face) return false;
    destroy();

    size_pt_ = size_pt;
    font_ = hb_font_create(face);
    if (!font_) return false;

    // 必须挂 ot funcs，否则拿不到 OpenType 度量，hb_font_draw_glyph 也没有轮廓实现。
    hb_ot_font_set_funcs(font_);

    const int scale = static_cast<int>(std::lround(size_pt * kFontScalePerPt));
    hb_font_set_scale(font_, scale, scale);

    // 可变字体：显式钉住字重，避免不同构建默认实例不一致导致度量漂移。
    hb_variation_t var{};
    var.tag = hb_tag_from_string("wght", -1);
    var.value = weight;
    hb_font_set_variations(font_, &var, 1);

    return true;
}

unsigned FontInstance::upem() const {
    if (!font_) return 1000u;
    return hb_face_get_upem(hb_font_get_face(font_));
}

float FontInstance::ascenderPt() const {
    if (!font_) return size_pt_;
    hb_font_extents_t ext{};
    if (!hb_font_get_h_extents(font_, &ext)) return size_pt_ * 0.8f;
    // hb_font_extents_t 的 ascender 是正值（实测 NotoSansSC 12pt → +13.92 磅），
    //   表示基线之上的高度。不要取负——取了基线会跑到页面外。
    return scaleUnitsToPt(ext.ascender);
}

float FontInstance::descenderPt() const {
    if (!font_) return size_pt_ * 0.25f;
    hb_font_extents_t ext{};
    if (!hb_font_get_h_extents(font_, &ext)) return size_pt_ * 0.25f;
    // descender 是负值（基线之下），取反成"向下的高度"
    return -scaleUnitsToPt(ext.descender);
}

int FontLibrary::addFace(const std::string& path) {
    auto f = std::make_unique<FontFace>();
    if (!f->loadFromFile(path)) return -1;
    faces_.push_back(std::move(f));
    return static_cast<int>(faces_.size()) - 1;
}

int FontLibrary::addFaceLazy(const std::string& path) {
    auto f = std::make_unique<FontFace>();
    if (!f->loadFromFileLazy(path)) return -1;
    // 新脸按当前配置建缓存 —— 否则"换配置再重装"拿到的仍是默认档的那套。
    if (FontSource* src = f->source()) src->configureCache(cache_block_size_, cache_max_blocks_);
    faces_.push_back(std::move(f));
    return static_cast<int>(faces_.size()) - 1;
}

int FontLibrary::addFaceFromMemory(const std::string& name, const uint8_t* data, size_t len) {
    auto f = std::make_unique<FontFace>();
    if (!f->loadFromMemory(name, data, len)) return -1;
    faces_.push_back(std::move(f));
    return static_cast<int>(faces_.size()) - 1;
}

FontFace* FontLibrary::face(int index) {
    if (index < 0 || index >= static_cast<int>(faces_.size())) return nullptr;
    return faces_[static_cast<size_t>(index)].get();
}

const FontFace* FontLibrary::face(int index) const {
    if (index < 0 || index >= static_cast<int>(faces_.size())) return nullptr;
    return faces_[static_cast<size_t>(index)].get();
}

const std::string& FontLibrary::facePath(int index) const {
    static const std::string kEmpty;
    const FontFace* f = face(index);
    return f ? f->path() : kEmpty;
}

FontCacheTotals FontLibrary::cacheTotals() const {
    FontCacheTotals t;
    for (const auto& f : faces_) {
        const FontSource* src = f->source();
        if (src == nullptr) continue;  // 整份装载的脸没有数据源
        const FontBlockCache& c = src->cache();
        t.hits += c.hits();
        t.misses += c.misses();
        t.storage_reads += c.storageReads();
        t.storage_bytes += c.storageBytes();
        t.resident_bytes += c.residentBytes();
        t.read_failures += c.readFailures();
        t.eof_reads += c.eofReads();
        t.io_ms += c.ioMs();
    }
    return t;
}

void FontLibrary::setCacheConfig(size_t block_size, size_t max_blocks) {
    cache_block_size_ = block_size > 0 ? block_size : 1u;
    cache_max_blocks_ = max_blocks > 0 ? max_blocks : 1u;
    // 已有的脸当场换（configureCache 会清空它们已有的块与统计），
    //   于是"换配置 → 重装文档 → 量"这条路得到的账是干净的。
    for (const auto& f : faces_) {
        FontSource* src = f->source();
        if (src != nullptr) src->configureCache(cache_block_size_, cache_max_blocks_);
    }
}

FontInstance* FontLibrary::instance(int face_index, float size_pt) {
    const FontFace* f = face(face_index);
    if (!f) return nullptr;

    const int key_size = static_cast<int>(std::lround(size_pt * 4.0f));
    const auto key = std::make_pair(face_index, key_size);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    auto inst = std::make_unique<FontInstance>();
    if (!inst->init(const_cast<hb_face_t*>(f->face()), size_pt)) return nullptr;
    FontInstance* raw = inst.get();
    instances_.push_back(std::move(inst));
    cache_.emplace(key, raw);
    return raw;
}

}  // namespace heditor
