#pragma once

#include <string>
#include <string_view>

#include "payload_schema.gen.h"

namespace pk {
/**
 * 校验一份载荷的顶层键，然后把原字符串原样返回。
 *
 * 写出代码是手拼的：o << "\"relaidLines\":" << e.relaid_lines。右边那半个由编译器守着，
 *   左边那半个（键名字符串）谁都不守 —— 改了键名而 JS 没改，结果是 undefined，而
 *   undefined 不会让任何断言变红，只会让报告少一列。所以每个写出函数在 return 之前
 *   过一遍这张表，与 docs/api/payloads.json（同一份代码抽出来的）逐项比。
 * 单出口的载荷按严格有序比（键名、顺序、个数），optional 的允许跳过；多出口的（partial，
 *   有提前返回）各条路键集合不同，只查每个键在契约里存在。整个载荷是 null 时放行
 *   （hitTest 没命中）。不符就报错：这是 bug，不是用户输入。
 * 成本：一遍扫描、零分配，1 KB 约 1 µs，常开。
 */
std::string checked(const PayloadSpec& spec, std::string json);

}  // namespace pk
