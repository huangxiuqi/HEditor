#include "core/text/font.h"

#include <hb-ot.h>
#include <hb.h>

#include <cmath>
#include <fstream>
#include <iterator>

namespace ced {

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
    return true;
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

}  // namespace ced
