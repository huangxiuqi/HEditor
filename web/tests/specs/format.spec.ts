// 格式判据：字符格式（粗/斜/下划线/字号）、段落格式（行距）与文档水印都要落到文档上，
// 能在工具栏那套公开面上读写回来，并且画面真的跟着变。
//
// 字符与段落那一半读文档与几何；水印与"改完格式画面重画"必须比像素：改了但画面不重画时，
//   命令返回 true、读回的值也对、增量产物还等于全量 —— 只有像素能看见"没重画"。
import { createHash } from 'node:crypto';

import { check } from '../support/check';
import { test } from '../support/probe';

// 页面里的公共入口。
const D = 'window.__demo';

/**
 * 帧里"有墨"的像素数：任一通道低于阈值就算画了东西。
 *
 * 水印是半透明灰，压在纸面上是浅灰 —— 阈值取 200 时它仍然算墨（纸面是 255）。
 */
const INK = `
  const f = await D.rawFrame();
  let ink = 0;
  for (let i = 0; i + 3 < f.length; i += 4) {
    if (f[i] < 200 || f[i + 1] < 200 || f[i + 2] < 200) ++ink;
  }
  return ink;`;

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

  test('选区中间加粗：只有选中的那几个字变，别处不动', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁戊己庚辛');
      await e.select({ para: 0, offset: 2 }, { para: 0, offset: 5 });
      const ok = await e.format({ bold: true });
      await e.flush();
      const at = async (o) => {
        await e.setCaret({ para: 0, offset: o });
        return (await e.formatAt()).bold === true;
      };
      return { ok, text: e.text(), head: await at(0), in: await at(3), tail: await at(7) };`);
    check('命令返回 true', r.ok === true, String(r.ok));
    check('文本一个字没变', r.text === '甲乙丙丁戊己庚辛', r.text);
    check('★ 选区之后的那一段之前是普通字', r.head === false, String(r.head));
    check('★ 选中的字变粗了', r.in === true, String(r.in));
    check('★ 选区之后的那一段之后还是普通字', r.tail === false, String(r.tail));
  });

  test('回显：有选区时报"共同的标记"，混合选区不点亮', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁戊己庚辛');
      await e.select({ para: 0, offset: 2 }, { para: 0, offset: 5 });
      await e.format({ bold: true });
      await e.flush();
      // 完全落在粗体里
      await e.select({ para: 0, offset: 2 }, { para: 0, offset: 4 });
      const inside = await e.formatAt();
      // 一半粗一半不粗
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 7 });
      const mixed = await e.formatAt();
      // 完全落在普通字里
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      const outside = await e.formatAt();
      return { inside, mixed, outside };`);
    check('★ 整段都在粗体里：报粗', r.inside.bold === true, JSON.stringify(r.inside));
    check('★ 混合选区：不报粗（没有统一值）', r.mixed.bold !== true, JSON.stringify(r.mixed));
    check('★ 全在普通字里：报不粗', r.outside.bold !== true, JSON.stringify(r.outside));
  });

  test('工具条：连点两下增大字号走两档', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      const btn = document.querySelector('.heditor-ribbon__button[data-act="grow"]');
      await e.setText('甲乙丙丁');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      await e.format({ size: 14 });
      await e.flush();
      // 两下之间不等：这正是"读到旧值就只走一档"的那条路。
      btn.click();
      btn.click();
      await new Promise((r) => setTimeout(r, 800));
      await e.setCaret({ para: 0, offset: 1 });
      const mark = await e.formatAt();
      return { mark };`);
    check('★ 两下就是两档（14 → 16 → 18）', r.mark.size === 18, JSON.stringify(r.mark));
  });

  test('撤销：一次撤销把切开的正文合回去', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁戊己庚辛');
      await e.select({ para: 0, offset: 2 }, { para: 0, offset: 5 });
      await e.format({ bold: true });
      await e.flush();
      await e.undo();
      await e.flush();
      await e.setCaret({ para: 0, offset: 3 });
      const mark = await e.formatAt();
      return { text: e.text(), mark };`);
    check('文本还是那八个字', r.text === '甲乙丙丁戊己庚辛', r.text);
    check('★ 一次撤销就把加粗退掉', r.mark.bold !== true, JSON.stringify(r.mark));
  });

  test('整段改字重/斜体：增量排版必须跟全量一致', async ({ open }) => {
    // 这一条挡的是"改完画面上没变、动一下字号才跟着变"：文本没动、run 边界也没动时，
    //   整形缓存若把字重漏在指纹之外，就会拿旧字形的排版结果接着用。自审是唯一能当场
    //   看见这件事的判据（它拿运行时排版跟一次全量重排逐字形比）。
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      const t = '整段对照文字 ABCD';
      await e.setText([t, t, t].join('\\n'));
      await e.flush();
      const audit = async () => ({
        compile: (await e.diag.auditCompile()).diff,
        layout: (await e.diag.auditLayout()).diff,
      });
      const start = await audit();
      // 整段选中：文本与 run 边界都不变，只有标记变。
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 13 });
      await e.format({ bold: true });
      await e.flush();
      const bold = await audit();
      await e.select({ para: 1, offset: 0 }, { para: 1, offset: 13 });
      await e.format({ italic: true });
      await e.flush();
      const italic = await audit();
      await e.select({ para: 2, offset: 0 }, { para: 2, offset: 13 });
      await e.format({ underline: true, size: 20 });
      await e.flush();
      const both = await audit();
      return { start, bold, italic, both };`);
    check('起点就干净', r.start.compile === 0 && r.start.layout === 0, JSON.stringify(r.start));
    check('★ 整段加粗之后：编译与排版都跟上', r.bold.compile === 0 && r.bold.layout === 0,
          JSON.stringify(r.bold));
    check('★ 整段斜体之后：编译与排版都跟上', r.italic.layout === 0, JSON.stringify(r.italic));
    check('★ 下划线 + 字号一起改之后：仍然一致', r.both.layout === 0, JSON.stringify(r.both));
  });

  test('画面重画：粗体、斜体、下划线各改一次，像素必须跟着变', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 一行一个字：这三类改动都不会让任何字形的 x/y 移动（合成倾斜不改推进，下划线是另
    //   画一条线，单字也不动位置），所以"画出来不一样"只能靠标记本身进页指纹。指纹漏了
    //   它们，就是"命令成功、读回也对、画面停在旧帧"。
    await p.ev(`const e = ${D};
      await e.setText('甲\\n乙\\n丙');
      // 光标是瞬态、会闪：设成全透明，截图里只剩内容像素。
      await e.configure({ caretStyle: Object.assign({}, e.config().caretStyle, { color: [0, 0, 0, 0] }) });
      await e.setCaret({ para: 0, offset: 0 });
      await e.flush();`);

    const clip = await p.ev<any>(`
      const e = ${D};
      const r = e.caret().rect;
      const pg = document.querySelector('.heditor-pages').getBoundingClientRect();
      const d = window.devicePixelRatio || 1;
      return { x: pg.left + r.x / d - 8, y: pg.top + r.y / d - 10, width: 160, height: 120 };`);

    /**
     * 拍一张并算摘要：判据只关心"这一拍和上一拍是不是同一张"，不比绝对像素。
     * @returns 截图的摘要
     */
    const shot = async (): Promise<string> => {
      const buf = await p.page.screenshot({ clip });
      return createHash('sha1').update(buf).digest('hex');
    };

    /**
     * 在某一页上打开一种标记，回来之后拍一张。
     * @param para 段号
     * @param key 哪个字段
     * @returns 这一拍与上一拍的两张摘要
     */
    const apply = async (para: number, key: string): Promise<{ before: string; after: string }> => {
      await p.ev(`const e = ${D}; await e.setCaret({ para: ${para}, offset: 0 }); await e.flush();`);
      await p.page.waitForTimeout(300);
      const before = await shot();
      await p.ev(`const e = ${D};
        await e.select({ para: ${para}, offset: 0 }, { para: ${para}, offset: 1 });
        await e.format({ ${key}: true });
        await e.setCaret({ para: ${para}, offset: 0 });
        await e.flush();`);
      await p.page.waitForTimeout(500);
      return { before, after: await shot() };
    };

    const bold = await apply(0, 'bold');
    check('★ 加粗之后画面变了', bold.before !== bold.after, '两拍一模一样');
    const italic = await apply(1, 'italic');
    check('★ 斜体之后画面变了', italic.before !== italic.after, '两拍一模一样');
    const underline = await apply(2, 'underline');
    check('★ 下划线之后画面变了', underline.before !== underline.after, '两拍一模一样');
  });

  test('敲字继承：段落末尾接字沿用前一个字的格式，已有的字不动', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      const bold = async (para, n) => {
        await e.select({ para, offset: 0 }, { para, offset: n });
        await e.format({ bold: true });
        await e.flush();
      };
      const read = async (para, o) => {
        await e.setCaret({ para, offset: o });
        return (await e.formatAt()).bold === true;
      };
      // 末尾接两个字：这两个字必须跟着前一个字变粗。
      //   先收起选区 —— 用户是"点一下末尾再敲"，点完选区就没了；带着选区敲字是替换，
      //   那是另一条路（Word 也一样）。
      await e.setText('甲乙丙丁');
      await bold(0, 4);
      await e.setCaret({ para: 0, offset: 4 });
      await e.insertText({ para: 0, offset: 4 }, '戊己');
      await e.flush();
      const tail = { text: e.text(), 0: await read(0, 0), 3: await read(0, 3),
                     4: await read(0, 4), 5: await read(0, 5) };

      // 段落开头敲字：左边没有字，沿用右边那一段（Word 同一条规则）。
      await e.setText('甲乙');
      await bold(0, 2);
      await e.setCaret({ para: 0, offset: 0 });
      await e.insertText({ para: 0, offset: 0 }, '丙');
      await e.flush();
      const head = { text: e.text(), 0: await read(0, 0), 2: await read(0, 2) };
      return { tail, head };`);
    check('正文没丢没乱', r.tail.text === '甲乙丙丁戊己', r.tail.text);
    check('原来的字还是粗的', r.tail[0] === true && r.tail[3] === true,
          JSON.stringify(r.tail));
    check('★ 末尾接的字跟着变粗（第一个）', r.tail[4] === true, JSON.stringify(r.tail));
    check('★ 末尾接的字跟着变粗（第二个）', r.tail[5] === true, JSON.stringify(r.tail));
    check('开头敲的字沿用右边那段', r.head[0] === true, JSON.stringify(r.head));
    check('后面的字还是粗的', r.head[2] === true, JSON.stringify(r.head));
  });

  test('落点继承：格式段左边取左边、右边取它自己', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      // 一段里只有一个字改了格式：左右两边界各敲一次，答案必须落在各自左边那一段上。
      await e.setText('甲乙丙丁');
      await e.select({ para: 0, offset: 1 }, { para: 0, offset: 2 });
      await e.format({ bold: true });
      await e.clearSelection();
      // 左边界敲：沿用左边「甲」的普通格式，不是右边「乙」的加粗。
      await e.setCaret({ para: 0, offset: 1 });
      await e.insertText({ para: 0, offset: 1 }, 'X');
      // 右边界敲：接在「乙」后面往下写，沿用加粗，不是右边「丙」的普通格式。
      await e.setCaret({ para: 0, offset: 3 });
      await e.insertText({ para: 0, offset: 3 }, 'Y');
      await e.flush();
      // 逐字读（单字选区报的就是那个字自己的标记）。
      const mark = async (o) => {
        await e.select({ para: 0, offset: o }, { para: 0, offset: o + 1 });
        return (await e.formatAt()).bold === true;
      };
      const chars = [];
      for (let i = 0; i < 6; ++i) chars.push(await mark(i));
      // 工具栏回显：空选区落在哪一侧就报哪一侧 —— 与敲字同一个口径。
      const show = async (o) => {
        await e.setCaret({ para: 0, offset: o });
        return (await e.formatAt()).bold === true;
      };
      const caret = { afterLeft: await show(2), afterBold: await show(3) };
      return { text: e.text(), chars, caret };`);
    check('文本接在两侧', r.text === '甲X乙Y丙丁', r.text);
    check('原来的字没被带跑', r.chars[0] === false && r.chars[2] === true && r.chars[4] === false,
          JSON.stringify(r.chars));
    check('★ 左边界敲的字不加粗（沿用左边那一段）', r.chars[1] === false, JSON.stringify(r.chars));
    check('★ 右边界敲的字加粗（接着加粗那一段往下写）', r.chars[3] === true, JSON.stringify(r.chars));
    check('★ 回显同口径：左边界处报普通', r.caret.afterLeft === false, JSON.stringify(r.caret));
    check('★ 回显同口径：加粗段右边报加粗', r.caret.afterBold === true, JSON.stringify(r.caret));
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

test.describe('文档水印', () => {
  test('水印：设上去画得出来，关掉就回去，一步撤销', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    await p.read<any>(`
      await H.setText('正文一二三四五六七八九十');
      await H.flush();
      await D.stopBlink();`);
    const before = await p.read<any>(`return { st: await H.watermark(), ink: await (async () => {${INK}})() };`);
    check('前置：还没有水印', before.st.enabled === false, JSON.stringify(before.st));

    await p.read<any>(`
      await H.setWatermark({ enabled: true, text: '内部资料', sizePt: 48 });
      await H.flush();
      await D.stopBlink();
      return true;`);
    const after = await p.read<any>(`return { st: await H.watermark(), ink: await (async () => {${INK}})() };`);
    check('★ 水印设上去了，读回文字与开关',
          after.st.enabled === true && after.st.text === '内部资料',
          JSON.stringify(after.st));
    check('★ 水印真的画进了帧（墨迹像素变多）', Number(after.ink) > Number(before.ink) + 500,
          `无 ${before.ink} → 有 ${after.ink}`);

    // 关掉：墨迹回落到原位（±60 容许抗锯齿与光标相位的差别）。
    await p.read<any>(`
      await H.setWatermark({ enabled: false });
      await H.flush();
      await D.stopBlink();
      return true;`);
    const off = await p.read<any>(`return { st: await H.watermark(), ink: await (async () => {${INK}})() };`);
    check('关掉之后水印不画了', off.st.enabled === false, JSON.stringify(off.st));
    check('★ 关掉之后墨迹回到原样（差 ' + String(Number(off.ink) - Number(before.ink)) + '）',
          Math.abs(Number(off.ink) - Number(before.ink)) < 60,
          `${before.ink} → ${off.ink}`);

    // 一步撤销：回到"开着"。
    const undone = await p.read<any>(`
      await H.undo();
      await H.flush();
      return await H.watermark();`);
    check('★ 一步撤销把水印开回来', undone.enabled === true && undone.text === '内部资料',
          JSON.stringify(undone));
  });
});
