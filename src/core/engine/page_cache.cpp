/**
 * 页缓存的实现：淘汰、脏矩形合并、新鲜度判定。
 *
 * 这三件事都要看"页面上哪一块、什么几何"，与文档内容无关，所以整块搬到这里；判据
 * （逐像素的帧审计、脏矩形面积）一条没改，搬动本身不改行为。
 */
#include "core/engine/page_cache.h"

#include <algorithm>

namespace heditor {
namespace {

/**
 * 两个矩形相接或相交（容差一像素）。
 *
 * 容差给一像素是因为脏矩形来自浮点几何，同一块像素两次算出来可能差零点几磅：
 * 不认这点容差，光标"擦旧画新"那两块永远并不成一块，宿主收到两条挨着的窄条。
 */
bool rectsTouch(const Rect& a, const Rect& b) {
    return a.x <= b.x + b.w + 1.0f && b.x <= a.x + a.w + 1.0f && a.y <= b.y + b.h + 1.0f &&
           b.y <= a.y + a.h + 1.0f;
}

/**
 * 两个矩形的并集（外接矩形）。
 * @param a 左矩形
 * @param b 右矩形
 * @returns 并集
 */
Rect rectUnion(const Rect& a, const Rect& b) {
    const float x0 = std::min(a.x, b.x);
    const float y0 = std::min(a.y, b.y);
    const float x1 = std::max(a.x + a.w, b.x + b.w);
    const float y1 = std::max(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

bool PageCache::setSignature(uint64_t sig) {
    if (sig == sig_) return false;
    sig_ = sig;
    dirty_.clear();
    return true;
}

PageSlot* PageCache::find(int page) {
    const auto it = slots_.find(page);
    return (it == slots_.end()) ? nullptr : &it->second;
}

const PageSlot* PageCache::find(int page) const {
    const auto it = slots_.find(page);
    return (it == slots_.end()) ? nullptr : &it->second;
}

PageSlot& PageCache::at(int page) { return slots_[page]; }

bool PageCache::contains(int page) const { return slots_.find(page) != slots_.end(); }

void PageCache::clear() {
    slots_.clear();
    dirty_.clear();
}

void PageCache::clearDirty() { dirty_.clear(); }

void PageCache::erase(int page) {
    slots_.erase(page);
    dirty_.erase(page);
}

void PageCache::trim(int first_page, int count) {
    // 窗口内一页都不丢：它们是下一拍很可能还要用的，而且页位图就是导出像素的唯一来源。
    const std::size_t cap = static_cast<std::size_t>(std::max(count + 2, 4));
    if (slots_.size() <= cap) return;
    std::vector<std::pair<int, int>> far;  // (距离, 页号)
    far.reserve(slots_.size());
    for (const auto& kv : slots_) {
        const int p = kv.first;
        if (p >= first_page && p < first_page + count) continue;
        const int d = (p < first_page) ? (first_page - p) : (p - (first_page + count - 1));
        far.push_back({d, p});
    }
    // 远处先丢；同样远的先丢页号大的（往回滚更可能先用到小页号）。
    std::sort(far.begin(), far.end(),
              [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second > b.second;
              });
    for (const auto& e : far) {
        if (slots_.size() <= cap) break;
        erase(e.second);
    }
}

bool PageCache::fresh(int page, uint64_t fp, int px_w, int px_h) const {
    const PageSlot* pb = find(page);
    if (pb == nullptr || !pb->valid) return false;
    if (pb->px.width() != px_w || pb->px.height() != px_h) return false;
    // 内容指纹之外还有一份"光栅配置"的签名：底色、边框、缩放、页度量这些变了，
    //   内容一个字节都没动，但画出来的像素不一样。签名是这里唯一的判据 —— 不再靠
    //   setConfig 记得清缓存。
    if (pb->raster_sig != sig_) return false;
    return pb->fp == fp;
}

void PageCache::noteDirty(int page, const Rect& r) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    std::vector<Rect>& v = dirty_[page];
    v.push_back(r);
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < v.size() && !merged; ++i) {
            for (size_t j = i + 1; j < v.size(); ++j) {
                if (!rectsTouch(v[i], v[j])) continue;
                v[i] = rectUnion(v[i], v[j]);
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(j));
                merged = true;
                break;
            }
        }
    }
    if (v.size() > 16) {
        Rect u = v.front();
        for (const Rect& q : v) u = rectUnion(u, q);
        v.assign(1, u);
    }
}

std::vector<Rect> PageCache::takeDirty(int page) {
    std::vector<Rect> out;
    const auto it = dirty_.find(page);
    if (it == dirty_.end()) return out;
    out.swap(it->second);
    dirty_.erase(it);
    return out;
}

bool PageCache::hasDirty(int page) const {
    const auto it = dirty_.find(page);
    return it != dirty_.end() && !it->second.empty();
}

std::vector<int> PageCache::dirtyPages() const {
    std::vector<int> pages;
    pages.reserve(dirty_.size());
    for (const auto& kv : dirty_) {
        if (!kv.second.empty()) pages.push_back(kv.first);
    }
    std::sort(pages.begin(), pages.end());
    return pages;
}

}  // namespace heditor
