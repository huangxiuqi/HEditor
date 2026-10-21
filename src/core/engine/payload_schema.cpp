#include "core/engine/payload_schema.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace pk {
namespace {

/**
 * 跳过空白。
 */
void skipWs(std::string_view s, size_t* i) {
    while (*i < s.size() && (s[*i] == ' ' || s[*i] == '\n' || s[*i] == '\r' || s[*i] == '\t')) {
        ++*i;
    }
}

/**
 * 读一个 JSON 字符串的内容（s[*i] 必须指向开引号）。不处理转义 —— 键名里没有。
 */
std::string_view readKey(std::string_view s, size_t* i) {
    if (*i >= s.size() || s[*i] != '"') return {};
    const size_t begin = ++*i;
    while (*i < s.size() && s[*i] != '"') {
        // 键名里出现转义说明它不是这里写出来的（键只有字母数字），按不匹配处理。
        if (s[*i] == '\\') return {};
        ++*i;
    }
    const std::string_view key = s.substr(begin, *i - begin);
    if (*i < s.size()) ++*i;  // 吃掉收尾引号
    return key;
}

/**
 * 跳过一个值：字符串要处理转义，对象与数组按层数配对。
 */
void skipValue(std::string_view s, size_t* i) {
    skipWs(s, i);
    if (*i >= s.size()) return;
    const char c = s[*i];
    if (c == '"') {
        ++*i;
        while (*i < s.size()) {
            if (s[*i] == '\\') {
                *i += 2;
                continue;
            }
            if (s[*i] == '"') {
                ++*i;
                return;
            }
            ++*i;
        }
        return;
    }
    if (c == '{' || c == '[') {
        int depth = 0;
        while (*i < s.size()) {
            const char ch = s[*i];
            if (ch == '"') {
                ++*i;
                while (*i < s.size()) {
                    if (s[*i] == '\\') {
                        *i += 2;
                        continue;
                    }
                    if (s[*i] == '"') {
                        ++*i;
                        break;
                    }
                    ++*i;
                }
                continue;
            }
            if (ch == '{' || ch == '[') ++depth;
            if (ch == '}' || ch == ']') {
                --depth;
                if (depth == 0) {
                    ++*i;
                    return;
                }
            }
            ++*i;
        }
        return;
    }
    // 数字 / true / false / null：吃到分隔符为止
    while (*i < s.size() && s[*i] != ',' && s[*i] != '}' && s[*i] != ']') ++*i;
}

/**
 * 报错出口。wasm 一侧是 -fno-exceptions（core 的编译选项），throw 编不过 —— 那边只能 abort。
 * 两条路都先把消息写出去：abort 之后再想打印就没机会了。
 */
[[noreturn]] void failText(std::string msg) {
    std::fprintf(stderr, "%s\n", msg.c_str());
#if defined(__cpp_exceptions)
    throw std::runtime_error(msg);
#else
    std::abort();
#endif
}

/**
 * 抛。消息要够定位：哪份载荷、第几个键、期望什么、实际什么、下一步做什么。
 * 这类错误不是用户输入导致的，别写成"请检查输入"。
 */
[[noreturn]] void fail(const PayloadSpec& spec, int index, std::string_view want,
                       std::string_view got) {
    std::string msg = "载荷 ";
    msg += spec.name;
    msg += " 的键与契约不符（第 " + std::to_string(index + 1) + " 个）：期望 \"";
    msg += want;
    msg += "\"，实际 \"";
    msg += got;
    msg += "\"。改过写出代码就重跑 python scripts/gen_payloads.py，并检查 JS 侧的读取点。";
    failText(msg);
}

}  // namespace

std::string checked(const PayloadSpec& spec, std::string json) {
    const std::string_view s = json;
    size_t i = 0;
    skipWs(s, &i);
    // hitTest 没命中时返回 "null" —— 那是"没有结果"，不是形状错误。
    if (s.compare(i, 4, "null") == 0) return json;
    if (i >= s.size() || s[i] != '{') {
        failText("载荷 " + std::string(spec.name) + " 不是一个 JSON 对象（契约表只描述对象载荷）");
    }
    ++i;
    int next = 0;  // 严格档：契约表里下一项的下标
    while (true) {
        skipWs(s, &i);
        if (i >= s.size() || s[i] == '}') break;
        const std::string_view key = readKey(s, &i);
        if (key.empty()) {
            failText("载荷 " + std::string(spec.name) + " 里读不出键名（不是我们写出来的 JSON？）");
        }
        if (spec.partial) {
            // 多出口的载荷（提前返回，各条路键集合不同）：只查"这个键在契约里存在"。
            //   顺序与缺键都不可判 —— 那条路上的 note 在另一条路上根本不出现。
            bool found = false;
            for (int k = 0; k < spec.count; ++k) {
                if (spec.keys[k].name == key) {
                    found = true;
                    break;
                }
            }
            if (!found) fail(spec, next, "（契约里没有这个键）", key);
        } else {
            // 在契约表里找它：允许跳过 optional 的项，但不能跳过必需的项。
            while (next < spec.count && spec.keys[next].name != key) {
                if (!spec.keys[next].optional) fail(spec, next, spec.keys[next].name, key);
                ++next;
            }
            if (next >= spec.count) fail(spec, next, "（没有更多键）", key);
            ++next;
        }
        skipWs(s, &i);
        if (i < s.size() && s[i] == ':') ++i;
        skipValue(s, &i);
        skipWs(s, &i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
        }
        break;
    }
    if (spec.partial) return json;
    // 剩下的必须是可选的：非可选 = 漏写了。
    for (int k = next; k < spec.count; ++k) {
        if (!spec.keys[k].optional) fail(spec, k, spec.keys[k].name, "（缺失）");
    }
    return json;
}

}  // namespace pk
