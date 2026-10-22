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

/**
 * 样式种类的名字。
 * @param kind 种类
 * @return 名字
 */
const char* styleKindName(StyleKind kind);

/**
 * 一条样式。段落样式同时带段落级属性与字符级属性（段落标记自己的字符格式），
 * 字符样式只有字符级属性 —— 后者对段落样式无意义，读它没有定义。
 */
struct StyleDef {
    /**
     * 样式名（元素上的样式引用就是它）。
     */
    std::string name;

    /**
     * 继承自哪一条；空表示直接继承文档默认值。
     */
    std::string based_on;

    /**
     * 段落样式还是字符样式。
     */
    StyleKind kind = StyleKind::Paragraph;

    /**
     * 段落级属性（字符样式上无意义）。
     */
    ParaFormatPatch para;

    /**
     * 字符级属性。段落样式上它管的是段落标记自己的格式。
     */
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

    /**
     * 文档默认值的字符级部分。
     * @return 默认字符格式
     */
    const CharFormat& docDefaultsRun() const { return doc_defaults_run_; }

    /**
     * 换文档默认值（会推进版本号）。
     * @param para 默认段落格式
     * @param run 默认字符格式
     */
    void setDocDefaults(ParaFormatPatch para, CharFormat run);

    /**
     * 加一条样式，同名同种时替换。返回是否替换了已有的一条。
     * @param def 样式定义
     * @return true = 替换了已有的一条（否则是新增）
     */
    bool add(const StyleDef& def);

    /**
     * 删一条样式（会推进版本号）。
     * @param kind 段落样式还是字符样式
     * @param name 样式名
     * @return true = 删掉了；没有这一条时 false
     */
    bool remove(StyleKind kind, const std::string& name);

    /**
     * 按种类与名字查样式。
     * @param kind 段落样式还是字符样式
     * @param name 样式名
     * @return 样式；没找到为 nullptr
     */
    const StyleDef* find(StyleKind kind, const std::string& name) const;

    /**
     * 全部样式名，按加入顺序。顺序稳定，报告与判据才能逐项比对。
     * @param kind 段落样式还是字符样式
     * @return 样式名，按加入顺序
     */
    std::vector<std::string> names(StyleKind kind) const;

    /**
     * 样式总数（段落加字符）。
     * @return 条数
     */
    int size() const { return static_cast<int>(paragraphs_.size() + characters_.size()); }

    /**
     * 版本号，任何改动都自增。缓存按它整体失效（第六节）。
     * @return 版本号（任何改动都自增）
     */
    int version() const { return version_; }

private:
    ParaFormatPatch doc_defaults_para_;

    /**
     * 文档默认值的字符级部分。
     */
    CharFormat doc_defaults_run_;

    /**
     * 段落样式，按加入顺序（names 的顺序靠它稳定）。
     */
    std::vector<StyleDef> paragraphs_;

    /**
     * 字符样式，按加入顺序。
     */
    std::vector<StyleDef> characters_;

    /**
     * 版本号。从 1 起，0 留给"还没读过的缓存"。
     */
    int version_ = 1;
};

/**
 * 把一个段落标记解析成完整的段落格式。
 *
 * 顺序是文档默认值 → style_name 的样式链 → inline_patch。style_name 为空或找不到时只用
 * 默认值并记一条诊断（外部文档引用了一个本机没有的样式名，降级渲染比拒绝打开有用）。
 * @param sheet 样式表
 * @param style_name 段落样式引用
 * @param inline_patch 内联覆盖
 * @param diagnostics 记诊断（可为 nullptr）
 * @return 解析出来的完整段落格式
 */
ParaFormat resolveParagraphFormat(const StyleSheet& sheet, const std::string& style_name,
                                  const ParaFormatPatch& inline_patch,
                                  std::vector<std::string>* diagnostics);

/**
 * 把一段文本解析成完整的字符格式。
 *
 * 顺序是文档默认值（run 部分）→ 所在段落的段落样式（run 部分）→ char_style 的字符样式链
 * → 元素上的内联 marks。段落样式也带字符级属性 —— 只算字符样式会漏掉"这个样式规定了字体"。
 * @param sheet 样式表
 * @param para_style 段落样式引用
 * @param char_style 字符样式引用
 * @param inline_marks 内联标记
 * @param diagnostics 记诊断（可为 nullptr）
 * @return 解析出来的完整字符格式
 */
CharFormat resolveRunFormat(const StyleSheet& sheet, const std::string& para_style,
                            const std::string& char_style, const CharFormat& inline_marks,
                            std::vector<std::string>* diagnostics);

/**
 * 段落样式链贡献的字符格式基线：文档默认值的 run 部分 + 段落样式链的 run 部分。
 *
 * 为什么单独暴露：一段里有多个文本节点时，这条链对每个节点都一样。逐个节点重解会把
 * "未知段落样式"这类诊断按节点数重复报出来，而调用方往往已经在解析段落格式时报过一次了
 * （那条链是同一根），于是可以把 diagnostics 传 nullptr，只取值不报。
 * @param sheet 样式表
 * @param para_style 段落样式引用
 * @param diagnostics 记诊断（可为 nullptr）
 * @return 这一段所有 run 共用的字符格式基线
 */
CharFormat paragraphRunBase(const StyleSheet& sheet, const std::string& para_style,
                            std::vector<std::string>* diagnostics);

/**
 * 在调用方算好的基线上叠字符样式链与内联 marks。
 *
 * 与 resolveRunFormat 的关系：它把"段落那一半"换成外部传入的基线，于是同一段里的多个
 * 文本节点不必重解段落链。两者对同一个 (para_style, char_style, marks) 必须给出相同结果。
 * @param sheet 样式表
 * @param base 调用方算好的基线
 * @param char_style 字符样式引用
 * @param inline_marks 内联标记
 * @param diagnostics 记诊断（可为 nullptr）
 * @return 叠完字符样式与内联标记的完整字符格式
 */
CharFormat resolveRunFormatFrom(const StyleSheet& sheet, const CharFormat& base,
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
     * @param tree 文档
     * @param mark_id 段落标记的节点 id
     * @return 解析出来的段落格式（缓存的引用）
     */
    const ParaFormat& paragraph(const NodeTree& tree, NodeId mark_id);

    /**
     * 一段文本的字符格式。para_style 由它所在的那一段给出：同一段里的 run 共用一个段落样式，
     * 换了段落样式就是另一份结果，所以缓存记下上一次用的段落样式名，不同就当没命中。
     * @param tree 文档
     * @param run_id 文本节点的节点 id
     * @param para_style 它所在段的段落样式名
     * @return 解析出来的字符格式（缓存的引用）
     */
    const CharFormat& run(const NodeTree& tree, NodeId run_id, const std::string& para_style);

    /**
     * 某个元素自己改了（内联覆盖变了、文本被重写）时只失效它一个。
     * @param id 改了的那个元素
     */
    void invalidate(NodeId id);

    /**
     * 清掉全部缓存条目与诊断（计数不动）。
     */
    void clear();

    /**
     * 真正跑过解析的次数。
     * @return 次数
     */
    int computations() const { return computations_; }

    /**
     * 缓存命中的次数。
     * @return 次数
     */
    int hits() const { return hits_; }

    /**
     * 缓存里现在有多少条。
     * @return 条数
     */
    int entries() const { return static_cast<int>(paragraphs_.size() + runs_.size()); }

    /**
     * 解析过程中攒下的诊断（成环、未知样式名），按原文去重。
     * @return 诊断（按原文去重）
     */
    const std::vector<std::string>& diagnostics() const { return diagnostics_; }

private:

    /**
     * 样式表版本变了就整体作废。每次查询前调一次。
     */
    void syncVersion();

    /**
     * 样式表（不持有）。
     */
    const StyleSheet& sheet_;

    /**
     * 缓存对应的样式表版本；与样式表不符时整体作废。
     */
    int version_ = 0;

    /**
     * 段落标记的解析结果（键是标记节点 id）。
     */
    std::unordered_map<NodeId, ParaFormat> paragraphs_;

    /**
     * 文本节点的解析结果，附带解析时用的段落样式名（换了段落样式就不算命中）。
     */
    std::unordered_map<NodeId, std::pair<std::string, CharFormat>> runs_;

    /**
     * 解析过程中攒下的诊断（成环、未知样式名）。
     */
    std::vector<std::string> diagnostics_;

    /**
     * 真正跑过解析的次数。
     */
    int computations_ = 0;

    /**
     * 缓存命中的次数。
     */
    int hits_ = 0;
};

}  // namespace heditor
