// 格式判据：字符格式（粗/斜/下划线/字号）与段落格式（行距）都要真的落到文档上，
// 并且能在工具栏那套公开面上读写回来。
//
// 只看内容与几何，不比像素：像素差由 stress 那条差分审计盯着，这里判的是"格式生没生效"。
import { check } from '../support/check';
import { test } from '../support/probe';

// 页面里的公共入口。
const D = 'window.__demo';

test.describe('格式', () => {
  test('字符格式：加粗落到标记上，一次撤销退掉', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      const before = await e.formatAt();
      const ok = await e.format({ bold: true });
      await e.flush();
      await e.setCaret({ para: 0, offset: 1 });
      const after = await e.formatAt();
      const text = e.text();
      await e.undo();
      await e.flush();
      await e.setCaret({ para: 0, offset: 1 });
      const undone = await e.formatAt();
      return { before, ok, after, text, undone };`);
    check('加粗之前没有显式标记', r.before.bold === undefined, JSON.stringify(r.before));
    check('命令返回 true', r.ok === true, String(r.ok));
    check('★ 加粗之后该处的标记是粗的', r.after.bold === true, JSON.stringify(r.after));
    check('文本一个字没变', r.text === '甲乙丙丁', r.text);
    check('★ 一次撤销就把加粗退掉', r.undone.bold !== true, JSON.stringify(r.undone));
  });

  test('幂等：同一个值再设一遍不产生新记录', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      const first = await e.format({ italic: true });
      const second = await e.format({ italic: true });
      await e.flush();
      await e.undo();
      await e.flush();
      await e.setCaret({ para: 0, offset: 1 });
      const undone = await e.formatAt();
      return { first, second, undone };`);
    check('第一次设成功', r.first === true, String(r.first));
    check('★ 第二次返回 false（没有改动，不占撤销栈）', r.second === false, String(r.second));
    check('★ 一次撤销就到了设之前（说明没多记一条）', r.undone.italic !== true,
          JSON.stringify(r.undone));
  });

  test('斜体与下划线各自独立', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      await e.format({ underline: true });
      await e.flush();
      await e.setCaret({ para: 0, offset: 1 });
      const a = await e.formatAt();
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      await e.format({ underline: false });
      await e.flush();
      await e.setCaret({ para: 0, offset: 1 });
      const b = await e.formatAt();
      return { a, b };`);
    check('下划线写进去了', r.a.underline === true, JSON.stringify(r.a));
    check('没有顺手把粗体或斜体也设上', r.a.bold === undefined && r.a.italic === undefined,
          JSON.stringify(r.a));
    check('★ 显式取消下划线（false 不是继承）', r.b.underline === false, JSON.stringify(r.b));
  });

  test('字号：字符字号变大之后页数增加', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      const lines = [];
      for (let i = 0; i < 120; i++) lines.push('第' + (i + 1) + '段的文字内容，用来撑出多页。');
      await e.setText(lines.join('\\n'));
      await e.flush();
      const before = e.geometry().pages;
      await e.selectAll();
      const ok = await e.format({ size: 24 });
      await e.flush();
      const after = e.geometry().pages;
      await e.setCaret({ para: 0, offset: 1 });
      const mark = await e.formatAt();
      return { before, after, ok, mark };`);
    check('命令返回 true', r.ok === true, String(r.ok));
    check('字号写进了标记', r.mark.size === 24, JSON.stringify(r.mark));
    check('★ 字号变大之后页数增加', r.after > r.before, r.before + ' → ' + r.after);
  });

  test('行距：段落格式改变行高', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      const lines = [];
      for (let i = 0; i < 120; i++) lines.push('第' + (i + 1) + '段的文字内容，用来撑出多页。');
      await e.setText(lines.join('\\n'));
      await e.flush();
      const before = e.geometry().pages;
      await e.selectAll();
      const ok = await e.paragraphFormat({ lineFactor: 2 });
      await e.flush();
      const after = e.geometry().pages;
      await e.setCaret({ para: 0, offset: 1 });
      const para = await e.paragraphFormatAt();
      return { before, after, ok, para };`);
    check('命令返回 true', r.ok === true, String(r.ok));
    check('行距写进了段落格式', r.para.lineFactor === 2, JSON.stringify(r.para));
    check('★ 行距放大之后页数增加', r.after > r.before, r.before + ' → ' + r.after);
  });

  test('空选区：字符格式不动任何文字', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.setCaret({ para: 0, offset: 2 });
      await e.clearSelection();
      const ok = await e.format({ bold: true });
      await e.flush();
      await e.setCaret({ para: 0, offset: 1 });
      const mark = await e.formatAt();
      return { ok, mark };`);
    check('空选区返回 false（没有可改的字符）', r.ok === false, String(r.ok));
    check('附近的文字没被改粗', r.mark.bold !== true, JSON.stringify(r.mark));
  });
});
