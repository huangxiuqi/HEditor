/**
 * 图片资源表：宿主解码之后交上来的像素，按资源 id 存放。
 *
 * 字节不进文档（见 docs/文档模型设计.md 第十一节）：文档只存 id，换台机器打开时由宿主重新
 * 注册。注册是显式的，引擎不去找文件、也不猜路径 —— 没有像素时排版照常按尺寸给出盒子，
 * 画面上是占位框而不是静默消失。
 *
 * 只有一份表、一份所有权：像素在注册时拷进来，按资源 id 整体替换。重复注册同一个 id 表示
 * "这张图换了"，版本号随之上调，页位图靠它判新旧 —— 文档内容一个字都没变，页指纹看不出来。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace heditor {
/**
 * 一张图片的像素。
 *
 * rgba 是预乘 alpha 的 RGBA、行优先，长度恒为 width * height * 4。预乘在注册时做一次：
 * 光栅每次绘制都要用它，逐次换算等于把同一笔账重复付。
 */
struct ImageResource {
    /**
     * 资源 id（与文档里 ImageSpec::resource_id 对应）。
     */
    std::string id;

    /**
     * 像素宽。
     */
    int width = 0;

    /**
     * 像素高。
     */
    int height = 0;

    /**
     * 预乘 alpha 的像素。
     */
    std::vector<uint8_t> rgba;

    /**
     * 第几次注册。同一个 id 换图时递增，后端缓存按它判"已解码的那一份还作不作数"。
     */
    uint32_t version = 0;
};

/**
 * 资源身份 → 光栅后端的缓存键。
 *
 * 后端按 64 位键索引已解码的图（字符串做键要多一次比较与一次分配），而键必须与资源一一对应：
 * 换图时同一个键换版本，删图时按键丢掉缓存。0 留给"不缓存"，所以哈希撞上 0 时挪到 1。
 * @param id 资源 id
 * @return 缓存键（非 0）
 */
uint64_t imageResourceKey(const std::string& id);

/**
 * 资源表。容量有上限，超了拒绝注册而不是静默淘汰 —— 淘汰会让某一页突然变回占位框，
 * 而那页的像素已经作废，画面上看起来是"随机某张图不见了"。
 */
class ImageStore {
public:

    /**
     * 全表像素字节上限。一张 4000 x 3000 的图是 48 MB，这个额度够放几张。
     */
    static constexpr size_t kMaxBytes = 256u * 1024u * 1024u;

    /**
     * 注册一张图。已存在同 id 时整体替换。
     * @param id 资源 id（空串拒绝）
     * @param width 像素宽（必须为正）
     * @param height 像素高（必须为正）
     * @param rgba 像素（未预乘的 RGBA，行优先）
     * @param bytes 字节数（必须等于 width * height * 4）
     * @param error 写回失败原因
     * @return true = 收下了
     */
    bool put(const std::string& id, int width, int height, const uint8_t* rgba, size_t bytes,
             std::string* error);

    /**
     * 删掉一张图。
     * @param id 资源 id
     * @return true = 真的删掉了一张
     */
    bool erase(const std::string& id);

    /**
     * 清空整表。
     */
    void clear();

    /**
     * 查一张图。
     * @param id 资源 id
     * @return 资源；没有为 nullptr
     */
    const ImageResource* find(const std::string& id) const;

    /**
     * 张数。
     * @return 张数
     */
    int count() const { return static_cast<int>(items_.size()); }

    /**
     * 像素总字节数。
     * @return 字节数
     */
    size_t bytes() const { return bytes_; }

    /**
     * 变更计数：任何增删改都加一。
     * 它要混进光栅签名 —— 换了图片像素时文档与页指纹都不变，只有它能让页位图作废。
     * @return 变更计数
     */
    uint64_t generation() const { return generation_; }

private:

    /**
     * 按资源 id 索引的像素。
     */
    std::map<std::string, ImageResource> items_;

    /**
     * 当前像素总字节数。
     */
    size_t bytes_ = 0;

    /**
     * 变更计数（初值 1：0 留给"没注册过"）。
     */
    uint64_t generation_ = 1;

    /**
     * 下一次注册给的版本号。
     */
    uint32_t next_version_ = 1;
};

}  // namespace heditor
