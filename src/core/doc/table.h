/**
 * 表格：派生网格与行列操作（见 docs/文档模型设计.md 第九节）。
 *
 * 模型里只有跨度，没有网格。"续格"这种格位在模型里根本不存在（HTML 式），只在派生网格里
 * 出现且指向原点格。所以网格是推导出来的，推导的结果必须满足第二节那条表格类不变量，
 * 否则就是 bug 而不是"数据本来就这样"。
 *
 * 行列操作不是新的步骤类型，而是先改跨度、再重新推导、然后交给既有步骤：
 * 插一列 = 几个格子的跨度变了 + 几行各插一个新格子；删一列 = 跨过的格子变窄（宽度降到 0 的
 * 连同内容一起删）。这样撤销、位置映射、不变量检查都自动复用一套机制，不必为表格另写一遍。
 */
#pragma once

#include <string>
#include <vector>

#include "core/doc/node.h"
#include "core/doc/steps.h"
#include "core/rev/table_spec.h"

namespace heditor {
/**
 * 派生网格里的一个格位：它属于哪个单元格，以及那个单元格的原点在哪。
 *
 * row / col 存的是原点坐标，不是当前格位坐标 —— 这正是"落在合并区域内返回原点格"
 * 这条规则的落点：查任意格位拿到的都是同一个原点。
 */
struct GridCell {
    /**
     * 占这个格位的单元格节点 id。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 原点格的行号（不是当前格位的行号）。
     */
    int row = 0;

    /**
     * 原点格的列号。
     */
    int col = 0;
};

/**
 * 派生网格。行优先，长度是 rows * cols。
 */
struct TableGrid {
    /**
     * 网格行数（= 行记录条数）。
     */
    int rows = 0;

    /**
     * 网格列数（由第一行的跨度之和定）。
     */
    int cols = 0;

    /**
     * 行优先的格位数组，长度 rows * cols。
     */
    std::vector<GridCell> slots;

    /**
     * 查一个格位。越界返回空；落在合并区域内时返回该区域的原点格（左上角）。
     * 这是唯一能让 cell(r, c) 在任何表上都可用的规则，否则调用方每次都要先自己判断
     * "这个格位是不是被别人合并了"。
     * @param row 行号
     * @param col 列号
     * @return 格位；越界为 nullptr；落在合并区域内时给原点格
     */
    const GridCell* at(int row, int col) const;

    /**
     * 两个格位是不是同一个单元格（也就是它们之间那条网格线被合并跨过）。
     * @param row_a 第一个格位所在行
     * @param col_a 第一个格位所在列
     * @param row_b 第二个格位所在行
     * @param col_b 第二个格位所在列
     * @return true = 两个格位属于同一个单元格
     */
    bool sameOrigin(int row_a, int col_a, int row_b, int col_b) const;

    /**
     * 某个单元格在它所在行里是第几格（按原点格算）。不在网格里时返回 -1。
     *
     * 行列操作要靠它定位"新格子插在哪两个格子之间" —— 插在"原列号不小于插入点的第一个
     * 格子"之前；跨过插入线的格子原列号更小，所以它留在前面吸收新列。
     * @param id 单元格节点 id
     * @return 它在所在行里是第几格；不在网格里为 -1
     */
    int originColOf(NodeId id) const;
};

/**
 * 标题行的行数：从第 0 行起连续标了 is_header 的行数。
 *
 * 它不再是存下来的字段 —— 标题行是逐行开关，"几个"只是数出来的。遇到第一个非标题行就停，
 * 所以"第 2 行标了标题而第 1 行没标"这种形状它只数到 0；那种形状由不变量报，不由它纠正。
 * 表格节点无效时返回 0。
 * @param tree 文档树
 * @param table 表格节点号
 * @return 从第 0 行起连续标了标题行的行数
 */
int headerRowCount(const NodeTree& tree, NodeId table);

/**
 * 网格推导的输入：一行里的各个格子。只有身份与跨度，没有内容。
 *
 * 模型层与修订层都要推网格，两边的节点类型不同，能共用的就是这一层 —— 网格规则（重叠、
 * 空洞、跨出表尾）只写一份，两套实现不会各自演化。
 */
struct GridCellSpan {
    /**
     * 单元格节点 id。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 纵向跨度。
     */
    int row_span = 1;

    /**
     * 横向跨度。
     */
    int col_span = 1;
};

/**
 * 由每行的跨度序列推出网格。这是网格规则的唯一实现。
 * @param rows 每行的跨度序列
 * @param out 写回网格
 * @param error 写回失败原因（重叠、空洞、跨出表尾）
 * @return true = 这些跨度能推出一张网格
 */
bool buildGridFromSpans(const std::vector<std::vector<GridCellSpan>>& rows, TableGrid* out,
                        std::string* error);

/**
 * 从跨度推导网格。失败时给出原因（重叠、空洞、合并超出表格末尾）。
 *
 * 列数由第一行自己的跨度之和定（第一行没有东西跨进来，所以它的覆盖范围就是列数），之后
 * 每一行都必须被完整覆盖且不留缝隙 —— 这就是"每行的跨度之和相等、等于网格列数"那条不变量
 * 的落点。注意那句话说的是"这一行被覆盖的格位数"，不是"这一行自己的格子的跨度之和"：纵向
 * 合并覆盖的行本来就没有那么多自己的格子（被合并掉的格位在模型里不存在），按后者判会把
 * 合法的表判成非法。
 * @param tree 文档树
 * @param table 表格节点号
 * @param out 写回网格
 * @param error 写回失败原因
 * @return true = 推得出网格
 */
bool buildGrid(const NodeTree& tree, NodeId table, TableGrid* out, std::string* error);

/**
 * 一批步骤。表格的行列操作由多条最小步骤组成，所以构造器返回一批而不是一条。
 */
struct StepBatch {
    /**
     * 这一批步骤（按顺序应用）。
     */
    std::vector<Step> steps;

    /**
     * 构造成功没有。
     */
    bool ok = false;

    /**
     * 失败原因（重叠、空洞、跨出表尾）。
     */
    std::string error;
};

/**
 * 新建表格的行列上限。它不是排版限制，而是"多打一个零"的挡板：行列相乘就是段数，
 * 200 x 50 已经是一万段，再往上排版与撤销栈会先把内存吃掉。
 */
constexpr int kMaxNewTableRows = 200;

/**
 * 新建表格的列数上限（理由同 kMaxNewTableRows）。
 */
constexpr int kMaxNewTableCols = 50;

/**
 * 造一张空表格的子树描述：rows 行、每行 cols 格、每格一枚段落标记。
 *
 * 空格也要有标记。块流与流位置都按段落标记递推，没有标记的格在流里一位都不占 ——
 * 于是"点进这一格"没有可落的位置，表格的行列操作也算不出它属于哪一段。
 *
 * 不写列宽与列定义：宽度 Auto 撑满内容区，列宽由排版按格数平分（见 layout 的 openTable），
 * 这是"刚插进来的表"该有的形状。要指定列宽请在此之后改表格记录。
 *
 * @param rows 行数，取值 [1, kMaxNewTableRows]
 * @param cols 每行的格数，取值 [1, kMaxNewTableCols]
 * @param header true = 首行标成标题行（跨页时在每页顶端重复）
 * @param out 写回子树描述
 * @return true = 参数可表达；false = 行列越界或 out 为空
 */
bool makeEmptyTableSpec(int rows, int cols, bool header, NodeSpec* out);

/**
 * 在 at_row 处插入一行（取 [0, 行数]，等于行数就是追加）。
 *
 * cells 是这一行的单元格描述，跨度之和必须等于列数（否则新行与网格对不上）。
 * 新行里的格子不允许 rowspan > 1 —— 那会与下面的行重叠；要跨行合并请在插入之后再做。
 * @param tree 文档树
 * @param table 表格节点号
 * @param at_row 插到第几行之前（等于行数就是追加）
 * @param cells 这一行的单元格
 * @return 一批步骤（失败时带原因）
 */
StepBatch makeInsertTableRow(NodeTree& tree, NodeId table, int at_row,
                             const std::vector<NodeSpec>& cells);

/**
 * 删除一行。
 *
 * 从上面跨进来的纵向合并会先缩短一格（否则它的覆盖会落到已经不存在的行上）；该行自己带
 * rowspan > 1 的格子则拒绝 —— 那些格子的内容随行一起消失，而它们覆盖的下面几行会凭空
 * 少一格。要那样做请先拆分合并。
 * @param tree 文档树
 * @param table 表格节点号
 * @param at_row 删第几行
 * @return 一批步骤（行里有跨行合并时拒绝）
 */
StepBatch makeRemoveTableRow(NodeTree& tree, NodeId table, int at_row);

/**
 * 在 at_col 处插入一列（取 [0, 列数]）。
 *
 * 跨过插入线的格子加宽一格（它就是吸收这一列的那一格，跨行合并的格子会在它覆盖的每一行里
 * 都吸收）；其余行各插一个新的空单元格。没有格子跨线时每行都要插，否则总宽度对不上。
 * @param tree 文档树
 * @param table 表格节点号
 * @param at_col 插到第几列之前（等于列数就是追加）
 * @return 一批步骤（失败时带原因）
 */
StepBatch makeInsertTableColumn(NodeTree& tree, NodeId table, int at_col);

/**
 * 删除一列。覆盖这一列的格子变窄一格；宽度降到 0 的格子连同它的内容一起删掉。
 *
 * 删掉最后一列会拒绝：留下一个"有行没有格"的表格是合法的，但没有意义，不如显式说不支持。
 * @param tree 文档树
 * @param table 表格节点号
 * @param at_col 删第几列
 * @return 一批步骤（删最后一列时拒绝）
 */
StepBatch makeRemoveTableColumn(NodeTree& tree, NodeId table, int at_col);

}  // namespace heditor
