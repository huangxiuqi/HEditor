/**
 * 长文样例：一段正文重复铺开，用来把文档撑到多页。
 *
 * setText 的 repeat 就是为这件事准备的：它把同一批段落整体复制 N 遍，一次调用、一次排版。
 *   判据量多页、行带、增量时用它当底稿 —— 一份 300 段的文档，靠逐段 insertText 铺要几百次
 *   往返，而那是判据在测自己的耐心，不是编辑器。
 */
import type { Editor } from '../../src/index';

const PARA =
  '这一段用于把文档撑到多页：固定的长度、固定的格式，重复铺开之后用来验证分页、行带与增量排版。';

/**
 * 每份长文铺多少段。
 */
const PARAS = 300;

/**
 * 搭长文样例。
 * @param ed 编辑器
 * @returns 搭建完成
 */
export async function buildLong(ed: Editor): Promise<void> {
  await ed.setText(PARA, { repeat: PARAS });
  // 全选再改段落格式：不带选区时 paragraphFormat 只作用于光标那一段。
  await ed.selectAll();
  await ed.paragraphFormat({ size: 12, spaceAfter: 6, lineSpacing: { rule: 'auto', factor: 1.5 } });
  await ed.setCaret({ para: 0, offset: 0 });
}
