/**
 * UTF-8 与码点互转：纯值操作。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace heditor {

/**
 * UTF-8 转码点。
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