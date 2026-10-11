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
#include "core/doc/table_spec.h"

namespace heditor {

/**
 * 派生网格里的一个格位：它属于哪个单元格，以及那个单元格的原点在哪。
 *
 * row / col 存的是原点坐标，不是当前格位坐标 —— 这正是"落在合并区域内返回原点格"
 * 这条规则的落点：查任意格位拿到的都是同一个原点。
 */
struct GridCell {
    NodeId id = kInvalidNodeId;
    int row = 0;
    int col = 0;
};

/**
 * 派生网格。行优先，长度是 rows * cols。
 */
struct TableGrid {
    int rows = 0;
    int cols = 0;
    std::vector<GridCell> slots;

    /**
     * 查一个格位。越界返回空；落在合并区域内时返回该区域的原点格（左上角）。
     * 这是唯一能让 cell(r, c) 在任何表上都可用的规则，否则调用方每次都要先自己判断
     * "这个格位是不是被别人合并了"。
     */
    const GridCell* at(int row, int col) const;

    /**
     * 两个格位是不是同一个单元格（也就是它们之间那条网格线被合并跨过）。
     */
    bool sameOrigin(int row_a, int col_a, int row_b, int col_b) const;

    /**
     * 某个单元格在它所在行里是第几格（按原点格算）。不在网格里时返回 -1。
     *
     * 行列操作要靠它定位"新格子插在哪两个格子之间" —— 插在"原列号不小于插入点的第一个
     * 格子"之前；跨过插入线的格子原列号更小，所以它留在前面吸收新列。
     */
    int originColOf(NodeId id) const;
};

/**
 * 从跨度推导网格。失败时给出原因（重叠、空洞、合并超出表格末尾）。
 *
 * 列数由第一行自己的跨度之和定（第一行没有东西跨进来，所以它的覆盖范围就是列数），之后
 * 每一行都必须被完整覆盖且不留缝隙 —— 这就是"每行的跨度之和相等、等于网格列数"那条不变量
 * 的落点。注意那句话说的是"这一行被覆盖的格位数"，不是"这一行自己的格子的跨度之和"：纵向
 * 合并覆盖的行本来就没有那么多自己的格子（被合并掉的格位在模型里不存在），按后者判会把
 * 合法的表判成非法。
 */
bool buildGrid(const NodeTree& tree, NodeId table, TableGrid* out, std::string* error);

/**
 * 一批步骤。表格的行列操作由多条最小步骤组成，所以构造器返回一批而不是一条。
 */
struct StepBatch {
    std::vector<Step> steps;
    bool ok = false;
    std::string error;
};

/**
 * 在 at_row 处插入一行（取 [0, 行数]，等于行数就是追加）。
 *
 * cells 是这一行的单元格描述，跨度之和必须等于列数（否则新行与网格对不上）。
 * 新行里的格子不允许 rowspan > 1 —— 那会与下面的行重叠；要跨行合并请在插入之后再做。
 */
StepBatch makeInsertTableRow(NodeTree& tree, NodeId table, int at_row,
                             const std::vector<NodeSpec>& cells);

/**
 * 删除一行。
 *
 * 从上面跨进来的纵向合并会先缩短一格（否则它的覆盖会落到已经不存在的行上）；该行自己带
 * rowspan > 1 的格子则拒绝 —— 那些格子的内容随行一起消失，而它们覆盖的下面几行会凭空
 * 少一格。要那样做请先拆分合并。
 */
StepBatch makeRemoveTableRow(NodeTree& tree, NodeId table, int at_row);

/**
 * 在 at_col 处插入一列（取 [0, 列数]）。
 *
 * 跨过插入线的格子加宽一格（它就是吸收这一列的那一格，跨行合并的格子会在它覆盖的每一行里
 * 都吸收）；其余行各插一个新的空单元格。没有格子跨线时每行都要插，否则总宽度对不上。
 */
StepBatch makeInsertTableColumn(NodeTree& tree, NodeId table, int at_col);

/**
 * 删除一列。覆盖这一列的格子变窄一格；宽度降到 0 的格子连同它的内容一起删掉。
 *
 * 删掉最后一列会拒绝：留下一个"有行没有格"的表格是合法的，但没有意义，不如显式说不支持。
 */
StepBatch makeRemoveTableColumn(NodeTree& tree, NodeId table, int at_col);

}  // namespace heditor
