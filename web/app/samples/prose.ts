/**
 * 正文样例：标题 + 四段说明。
 *
 * 示范的是段间距的写法 —— 用 paragraphFormat 的 spaceAfter 表达，不靠空段。空行是排版上
 *   的偶然（换了字号就变高），段后距是属性（跟着样式走，改字号不变形）。
 * 顺带演示 format 与 paragraphFormat 的分工：前者改字，后者改段。
 */
import type { Editor } from '../../src/index';

const TITLE = 'HEditor 示例文档';

const BODY = [
  '这一页由 Vite 直接跑源码：改 web/src 下的任何文件，保存即热更，不用先构建。',
  '引擎（wasm）、命令表、输入层、画布与滚动都在库里面 —— 页面拿到的是一个编辑器对象。',
  '可以试试：点画布落光标、敲字、Ctrl+Z 撤销、拖选。这些都没有在本文件里写一行事件处理。',
  '上面那排按钮全部走公开面：格式走 format / paragraphFormat，回显走 formatAt。',
];

/**
 * 搭正文样例。
 * @param ed 编辑器
 * @returns 搭建完成
 */
export async function buildProse(ed: Editor): Promise<void> {
  // setText 的契约是"一行一段"：换行切段，空行丢弃。所以段间距只能靠属性，不能靠空行。
  await ed.setText([TITLE, ...BODY].join('\n'));

  // 标题：加粗、18 磅、段后 12 磅。字符格式与段落格式是两个命令，各管一层。
  await ed.select({ para: 0, offset: 0 }, { para: 0, offset: TITLE.length });
  await ed.format({ bold: true, size: 18 });
  await ed.paragraphFormat({
    size: 18,
    spaceAfter: 12,
    lineSpacing: { rule: 'auto', factor: 1.3 },
  });

  // 正文：12 磅、1.6 倍行距、段后 6 磅。选区跨段时 paragraphFormat 作用于选中的每一段。
  const last = BODY.length;
  await ed.select({ para: 1, offset: 0 }, { para: last, offset: BODY[last - 1].length });
  await ed.paragraphFormat({
    size: 12,
    spaceAfter: 6,
    lineSpacing: { rule: 'auto', factor: 1.6 },
  });

  await ed.setCaret({ para: 0, offset: 0 });
}
