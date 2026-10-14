/**
 * FNV-1a 哈希混合器，供各处指纹/缓存键共用。
 * 三处需要它：页内容指纹、表单标记计划指纹、段级整形指纹（layout）。
 * 必须共用同一套混合，否则会出现「同一份输入在两个模块算出不同指纹」这类静默错位。
 * 非密码学强度：只要求内容相等则值相等、内容不同则值大概率不同。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace heditor {

/**
 * FNV-1a 64 位偏移基数。
 */
inline constexpr uint64_t kFnvBasis = 1469598103934665603ULL;
/**
 * FNV-1a 64 位质数。
 */
inline constexpr uint64_t kFnvPrime = 1099511628211ULL;

/**
 * 指纹里的"已知变了"哨兵位。
 *
 * 指纹有两个来源：一是真去哈希内容（computePageFingerprints / computeLineFingerprints），
 * 二是排版那侧直接告知"这一段变了，不必问"（后缀整体平移时后缀每一页都变了，答案早已
 * 确定，见 EditorEngine::markPagesChangedFrom）。后一种写下的不是真指纹，所以必须在编码上
 * 就与真指纹不相交：真指纹一律清掉这一位，标脏值把它置起来，两者不可能相等。
 * 少了这条约定，标脏值就有可能与某个真指纹撞上，症状是"那一页永远不重画"。
 */
inline constexpr uint64_t kFpDirtyBit = 1ULL << 63;

/**
 * 把 64 位值逐字节混入哈希。
 * @note 逐字节（而非单次异或再乘）是页指纹沿用至今的算法，此处原样保留：
 * 换算法等于换指纹，只影响碰撞分布，没有收益。
 */
inline uint64_t fnvMix(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFULL;
        h *= kFnvPrime;
    }
    return h;
}

/**
 * 按位混入一个浮点数。
 * @note 不做量化：量化适用于「画面是否一致」这类判据，不适用于缓存键 ——
 * 量化会把 12.0 与 12.001 判成同一份缓存。
 */
inline uint64_t fnvMixFloat(uint64_t h, float f) {
    uint32_t bits = 0;
    static_assert(sizeof(float) == sizeof(uint32_t), "float 不是 4 字节");
    std::memcpy(&bits, &f, sizeof(bits));
    return fnvMix(h, bits);
}

/**
 * 逐字节混入一段内存。
 */
inline uint64_t fnvMixBytes(uint64_t h, const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = fnvMix(h, b[i]);
    return h;
}

/**
 * 混入 uint32 序列（长度一并混入，避免前缀相同长度不同的两个序列同值）。
 */
inline uint64_t fnvMixU32s(uint64_t h, const std::vector<uint32_t>& v) {
    h = fnvMix(h, v.size());
    for (uint32_t x : v) h = fnvMix(h, x);
    return h;
}

/**
 * 混入 C 字符串（nullptr 与非空串区分对待）。
 */
inline uint64_t fnvMixCStr(uint64_t h, const char* s) {
    if (!s) return fnvMix(h, 0xFFFFFFFFu);
    for (const char* p = s; *p; ++p) h = fnvMix(h, static_cast<unsigned char>(*p));
    return fnvMix(h, 0);
}

}  // namespace heditor
