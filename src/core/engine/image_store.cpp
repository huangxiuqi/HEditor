#include "core/engine/image_store.h"

#include "core/utils/hash.h"

namespace heditor {

namespace {

/**
 * 未预乘的 RGBA 就地折成预乘。alpha 为 255 时原样保留 —— 绝大多数截图与照片属于这一档，
 * 多做一次乘法只是让那一片像素抖一位。
 */
void premultiply(uint8_t* px, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint8_t* p = px + i * 4u;
        const uint32_t a = p[3];
        if (a == 255u) continue;
        if (a == 0u) {
            p[0] = 0;
            p[1] = 0;
            p[2] = 0;
            continue;
        }
        // 四舍五入：直接截断会让整片半透明像素系统性地偏暗。
        p[0] = static_cast<uint8_t>((static_cast<uint32_t>(p[0]) * a + 127u) / 255u);
        p[1] = static_cast<uint8_t>((static_cast<uint32_t>(p[1]) * a + 127u) / 255u);
        p[2] = static_cast<uint8_t>((static_cast<uint32_t>(p[2]) * a + 127u) / 255u);
    }
}

}  // namespace

uint64_t imageResourceKey(const std::string& id) {
    const uint64_t h = fnvMixCStr(kFnvBasis, id.c_str());
    return h == 0 ? 1u : h;
}

bool ImageStore::put(const std::string& id, int width, int height, const uint8_t* rgba,
                     size_t bytes, std::string* error) {
    if (error != nullptr) error->clear();
    if (id.empty()) {
        if (error != nullptr) *error = "资源 id 不能为空";
        return false;
    }
    if (width <= 0 || height <= 0) {
        if (error != nullptr) *error = "图片尺寸必须是正数";
        return false;
    }
    if (rgba == nullptr) {
        if (error != nullptr) *error = "像素指针为空";
        return false;
    }
    const size_t need = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (bytes != need) {
        if (error != nullptr) {
            *error = "字节数与尺寸不符：期望 " + std::to_string(need) + "，实得 " +
                     std::to_string(bytes);
        }
        return false;
    }
    // 替换时先扣掉旧的那一份，再按新的一份算额度 —— 否则换一张同样大的图会被自己的旧账挤掉。
    const auto it = items_.find(id);
    const size_t old = (it == items_.end()) ? 0u : it->second.rgba.size();
    if (bytes_ - old + need > kMaxBytes) {
        if (error != nullptr) {
            *error = "图片资源超出上限：" + std::to_string(kMaxBytes / (1024u * 1024u)) + " MB";
        }
        return false;
    }

    ImageResource res;
    res.id = id;
    res.width = width;
    res.height = height;
    res.rgba.assign(rgba, rgba + need);
    premultiply(res.rgba.data(), static_cast<size_t>(width) * static_cast<size_t>(height));
    res.version = next_version_++;

    if (it != items_.end()) items_.erase(it);
    items_.emplace(id, std::move(res));
    bytes_ = bytes_ - old + need;
    ++generation_;
    return true;
}

bool ImageStore::erase(const std::string& id) {
    const auto it = items_.find(id);
    if (it == items_.end()) return false;
    bytes_ -= it->second.rgba.size();
    items_.erase(it);
    ++generation_;
    return true;
}

void ImageStore::clear() {
    if (items_.empty()) return;
    items_.clear();
    bytes_ = 0;
    ++generation_;
}

const ImageResource* ImageStore::find(const std::string& id) const {
    const auto it = items_.find(id);
    return (it == items_.end()) ? nullptr : &it->second;
}

}  // namespace heditor
