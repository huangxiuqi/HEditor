/**
 * 极简 UTF-8 解码（只做断行判定与码点分类用）
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace heditor {

/**
 * 从 s[pos] 解出一个码点，*next 写回下一个字节位置。非法序列按单字节跳过。
 * 参数取 string_view：调用方既有整串（std::string）也有切片（子串视图），
 * 用 string_view 可以避免为了切一刀而复制字符串。
 */
inline uint32_t decodeUtf8(std::string_view s, size_t pos, size_t* next) {
    if (pos >= s.size()) {
        if (next) *next = pos;
        return 0;
    }
    const uint8_t c0 = static_cast<uint8_t>(s[pos]);
    size_t len = 1;
    uint32_t cp = c0;
    if ((c0 & 0x80) == 0x00) {
        len = 1;
        cp = c0;
    } else if ((c0 & 0xE0) == 0xC0) {
        len = 2;
        cp = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        len = 3;
        cp = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        len = 4;
        cp = c0 & 0x07;
    } else {
        if (next) *next = pos + 1;
        return 0xFFFD;
    }
    if (pos + len > s.size()) {
        if (next) *next = pos + 1;
        return 0xFFFD;
    }
    for (size_t i = 1; i < len; ++i) {
        const uint8_t ci = static_cast<uint8_t>(s[pos + i]);
        if ((ci & 0xC0) != 0x80) {
            if (next) *next = pos + 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (ci & 0x3F);
    }
    if (next) *next = pos + len;
    return cp;
}

}  // namespace heditor
