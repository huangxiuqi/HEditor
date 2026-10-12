/**
 * 节点身份。
 *
 * 单独成一个头文件，是因为节点基类与节的记录都要用它：节的记录要引用页眉页脚的内容片段根，
 * 而节点基类要包含节的记录，两者互相包含会形成循环。把身份类型放到两边都能包含的最小位置，
 * 依赖方向就只剩一条。
 */
#pragma once

#include <cstdint>

namespace heditor {

/**
 * 64 位、文档内唯一、不回收（删掉的 ID 不再复用，避免旧引用指向新节点），
 * 所以序列化时必须落盘。0 保留给无效。
 */
using NodeId = uint64_t;
inline constexpr NodeId kInvalidNodeId = 0;

}  // namespace heditor
