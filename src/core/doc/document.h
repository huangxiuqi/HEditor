/**
 * 文档模型与编辑原语。
 * 偏移单位是码点（UCS-4），不是字节也不是 UTF-16。
 * "10⁹" 里的 ⁹ 占 3 个字节：偏移 3（\(10 与 ⁹ 之间）是合法的，偏移 4 就落在
 * ⁹ 的内部 —— 后者是一个"非法插入点"。用字节当单位就必须处处做
 * 前向/后向解码，否则光标能停在半个字符中间。而码点是整形与布局的公共尺子：
 * HarfBuzz 经 hb_buffer_add_utf32 输出的 cluster 直接是码点下标，文档偏移与 cluster
 * 之间不需要映射表，也就少一处会失同步的状态。
 *
 * 但码点仍不是编辑的最小单位：退格要删掉一个用户感知字符（grapheme cluster），例如
 * e 加组合重音 U+0301 是两个码点，故编辑偏移必须先吸附到簇边界（簇边界由布局层给出，
 * 见 LayoutResult::para_cluster_starts）。三把尺子逐层包含：字节 ⊇ 码点 ⊇ 簇。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace heditor {
/**
 * 位置：段 + 段内码点偏移
 * 档 1 的段落模型是扁平的：一个 BlockSpec = 一段文本，没有块树、没有样式树。
 * 所以位置用 (段下标, 段内偏移) 表示，段内偏移单位是码点。
 */
struct TextPos {
    /**
     * 段号（编辑态）。
     */
    int para = 0;

    /**
     * 段内码点偏移，范围 [0, 段长]。
     */
    size_t offset = 0;

    /**
     * 两个位置相同（段号与偏移都相等）。
     */
    bool operator==(const TextPos& o) const { return para == o.para && offset == o.offset; }

    /**
     * 不等 = 相等取反。
     */
    bool operator!=(const TextPos& o) const { return !(*this == o); }

    /**
     * 先比段号、再比偏移（文档顺序）。
     */
    bool operator<(const TextPos& o) const {
        return para != o.para ? para < o.para : offset < o.offset;
    }
};

/**
 * UTF-8 ↔ 码点
 * @param s UTF-8 文本
 * @return 码点序列
 */
std::vector<uint32_t> utf8ToCodepoints(const std::string& s);

/**
 * 码点数组转 UTF-8。
 * @param cps 码点
 * @param n 码点个数
 * @return UTF-8 文本
 */
std::string codepointsToUtf8(const uint32_t* cps, size_t n);

/**
 * 码点序列转 UTF-8。
 * @param cps 码点
 * @return UTF-8 文本
 */
std::string codepointsToUtf8(const std::vector<uint32_t>& cps);

/**
 * 编辑原语
 * 只作用在码点数组上，不做簇吸附（吸附是调用方的责任，因为只有布局层
 * 知道簇边界）。这样每个函数都是纯数据操作，可以被断言直接验证。
 */
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
