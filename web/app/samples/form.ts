/**
 * 表单样例：一份门诊登记单。
 *
 * 示范的是控件怎么落到正文里：控件是文档流的一部分，插入点是光标，插完光标停在控件内容里；
 *   它占宽度（括号进流），所以段落会重排 —— 这就是"控件不是浮层"的直观样子。
 * 五种都插一个：文本、日期、下拉、复选、只读。只读那条演示"填表的人只能看"：业务只读与
 *   编辑模式是两件事，后者由 configure 的 mode 管。
 */
import type { Editor } from '../../src/index';

const TITLE = '门诊登记';

/**
 * 每一行：提示语 + 这一行末尾要插的控件（null = 纯文本行）。
 */
const ROWS: Array<{
  text: string;
  control?: Parameters<Editor['insertControl']>[0];
}> = [
  { text: '姓名：', control: { type: 'text', key: 'name', placeholder: '请填写姓名' } },
  { text: '出生日期：', control: { type: 'date', key: 'birth' } },
  {
    text: '科室：',
    control: {
      type: 'dropdownList',
      key: 'dept',
      label: '内科',
      options: [
        { value: 'internal', label: '内科' },
        { value: 'surgery', label: '外科' },
        { value: 'derma', label: '皮肤科' },
      ],
    },
  },
  { text: '是否复诊：', control: { type: 'checkBox', key: 'followup' } },
  { text: '接诊医生：', control: { type: 'text', key: 'doctor', label: '张医生', readonly: true } },
  { text: '主诉：', control: { type: 'text', key: 'complaint', placeholder: '一句话写清症状' } },
];

/**
 * 搭表单样例。
 * @param ed 编辑器
 * @returns 搭建完成
 */
export async function buildForm(ed: Editor): Promise<void> {
  await ed.setText([TITLE, ...ROWS.map((r) => r.text)].join('\n'));

  // 标题加粗；其余行给一份统一的段后距（表单要一眼看清一行一项）。
  await ed.select({ para: 0, offset: 0 }, { para: 0, offset: TITLE.length });
  await ed.format({ bold: true, size: 16 });
  await ed.select({ para: 0, offset: 0 }, { para: ROWS.length, offset: ROWS[ROWS.length - 1].text.length });
  await ed.paragraphFormat({ size: 12, spaceAfter: 10, lineSpacing: { rule: 'auto', factor: 1.4 } });

  // 逐行插控件：插入点就是光标，所以先把光标放到那一行的末尾。
  //   从下往上插也一样 —— 控件插在本段内，不会挪动后面段落的编号。
  for (let i = 0; i < ROWS.length; ++i) {
    const row = ROWS[i];
    if (!row.control) continue;
    const para = i + 1;
    await ed.setCaret({ para, offset: row.text.length });
    await ed.insertControl(row.control);
  }

  await ed.setCaret({ para: 1, offset: ROWS[0].text.length });
}
