/**
 * UTF-8 与码点互转：单点解码（断行判定与码点分类）与整串互转。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heditor {
/**
 * 把一个码点按 UTF-8 追加到 out。非法码点（代理区、超过 U+10FFFF）写成 U+FFFD ——
 * 与 decodeUtf8 的容错口径对称：坏数据读进来是替换字符，写出去也不产出非法序列。
 */
inline void appendUtf8(std::string* out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

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

/**
 * 整串 UTF-8 转码点，非法序列按 decodeUtf8 的口径变成替换字符。
 * @param s UTF-8 文本
 * @returns 码点序列
 */
std::vector<uint32_t> utf8ToCodepoints(const std::string& s);

/**
 * 码点数组转 UTF-8。
 * @param cps 码点
 * @param n 码点个数
 * @returns UTF-8 文本
 */
std::string codepointsToUtf8(const uint32_t* cps, size_t n);

/**
 * 码点序列转 UTF-8。
 * @param cps 码点
 * @returns UTF-8 文本
 */
std::string codepointsToUtf8(const std::vector<uint32_t>& cps);

}  // namespace heditor
