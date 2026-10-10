#include "core/doc/document.h"

#include <algorithm>

#include "core/utils/utf8.h"

namespace ced {
namespace {

/**
 * 码点 → UTF-8 追加到 out。非法码点（代理区/越界）写 U+FFFD，
 * 保证输出的字节流永远是合法 UTF-8 —— 这一点很重要：这个字符串会直接被
 * wasm 边界交给 JS，一个非法序列会让 JS 侧的解码整体错位。
 */
void appendUtf8(std::string& out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

std::vector<uint32_t> utf8ToCodepoints(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size() / 2 + 1);
    size_t off = 0;
    while (off < s.size()) {
        size_t next = 0;
        const uint32_t cp = decodeUtf8(s, off, &next);
        if (next <= off) break;  // 防御：解码器不推进就停，绝不空转
        out.push_back(cp);
        off = next;
    }
    return out;
}

std::string codepointsToUtf8(const uint32_t* cps, size_t n) {
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) appendUtf8(out, cps[i]);
    return out;
}

std::string codepointsToUtf8(const std::vector<uint32_t>& cps) {
    return codepointsToUtf8(cps.data(), cps.size());
}

namespace textbuf {

void insertAt(std::vector<uint32_t>& buf, size_t offset, const uint32_t* cps, size_t n) {
    if (!cps || n == 0) return;
    if (offset > buf.size()) offset = buf.size();
    buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(offset), cps, cps + n);
}

void insertAt(std::vector<uint32_t>& buf, size_t offset, const std::vector<uint32_t>& cps) {
    insertAt(buf, offset, cps.data(), cps.size());
}

size_t eraseRange(std::vector<uint32_t>& buf, size_t begin, size_t end) {
    if (begin > buf.size()) begin = buf.size();
    if (end > buf.size()) end = buf.size();
    if (begin >= end) return 0;
    buf.erase(buf.begin() + static_cast<std::ptrdiff_t>(begin),
              buf.begin() + static_cast<std::ptrdiff_t>(end));
    return end - begin;
}

}  // namespace textbuf

size_t clusterFloor(const std::vector<uint32_t>& starts, size_t offset) {
    // 二分：找最后一个 <= offset 的元素
    size_t best = 0;
    size_t lo = 0, hi = starts.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (starts[mid] <= offset) {
            best = starts[mid];
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return best;
}

size_t clusterCeil(const std::vector<uint32_t>& starts, size_t offset) {
    // 二分：找第一个 > offset 的元素
    size_t lo = 0, hi = starts.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (starts[mid] <= offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < starts.size() ? starts[lo] : offset;
}

}  // namespace ced
