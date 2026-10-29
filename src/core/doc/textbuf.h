/**
 * 文档编辑原语与文本位置。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/text_pos.h"
#include "core/utils/utf8.h"

namespace heditor {
namespace textbuf {
/**
 * 在 offset 处插入。offset 会被夹到 [0, buf.size()]。
 */
void insertAt(std::vector<uint32_t>& buf, size_t offset, const uint32_t* cps, size_t n);
void insertAt(std::vector<uint32_t>& buf, size_t offset, const std::vector<uint32_t>& cps);

/**
 * 删除 [begin, end)。区间会被夹到合法范围；begin >= end 时什么都不做。
 * 返回实际删掉的码点数。
 */
size_t eraseRange(std::vector<uint32_t>& buf, size_t begin, size_t end);

}  // namespace textbuf

/**
 * 簇吸附
 * cluster_starts 必须是升序的簇起点码点下标。约定：最后一项是段长
 * （作为行末哨兵），这样"最后一个簇"也能被 floor 到。
 * clusterFloor：<= offset 的最大簇起点。光标停在这里；退格从这个位置往回删一个簇。
 * clusterCeil：> offset 的最小簇起点。删除前向时用它找"下一个可删边界"。
 * @param cluster_starts 升序的簇起点（末尾带段长哨兵）
 * @param offset 当前偏移
 * @return 不大于 offset 的最大簇起点；没有则为 offset
 */
size_t clusterFloor(const std::vector<uint32_t>& cluster_starts, size_t offset);

/**
 * 大于 offset 的最小簇起点（下一个可删边界）。
 * @param cluster_starts 升序的簇起点（末尾带段长哨兵）
 * @param offset 当前偏移
 * @return 下一个簇起点；没有则为 offset
 */
size_t clusterCeil(const std::vector<uint32_t>& cluster_starts, size_t offset);

}  // namespace heditor
