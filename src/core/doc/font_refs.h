/**
 * 字体引用表：下标 → {ps, family, file}。
 *
 * 片段与文档两个写出点共用这一份。各写一遍的漂移不会报错，只会让复制出去再粘回来的
 *   内容丢掉字体指向 —— 两边键名与"空字段不写"的口径必须永远一致。
 */
#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/doc/record_json.h"
#include "core/doc/serialize.h"

namespace heditor::doc {

/**
 * 写字体引用表。
 *
 * 按序遍历下标，于是同一份内容写出来的字节序列是确定的（判据比的是字节）。
 *   三项都空的引用不写 —— 那一侧不认识这个字体，读回来退化成"下标即下标"。
 *
 * @param used 用到的字体下标
 * @param font_ref 下标 → 引用
 * @returns 表；没有任何可写条目时是空对象
 */
inline recjson::ordered_json fontRefsJson(const std::unordered_set<int>& used,
                                          const std::function<FontRef(int)>& font_ref) {
    recjson::ordered_json fonts = recjson::ordered_json::object();
    std::vector<int> sorted(used.begin(), used.end());
    std::sort(sorted.begin(), sorted.end());
    for (int index : sorted) {
        const FontRef ref = font_ref(index);
        recjson::ordered_json entry = recjson::ordered_json::object();
        if (!ref.postscript.empty()) entry["ps"] = ref.postscript;
        if (!ref.family.empty()) entry["family"] = ref.family;
        if (!ref.file.empty()) entry["file"] = ref.file;
        if (!entry.empty()) fonts[std::to_string(index)] = std::move(entry);
    }
    return fonts;
}

}  // namespace heditor::doc
