// 插入判据：表格与内容控件都要真的进文档、落在该落的位置，并且画面跟着变。
//
// 为什么单独一份：这两条命令走的是 InsertNode 那条结构步骤，判据要同时钉住三件事 ——
//   结构对（段数、表格行列、控件区间）、落点对（光标进第一格 / 进控件内容末尾）、
//   以及"画面上真的看得见"（结构成功但没重画，命令返回 true、读回也对，只有像素能发现）。
import { createHash } from 'node:crypto';

import { check } from '../support/check';
import { test } from '../support/probe';

const D = 'window.__demo';

test.describe('插入表格', () => {
  test('结构、落点与撤销：表格落在段后，光标进第一格', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.setCaret({ para: 0, offset: 2 });
      await e.flush();
      const ok = await e.insertTable({ rows: 2, cols: 3, header: true });
      await e.flush();

      const paras = e.paragraphs();
      const caret = e.caret();
      const json = await e.save();
      // 光标已经在第一格里：接着打字就该进那一段。
      const typed = await e.type('甲');
      await e.flush();
      const firstCell = e.paragraph(1);
      const layoutDiff = (await e.diag.auditLayout()).diff;
      const compileDiff = (await e.diag.auditCompile()).diff;

      // 撤销两次：先退那个字，再退表格本身。
      await e.undo();
      await e.undo();
      await e.flush();
      const afterUndo = e.paragraphs();
      const undoneJson = await e.save();
      return { ok, paras, caret, json, typed, firstCell, layoutDiff, compileDiff, afterUndo,
               undoneJson };`);

    check('insertTable 返回 true', r.ok === true, String(r.ok));
    check('★ 段数 = 1 段正文 + 6 格', r.paras.length === 7, JSON.stringify(r.paras));
    check('原段落没被表格切开', r.paras[0] === '甲乙丙丁', JSON.stringify(r.paras[0]));
    check('★ 光标落在第一格（第 1 段）', r.caret.pos.para === 1, JSON.stringify(r.caret.pos));
    check('★ 打字进的是第一格', r.typed === true && r.firstCell === '甲',
          JSON.stringify(r.firstCell));
    check('★ 保存的文档里有两行三列的表格结构',
          /"Table"/.test(r.json) && (r.json.match(/"TableCell"/g) || []).length === 6,
          'TableCell x' + (r.json.match(/"TableCell"/g) || []).length);
    check('插入之后增量排版 == 全量', r.layoutDiff === 0, String(r.layoutDiff));
    check('插入之后编译产物 == 全量', r.compileDiff === 0, String(r.compileDiff));
    check('★ 撤销两次之后回到一段', r.afterUndo.length === 1, JSON.stringify(r.afterUndo));
    check('★ 撤销之后文档里没有表格', !/"Table"/.test(r.undoneJson),
          (r.undoneJson.match(/"Table"/g) || []).length + ' 处');
  });

  test('画面重画：插一张表，帧必须变；撤销之后逐像素复原', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 两张判据分工不同：
    //   截图证明"上屏了"（页位图变了之后画布要跟着换，否则用户看不见）；
    //   帧指纹证明"撤销之后内容逐像素回到原样"—— 它不受 DOM 层（滚动条、光标残影）影响。
    const r = await p.ev<any>(`
      const e = ${D};
      // 读指纹前先落一次光标：setCaret 会把相位拉回可见，两次读的瞬态因而可比。
      const digestAt = async (para, offset) => {
        await e.setCaret({ para, offset });
        await e.flush();
        return await e.diag.frameDigest();
      };
      await e.setText('甲乙丙丁');
      await e.flush();
      const before = await digestAt(0, 0);
      await e.insertTable({ rows: 2, cols: 3 });
      await e.flush();
      const after = await e.diag.frameDigest();
      await e.undo();
      await e.flush();
      const undone = await digestAt(0, 0);
      return { before, after, undone };`);

    const clip = await p.ev<any>(`
      const pg = document.querySelector('.heditor-pages').getBoundingClientRect();
      return { x: pg.left + 40, y: pg.top + 40, width: 320, height: 220 };`);

    /**
     * 拍一张并算摘要。
     * @returns 截图摘要
     */
    const shot = async (): Promise<string> => {
      const buf = await p.page.screenshot({ clip });
      return createHash('sha1').update(buf).digest('hex');
    };

    // 截图那半重新走一遍：上面那一段结束时文档已经撤销回来了。
    const beforeShot = await shot();
    await p.ev(`const e = ${D}; await e.setCaret({ para: 0, offset: 0 }); await e.flush();`);
    const settledShot = await shot();
    await p.ev(`const e = ${D}; await e.insertTable({ rows: 2, cols: 3 }); await e.flush();`);
    const afterShot = await shot();

    check('★ 插表之后帧内容变了（页位图重光栅了）', r.before !== r.after,
          r.before + ' → ' + r.after);
    check('★ 撤销之后帧内容逐像素回到插表之前', r.undone === r.before,
          r.before + ' vs ' + r.undone);
    check('★ 插表那一下在屏幕上看得见', settledShot !== afterShot,
          settledShot.slice(0, 8) + ' → ' + afterShot.slice(0, 8));
  });

  test('越界参数与模式：不落地、不抛错', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.flush();
      const before = e.text();
      const zeroRows = await e.insertTable({ rows: 0, cols: 3 });
      const zeroCols = await e.insertTable({ rows: 3, cols: 0 });
      await e.flush();
      const untouched = e.text();
      // 表单模式是填值态：不该长出新的表。
      await e.configure({ mode: 'form' });
      await e.flush();
      const inForm = await e.insertTable({ rows: 2, cols: 2 });
      await e.configure({ mode: 'document' });
      await e.flush();
      return { before, zeroRows, zeroCols, untouched, inForm, paras: e.paragraphs().length };`);

    check('rows = 0 返回 false', r.zeroRows === false, String(r.zeroRows));
    check('cols = 0 返回 false', r.zeroCols === false, String(r.zeroCols));
    check('★ 被拒时文档一个字都没动', r.untouched === r.before, r.untouched);
    check('★ 表单模式下插入表格被拒', r.inForm === false, String(r.inForm));
    check('表单模式那一次也没有留下空段', r.paras === 1, String(r.paras));
  });
});

test.describe('插入控件', () => {
  test('区间、取值与两次撤销', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.setCaret({ para: 0, offset: 2 });
      await e.flush();
      const ok = await e.insertControl({ type: 'text', key: 'patient', label: '张三' });
      await e.flush();

      const text = e.text();
      const caret = e.caret();
      const form = e.form();
      // 光标在控件内容末尾：接着打字就是填这个字段。
      const typed = await e.type('丰');
      await e.flush();
      const filled = e.text();
      const focused = e.form()[0] ? e.form()[0].focused : null;

      await e.undo();
      await e.undo();
      await e.flush();
      const afterUndo = e.text();
      const formAfter = e.form();
      return { ok, text, caret, form, typed, filled, focused, afterUndo, formAfter };`);

    check('insertControl 返回 true', r.ok === true, String(r.ok));
    check('★ 控件不切断段落：文字是"甲乙张三丙丁"', r.text === '甲乙张三丙丁', r.text);
    check('★ 光标落在控件内容末尾（偏移 4）', r.caret.pos.offset === 4,
          JSON.stringify(r.caret.pos));
    check('★ 表单清单里出现一个控件', Array.isArray(r.form) && r.form.length === 1,
          JSON.stringify(r.form));
    check('★ 光标在控件里（focused 为真）', r.focused === true, String(r.focused));
    check('★ 接着打字进的是控件', r.typed === true && r.filled === '甲乙张三丰丙丁', r.filled);
    check('★ 两次撤销回到纯文本', r.afterUndo === '甲乙丙丁', r.afterUndo);
    check('★ 撤销之后控件清单空了', Array.isArray(r.formAfter) && r.formAfter.length === 0,
          JSON.stringify(r.formAfter));
  });

  test('画面重画：插一个字段，括号与底纹必须画出来', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    await p.ev(`const e = ${D};
      await e.setText('甲乙丙丁');
      await e.configure({ caretStyle: Object.assign({}, e.config().caretStyle, { color: [0, 0, 0, 0] }) });
      await e.setCaret({ para: 0, offset: 0 });
      await e.flush();`);

    const clip = await p.ev<any>(`
      const e = ${D};
      const r = e.caret().rect;
      const pg = document.querySelector('.heditor-pages').getBoundingClientRect();
      const d = window.devicePixelRatio || 1;
      return { x: pg.left + r.x / d - 6, y: pg.top + r.y / d - 8, width: 360, height: 120 };`);

    /**
     * 拍一张并算摘要。
     * @returns 截图摘要
     */
    const shot = async (): Promise<string> => {
      const buf = await p.page.screenshot({ clip });
      return createHash('sha1').update(buf).digest('hex');
    };

    const before = await shot();
    await p.ev(`const e = ${D};
      await e.setCaret({ para: 0, offset: 2 });
      await e.insertControl({ type: 'text', key: 'name', label: '张三' });
      await e.flush();`);
    const after = await shot();
    check('★ 插字段之后画面真的变了', before !== after,
          before.slice(0, 8) + ' → ' + after.slice(0, 8));
  });

  test('只读模式拒绝，且不改文档', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.configure({ mode: 'readOnly' });
      await e.flush();
      const refused = await e.insertControl({ type: 'text', label: '不该出现' });
      await e.configure({ mode: 'document' });
      await e.flush();
      return { refused, text: e.text(), form: e.form() };`);

    check('★ 只读模式下插入控件被拒', r.refused === false, String(r.refused));
    check('文档没变', r.text === '甲乙丙丁', r.text);
    check('没有留下控件', Array.isArray(r.form) && r.form.length === 0, JSON.stringify(r.form));
  });
});
