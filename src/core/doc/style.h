/**
 * 样式表与三层解析（见 docs/文档模型设计.md 第六节）。
 *
 * 三层是文档默认值、样式链、内联覆盖：解析按这个顺序逐属性合并，后面覆盖前面。解析结果是
 * 运行期样式，只有它进编译与排版 —— 引擎不认识样式表，样式系统的复杂度因此不会渗进排版层。
 *
 * 样式链沿 based_on 从叶往根走，合并时反过来从根往叶叠（叶覆盖根）。链上重复出现同一个名字
 * 说明成环，那时停止下钻并记一条诊断：成环是文档损坏，但降级成"能算多少算多少"再报出来，
 * 比转圈或者抛出去更有用（导入外部文档时尤其如此）。
 *
 * 元素上挂的是"样式引用 + 与样式不同的那些属性"（ParaFormatPatch 与 CharFormat 都是三态），
 * 所以"没写"与"写成了默认值"能分开，继承才算得对。
 *
 * 解析结果必须缓存：内联覆盖让结果依赖上下文，不能每帧重走一遍继承链。缓存键是元素 ID 加
 * 样式表版本号 —— 样式表一改，版本号变，整个缓存作废（改一次样式影响的是全篇引用它的元素，
 * 那些元素散在各处，逐个失效不如整体失效）。
 */
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/doc/char_format.h"
#include "core/doc/node.h"
#include "core/doc/para_format.h"

namespace heditor {

/**
 * 样式种类。表格样式要等 1.7 有表格之后再进来。
 */
enum class StyleKind : uint8_t { Paragraph = 0, Character = 1 };

const char* styleKindName(StyleKind kind);

/**
 * 一条样式。段落样式同时带段落级属性与字符级属性（段落标记自己的字符格式），
 * 字符样式只有字符级属性 —— 后者对段落样式无意义，读它没有定义。
 */
struct StyleDef {
    std::string name;
    std::string based_on;  // 空表示直接继承文档默认值
    StyleKind kind = StyleKind::Paragraph;
    ParaFormatPatch para;
    CharFormat run;

    bool operator==(const StyleDef& o) const = default;
};

/**
 * 样式表。存在文档上（Document 持有它），元素只存名字。
 *
 * 改动一律走 setDocDefaults / add / remove，它们会自己推进版本号；没有拿到内部引用的可变
 * 访问器 —— 版本号是缓存失效的唯一依据，漏推一次的表现是"改了样式但屏幕上没变"，很难查。
 */
class StyleSheet {
public:
    const ParaFormatPatch& docDefaultsPara() const { return doc_defaults_para_; }
    const CharFormat& docDefaultsRun() const { return doc_defaults_run_; }

    void setDocDefaults(ParaFormatPatch para, CharFormat run);

    /**
     * 加一条样式，同名同种时替换。返回是否替换了已有的一条。
     */
    bool add(const StyleDef& def);

    bool remove(StyleKind kind, const std::string& name);

    const StyleDef* find(StyleKind kind, const std::string& name) const;

    /**
     * 全部样式名，按加入顺序。顺序稳定，报告与判据才能逐项比对。
     */
    std::vector<std::string> names(StyleKind kind) const;

    int size() const { return static_cast<int>(paragraphs_.size() + characters_.size()); }

    /**
     * 版本号，任何改动都自增。缓存按它整体失效（第六节）。
     */
    int version() const { return version_; }

private:
    ParaFormatPatch doc_defaults_para_;
    CharFormat doc_defaults_run_;

    std::vector<StyleDef> paragraphs_;
    std::vector<StyleDef> characters_;
    int version_ = 1;  // 从 1 起：0 留给"还没读过的缓存"
};

/**
 * 把一个段落标记解析成完整的段落格式。
 *
 * 顺序是文档默认值 → style_name 的样式链 → inline_patch。style_name 为空或找不到时只用
 * 默认值并记一条诊断（外部文档引用了一个本机没有的样式名，降级渲染比拒绝打开有用）。
 */
ParaFormat resolveParagraphFormat(const StyleSheet& sheet, const std::string& style_name,
                                  const ParaFormatPatch& inline_patch,
                                  std::vector<std::string>* diagnostics);

/**
 * 把一段文本解析成完整的字符格式。
 *
 * 顺序是文档默认值（run 部分）→ 所在段落的段落样式（run 部分）→ char_style 的字符样式链
 * → 元素上的内联 marks。段落样式也带字符级属性 —— 只算字符样式会漏掉"这个样式规定了字体"。
 */
CharFormat resolveRunFormat(const StyleSheet& sheet, const std::string& para_style,
                            const std::string& char_style, const CharFormat& inline_marks,
                            std::vector<std::string>* diagnostics);

/**
 * 解析结果的缓存。键是元素 ID 加样式表版本号。
 *
 * 命中与否有计数：判据要证明的是"第二次查不重算"，只比结果相等证明不了这一点（结果本来就
 * 相等，重算也相等）。
 */
class StyleCache {
public:
    explicit StyleCache(const StyleSheet& sheet);

    /**
     * 一个段落标记的段落格式。第二次查同一个标记且样式表没改过时直接命中。
     */
    const ParaFormat& paragraph(const NodeTree& tree, NodeId mark_id);

    /**
     * 一段文本的字符格式。para_style 由它所在的那一段给出：同一段里的 run 共用一个段落样式，
     * 换了段落样式就是另一份结果，所以缓存记下上一次用的段落样式名，不同就当没命中。
     */
    const CharFormat& run(const NodeTree& tree, NodeId run_id, const std::string& para_style);

    /**
     * 某个元素自己改了（内联覆盖变了、文本被重写）时只失效它一个。
     */
    void invalidate(NodeId id);

    void clear();

    int computations() const { return computations_; }
    int hits() const { return hits_; }
    int entries() const { return static_cast<int>(paragraphs_.size() + runs_.size()); }

    /**
     * 解析过程中攒下的诊断（成环、未知样式名），按原文去重。
     */
    const std::vector<std::string>& diagnostics() const { return diagnostics_; }

private:
    /**
     * 样式表版本变了就整体作废。每次查询前调一次。
     */
    void syncVersion();

    const StyleSheet& sheet_;
    int version_ = 0;
    std::unordered_map<NodeId, ParaFormat> paragraphs_;
    std::unordered_map<NodeId, std::pair<std::string, CharFormat>> runs_;
    std::vector<std::string> diagnostics_;
    int computations_ = 0;
    int hits_ = 0;
};

}  // namespace heditor
