#include "core/rev/utf8.h"

#include "core/utils/utf8.h"

namespace heditor {

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
    for (size_t i = 0; i < n; ++i) appendUtf8(&out, cps[i]);
    return out;
}

std::string codepointsToUtf8(const std::vector<uint32_t>& cps) {
    return codepointsToUtf8(cps.data(), cps.size());
}

}  // namespace heditor
