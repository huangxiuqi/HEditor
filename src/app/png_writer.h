/**
 * 极简 PNG 写出（RGBA8）。
 * 刻意不引 zlib：用 deflate 的 stored（未压缩）块，实现只有几十行且行为完全确定。
 * 代价是文件偏大，对"肉眼验证字形对不对"这个用途完全够用。
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ced {
namespace png {

inline uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        init = true;
    }
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

inline uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

inline void pushBE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

inline void pushChunk(std::vector<uint8_t>& out, const char type[4],
                      const std::vector<uint8_t>& data) {
    pushBE32(out, static_cast<uint32_t>(data.size()));
    const size_t crc_start = out.size();
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(type[i]));
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t c = crc32(out.data() + crc_start, out.size() - crc_start) ^ 0xFFFFFFFFu;
    pushBE32(out, c);
}

/**
 * rgba: w*h*4 字节
 */
inline bool writeRgba(const std::string& path, const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return false;

    const size_t stride = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter type: None
        raw.insert(raw.end(), rgba + static_cast<size_t>(y) * stride,
                   rgba + static_cast<size_t>(y + 1) * stride);
    }

    // zlib 流：stored deflate
    std::vector<uint8_t> idat;
    idat.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    idat.push_back(0x78);
    idat.push_back(0x01);
    size_t offset = 0;
    while (offset < raw.size()) {
        const size_t len = std::min<size_t>(65535, raw.size() - offset);
        const bool last = (offset + len >= raw.size());
        idat.push_back(last ? 0x01 : 0x00);
        idat.push_back(static_cast<uint8_t>(len & 0xFF));
        idat.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        const uint16_t nlen = static_cast<uint16_t>(~len);
        idat.push_back(static_cast<uint8_t>(nlen & 0xFF));
        idat.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFF));
        idat.insert(idat.end(), raw.begin() + offset, raw.begin() + offset + len);
        offset += len;
    }
    const uint32_t ad = adler32(raw.data(), raw.size());
    pushBE32(idat, ad);

    std::vector<uint8_t> out;
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.insert(out.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    pushBE32(ihdr, static_cast<uint32_t>(w));
    pushBE32(ihdr, static_cast<uint32_t>(h));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // color type: RGBA
    ihdr.push_back(0);  // compression
    ihdr.push_back(0);  // filter
    ihdr.push_back(0);  // interlace
    pushChunk(out, "IHDR", ihdr);
    pushChunk(out, "IDAT", idat);
    pushChunk(out, "IEND", {});

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t written = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return written == out.size();
}

}  // namespace png
}  // namespace ced
