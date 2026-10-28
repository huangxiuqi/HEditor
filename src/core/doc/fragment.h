/**
 * 剪贴板片段：从修订里取出一段内容，变成可移植的值，再插回去。
 *
 * 与 Fragment（扁平的内容片段，只装文本与段落标记）分工不同：这里装的是结构，
 * 用 NodeSpec 递归表达，于是控件、表格、图片、公式都能原样复制 —— 剪贴板要的是
 * "粘回来一模一样"，而不只是文字。
 *
 * 三件事各自独立：captureRange 定"取哪一段、取成什么形状"；writeFragment / readFragment
 * 是这一段的落盘格式（跨实例、跨标签页靠它）；插回去由命令层按 InsertNode 步骤做，
 * 本模块不碰文档。
 *
 * 取内容的粒度规则：区间端点落在容器（控件、表格、行、格、链接）内部时，整块一起复制，
 * 并把取用区间扩到那块的两端。理由与删除那条路同源 —— 半个控件不是内容，粘回去也没法用；
 * 而"扩到整块"是唯一不会产出半截结构的选择。文本节点例外，它按码点截断，样式随节点带走。
 *
 * 表格比控件更硬：行与格离开表格不是可粘贴的内容，"半张表"也没有形状可表达，所以只碰到
 * 表格的一部分时复制的是整张表。要支持"复制几格"得先有"按格裁剪一张表"的构造，那是另一步。
 *
 * 节与文档是外壳，不复制成节点：它们只表达"在哪条流里"，复制它们等于复制"这份文档的位置"。
 * 碰到就下沉一层，把内容提上来。
 */
#pragma once

#include <string>
#include <vector>

#include "core/doc/document.h"
#include "core/doc/flow_types.h"
#include "core/doc/serialize.h"
#include "core/doc/subtree.h"

namespace heditor::doc {
/**
 * 取内容的结果。失败时 nodes 为空、error 写明原因（没有选中内容、区间越界、含节）。
 */
struct ClipCapture {
    /**
     * 取到的节点，按流顺序。第一个与最后一个可能是截断过的文本节点。
     */
    std::vector<NodeSpec> nodes;

    /**
     * 实际取用的流区间。可能比入参宽 —— 端点落在容器内部时向外扩到了整块。
     */
    FlowPos lo = 0;

    /**
     * 实际取用区间的终点（不含）。
     */
    FlowPos hi = 0;

    /**
     * 这一段的纯文本投影：段落标记折成换行，表格格之间是制表符。
     * 给系统剪贴板的 text/plain 用，也给"粘贴纯文本"这条退路用。
     */
    std::string text;

    /**
     * 取到了没有。
     */
    bool ok = false;

    /**
     * 失败原因。
     */
    std::string error;
};

/**
 * 取流区间 [lo, hi) 里的内容。
 * @param doc 修订
 * @param vis 这一对位置按哪种可见性算（与投影同一口径）
 * @param lo 区间起点
 * @param hi 区间终点（不含）
 * @return 取到的内容；空区间或越界时失败并带原因
 */
ClipCapture captureRange(const Doc& doc, FlowVisibility vis, FlowPos lo, FlowPos hi);

/**
 * 写片段的结果。
 */
struct ClipWriteResult {
    /**
     * 写成功了没有。
     */
    bool ok = false;

    /**
     * JSON 文本（无空白）。
     */
    std::string text;

    /**
     * HTML 文本：给系统剪贴板的 text/html。
     *
     * 它不是段内往返的通道（那是上面的 JSON），只为"粘到别的应用里还带着格式"而存在 ——
     *   只写纯文本的话，粘进 Word 或浏览器就是一段裸字。
     */
    std::string html;

    /**
     * 失败原因（某个字符串不是合法 UTF-8、或某个节点种类装不进 NodeSpec）。
     */
    std::string error;
};

/**
 * 把片段写成 JSON 文本。
 *
 * 格式自带字符串表与字体表，与文档落盘同一套记录级编码（record_json）：换一个进程、
 * 换一整套字体装载顺序，读回来仍然是同一段内容。没有字体表的话，"第 2 号字体"在另一个
 * 实例里就是另一张脸 —— 而两边看起来都"成功了"。
 * @param nodes 片段
 * @param opt 字体身份（与文档落盘同一份回调）
 * @return JSON 文本
 */
ClipWriteResult writeFragment(const std::vector<NodeSpec>& nodes, const SerializeOptions& opt = {});

/**
 * 读片段的结果。
 */
struct ClipReadResult {
    /**
     * 读成功了没有。
     */
    bool ok = false;

    /**
     * 读出来的片段。
     */
    std::vector<NodeSpec> nodes;

    /**
     * 读不下去的那一处。
     */
    std::string error;
};

/**
 * 从 JSON 文本读片段。读到坏数据时 nodes 为空 —— 不交出一段半截的内容。
 * @param text JSON 文本
 * @param opt 字体身份换成本机下标
 * @return 读出来的片段
 */
ClipReadResult readFragment(const std::string& text, const DeserializeOptions& opt = {});

/**
 * 系统剪贴板上的那一份载荷：{"f": 片段, "t": 纯文本, "h": HTML}，失败时是 {"e": 原因}。
 *
 * 为什么给一层信封：一次复制要同时交出两样东西（自家格式与纯文本投影），而 DOM 事件里
 * 拿数据是同步的 —— 两趟往返在 Worker 传输上会把"用户按 Ctrl+C"拖成两次异步等待，
 * 而剪贴板写入必须当场发生。信封只有一个键位约定，读的一方只认 f / t / e。
 * @param fragment 片段 JSON 文本（空表示失败）
 * @param text 纯文本投影
 * @param error 失败原因（非空时信封里只有这一项）
 * @param html 带格式的 HTML（别的应用读的那一份，可为空）
 * @return 载荷 JSON 文本
 */
std::string clipboardPayload(const std::string& fragment, const std::string& text,
                             const std::string& error, const std::string& html);

}  // namespace heditor::doc
