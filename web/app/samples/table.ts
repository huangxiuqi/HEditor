/**
 * 表格样例：一句话 + 一张 3x3 表。
 *
 * 示范的是 insertTable 的语义：表插在光标处，插完光标落在第一格，接着 type 就是往格子里
 *   写字。表格结构不在段落流里（格是块的容器），所以往哪一格放字靠光标，不靠坐标。
 * 读回结构走 tableState（行列、合并、列宽、每格的块区间）。换格用的是 Tab，那是输入层的
 *   事；程序化换格没有公开方法，判据要它时会显式说出来，不假装有。
 */
import type { Editor } from '../../src/index';

const LEAD = '下面这张表用 insertTable 插入，插完光标就在第一格，接着 type 就是往格子里写：';

/**
 * 搭表格样例。
 * @param ed 编辑器
 * @returns 搭建完成
 */
export async function buildTable(ed: Editor): Promise<void> {
  await ed.setText(LEAD);
  await ed.paragraphFormat({ size: 12, spaceAfter: 8 });

  // 表插在光标所在处（这一段之后）；插完光标进第一格。
  await ed.setCaret({ para: 0, offset: LEAD.length });
  await ed.insertTable({ rows: 3, cols: 3, header: true });

  // 表头第一格：此时光标已经在这一格里，直接打字。
  await ed.type('项目');
}
