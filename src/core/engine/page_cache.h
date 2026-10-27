/**
 * 页位图缓存与脏矩形账：一页一份像素，加"这一页哪几块像素变了"。
 *
 * 为什么把它从引擎里分出来：这两样东西的失效条件完全由"画出来是什么"决定（页指纹与
 * 光栅签名），而引擎的其余部分是"文档是什么"。混在一个类里时，新增一个光栅配置项要
 * 同时改签名、清缓存、清脏矩形三处，漏一处的症状是"改了之后屏幕不变"。
 *
 * 缓存是唯一的像素存放处：换窗口、换缩放都不作废它（判据是页指纹与光栅签名），
 * 于是滚过一页不会把已经画好的页重画一遍。脏矩形是"自上次取走以来累积"，与像素分开 ——
 * 像素可以长期留着，脏矩形只活到宿主取走那一刻。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/render/bitmap.h"
#include "core/utils/types.h"

namespace heditor {

/**
 * 一页上叠的瞬态：选区矩形（可以有多条）、光标矩形（最多一条）、图片拖拽框与把手。
 *
 * 分成三份而不是混在一个数组里：导出时选区混色在下、拖拽框居中、光标实心在上，顺序由
 * 类型定。由"谁最后被 push 进数组"定的话，只要有一处 push 顺序变了，光标就会被半透明地
 * 再混一次，而那种错只在重叠的几个像素上看得出。
 */
struct PageTransient {
    /**
     * 这一页上的选区矩形（页内像素坐标）。
     */
    std::vector<Rect> sel;

    /**
     * 这一页上的光标矩形（最多一条）。
     */
    Rect caret{};

    /**
     * 这一页上的图片拖拽框与把手（页内像素坐标）。
     *
     * 与选区同一层：呈现时叠、导出时不叠 —— 打印出来带一圈把手是错的。
     */
    std::vector<Rect> image_ui;
};

/**
 * 一页的光栅结果：页内坐标（原点 = 页左上角），不含页间隙。
 */
struct PageSlot {
    /**
     * 页像素（页内坐标，不含页间隙）。
     */
    Bitmap px;

    /**
     * 光栅时那一版页指纹。
     *
     * 哨兵值也算一版（见 kFpDirtyBit）：与当前页指纹相等就说明这份像素还作数。
     */
    uint64_t fp = 0;

    /**
     * 这一份像素是在哪一版"光栅配置"下画的。
     *
     * 页指纹只哈希字形等内容，看不见页面底色、边框、缩放、页度量这些"画出来是什么"的
     * 输入。把它们的哈希记在这里，比一次就是不新鲜 —— 新增一个光栅配置项时只要加进
     * 光栅签名，所有页自动失效，不必再维护一张"记得清缓存"的清单。
     */
    uint64_t raster_sig = 0;

    /**
     * 上一次导出时叠在这一页上的瞬态。
     *
     * 它只是"上一次记过哪几块"的账，用来算新旧差集 —— 像素本身不在页位图里，
     * 所以没有"擦"这一步：下一次导出从干净的页位图重来。
     */
    PageTransient transient;

    /**
     * 这一格装着页没有（没有就不用比对指纹）。
     */
    bool valid = false;
};

/**
 * 页缓存本体：页位图 + 脏矩形 + 光栅签名。
 */
class PageCache {
public:

    /**
     * 光栅签名换了一版就作废全部脏矩形（旧矩形描述的是旧像素）。
     *
     * @param sig 按当前配置算出来的签名
     * @returns 签名变了没有
     */
    bool setSignature(uint64_t sig);

    /**
     * 当前光栅签名。
     * @returns 签名
     */
    uint64_t signature() const { return sig_; }

    /**
     * 缓存里的这一页，没有就返回空。
     * @param page 页号
     * @returns 这一格，可能为空
     */
    PageSlot* find(int page);

    /**
     * 同上，只读。
     * @param page 页号
     * @returns 这一格，可能为空
     */
    const PageSlot* find(int page) const;

    /**
     * 取这一页的格子，没有就建一个。
     * @param page 页号
     * @returns 这一格
     */
    PageSlot& at(int page);

    /**
     * 缓存里有没有这一页。
     * @param page 页号
     * @returns 有没有
     */
    bool contains(int page) const;

    /**
     * 缓存的页数。
     * @returns 页数
     */
    std::size_t size() const { return slots_.size(); }

    /**
     * 清掉全部页位图与脏矩形。
     */
    void clear();

    /**
     * 只清脏矩形，页位图留着。
     */
    void clearDirty();

    /**
     * 丢掉这一页（页位图与它的脏矩形一起）。
     * @param page 页号
     */
    void erase(int page);

    /**
     * 按"离窗口最远"淘汰，窗口内的页一页都不丢。
     *
     * 不在这里做"整篇窗口就整份清掉"那种省内存的优化：页位图是导出像素的唯一来源，
     * 丢了这一段，窗口里就有几页拿不出像素。
     *
     * @param first_page 窗口首页
     * @param count 窗口页数
     */
    void trim(int first_page, int count);

    /**
     * 这一页的像素还作数吗：有格子、有指纹、尺寸对得上、光栅签名与指纹都等于当前版本。
     *
     * @param page 页号
     * @param fp 这一页当前的页指纹
     * @param px_w 页宽（设备像素）
     * @param px_h 页高（设备像素）
     * @returns 能不能直接上屏
     */
    bool fresh(int page, uint64_t fp, int px_w, int px_h) const;

    /**
     * 记一块"这一页的像素变了"（累积到被取走为止）。
     *
     * 合并规则：相交或相接的并成一个（光标擦旧画新就是这种），相距远的不并 ——
     * 并了会把中间一大片没变的像素也传上去。条数超过 16 时退回一个并集：
     * 一页几十个表单标记时逐条传反而更贵，而这一条只是"上界"，宁可多传不可少传。
     *
     * @param page 页号
     * @param r 页内像素矩形
     */
    void noteDirty(int page, const Rect& r);

    /**
     * 取走这一页累积的脏矩形（取完即清）。
     * @param page 页号
     * @returns 脏矩形，可能为空
     */
    std::vector<Rect> takeDirty(int page);

    /**
     * 这一页有没有攒着的脏矩形（读后不清）。
     * @param page 页号
     * @returns 有没有
     */
    bool hasDirty(int page) const;

    /**
     * 有脏矩形的页号（升序）。
     * @returns 页号表
     */
    std::vector<int> dirtyPages() const;

    /**
     * 全部页格，供呈现时遍历（窗口外的页要清瞬态账）。
     * @returns 页号到页格的表
     */
    std::unordered_map<int, PageSlot>& slots() { return slots_; }

    /**
     * 同上，只读。
     * @returns 页号到页格的表
     */
    const std::unordered_map<int, PageSlot>& slots() const { return slots_; }

private:

    /**
     * 页位图，按页号索引。
     */
    std::unordered_map<int, PageSlot> slots_;

    /**
     * 每页待上屏的脏矩形（页内像素坐标），自上次取走以来累积。
     */
    std::unordered_map<int, std::vector<Rect>> dirty_;

    /**
     * 当前光栅配置的签名（见 PageSlot::raster_sig）。
     */
    uint64_t sig_ = 0;
};

}  // namespace heditor
