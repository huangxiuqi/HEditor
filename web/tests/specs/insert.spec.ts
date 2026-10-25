// 插入判据：表格与内容控件都要真的进文档、落在该落的位置，并且画面跟着变。
//
// 为什么单独一份：这两条命令走的是 InsertNode 那条结构步骤，判据要同时钉住三件事 ——
//   结构对（段数、表格行列、控件区间）、落点对（光标进第一格 / 进控件内容末尾）、
//   以及"画面上真的看得见"（结构成功但没重画，命令返回 true、读回也对，只有像素能发现）。
import { createHash } from 'node:crypto';

import { check } from '../support/check';
import { test } from '../support/probe';

const D = 'window.__demo';

test.describe('插入图片', () => {
  test('真实鼠标点图片：选中出拖拽框，光标不动；点正文则取消选中', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 真实鼠标事件这一路必须过一遍：命中 → 图片分支 → 选中，全在输入层里。
    //   合成事件也能"看起来生效"，但它跳过了坐标换算与按下时的按下记账。
    const geo = await p.read<any>(`
      await H.setText('甲乙丙丁');
      await H.setCaret({ para: 0, offset: 2 });
      await H.flush();
      const data = new Uint8Array(32 * 16 * 4).fill(180);
      await H.registerImage({ id: 'pic', width: 32, height: 16, data });
      await H.insertImage({ resourceId: 'pic', pxWidth: 32, pxHeight: 16 });
      await H.flush();
      await D.stopBlink();
      const box = (await H.images())[0];
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      const q = box.rects[0];
      return {
        left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
        zoom: ZOOM(),
        cx: q.x + q.w / 2, cy: q.y + q.h / 2,
        // 图右侧 40 磅处是正文（同一个行盒里的字）
        tx: q.x + q.w + 40, ty: q.y + q.h / 2,
        digest0: await D.frameDigest(),
        caret0: JSON.stringify(H.caret().pos),
      };`);

    const at = (xPt: number, yPt: number) => ({
      x: geo.left + xPt * geo.zoom * geo.sx,
      y: geo.top + yPt * geo.zoom * geo.sy,
    });
    const click = async (xPt: number, yPt: number) => {
      const q = { ...at(xPt, yPt), button: 'left', clickCount: 1, buttons: 1 };
      await p.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...q });
      await p.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...q });
      await p.page.waitForTimeout(40);
    };

    await click(geo.cx, geo.cy);
    const afterImage = await p.read<any>(`
      return { digest: await D.frameDigest(), caret: JSON.stringify(H.caret().pos) };`);
    check('★ 点图片：拖拽框画出来了（帧变了）', afterImage.digest !== geo.digest0,
          geo.digest0 + ' → ' + afterImage.digest);
    check('★ 点图片不落光标', afterImage.caret === geo.caret0,
          afterImage.caret + ' vs ' + geo.caret0);

    await click(geo.tx, geo.ty);
    const afterText = await p.read<any>(`
      return { digest: await D.frameDigest(), caret: JSON.stringify(H.caret().pos) };`);
    check('★ 点正文：光标移过去了（图片没把这一下吞掉）', afterText.caret !== geo.caret0,
          afterText.caret + ' vs ' + geo.caret0);
    check('★ 点正文之后拖拽框消失（帧回到没有框的样子）',
          afterText.digest !== afterImage.digest, afterText.digest);
  });

  test('注册像素、插入、命中把手、改尺寸与撤销', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.setCaret({ para: 0, offset: 2 });
      await e.flush();

      // 32 x 16 的纯色块（RGBA）。像素由宿主解码之后交给引擎，文档里只留资源 id。
      const w = 32, h = 16;
      const data = new Uint8Array(w * h * 4);
      for (let i = 0; i < w * h; i++) {
        data[i * 4] = 20; data[i * 4 + 1] = 120; data[i * 4 + 2] = 220; data[i * 4 + 3] = 255;
      }
      await e.registerImage({ id: 'pic', width: w, height: h, data });
      const digestBefore = await e.diag.frameDigest();
      const ok = await e.insertImage({ resourceId: 'pic', pxWidth: w, pxHeight: h });
      await e.flush();
      const digestAfter = await e.diag.frameDigest();

      const list = await e.images();
      const box = list[0];
      const rect = box ? box.rects[0] : null;
      const body = rect ? await e.imageHit(rect.x + rect.w / 2, rect.y + rect.h / 2) : null;
      const corner = rect ? await e.imageHit(rect.x, rect.y) : null;
      const outside = rect ? await e.imageHit(rect.x + rect.w + 60, rect.y + rect.h + 60) : null;

      // 选中 → 拖拽框与把手进瞬态层（帧上多出蓝色像素）；取消 → 逐像素回到没选中的样子。
      await e.selectImage(0);
      await e.flush();
      const digestSelected = await e.diag.frameDigest();
      await e.selectImage(-1);
      await e.flush();
      const digestCleared = await e.diag.frameDigest();

      const width0 = box ? box.w : 0;
      const height0 = box ? box.h : 0;
      const resized = await e.resizeImage(0, width0 * 2, height0 * 2);
      await e.flush();
      const afterResize = (await e.images())[0];
      await e.undo();
      await e.flush();
      const afterUndo = (await e.images())[0];

      return { ok, list, body, corner, outside, digestBefore, digestAfter, digestSelected,
               digestCleared, width0, height0, resized, afterResize, afterUndo };`);

    check('insertImage 返回 true', r.ok === true, String(r.ok));
    check('★ 文档里有一张图', Array.isArray(r.list) && r.list.length === 1,
          JSON.stringify(r.list));
    check('★ 像素在资源表里', r.list[0]?.ready === true, String(r.list[0]?.ready));
    check('★ 显示尺寸按 96 DPI 折算：32x16 像素 = 24 x 12 磅',
          Math.abs(r.width0 - 24) < 0.01 && Math.abs(r.height0 - 12) < 0.01,
          r.width0 + ' x ' + r.height0);
    check('插进去之后画面变了', r.digestBefore !== r.digestAfter,
          r.digestBefore + ' → ' + r.digestAfter);
    check('★ 点图身命中这张图、不在把手上',
          r.body?.index === 0 && r.body?.handle < 0, JSON.stringify(r.body));
    check('★ 点左上角命中 0 号把手',
          r.corner?.index === 0 && r.corner?.handle === 0, JSON.stringify(r.corner));
    check('点在图外什么都没命中', r.outside?.index === -1, JSON.stringify(r.outside));
    check('★ 选中之后拖拽框可见（帧变了）', r.digestSelected !== r.digestCleared,
          r.digestSelected + ' vs ' + r.digestCleared);
    check('★ 改尺寸生效', r.resized === true &&
          Math.abs(r.afterResize.w - r.width0 * 2) < 0.01,
          r.afterResize?.w + ' / 期望 ' + r.width0 * 2);
    check('★ 撤销回到改尺寸之前', Math.abs(r.afterUndo.w - r.width0) < 0.01,
          r.afterUndo?.w + ' / 期望 ' + r.width0);
  });
});

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

  test('格内换行：第二行要画出来，画面与全量重画逐字节相同', async ({ open }) => {
    // 敲字撑出第二行时，这一行要同时做到三件事：排版把它排进格子的可用宽度里、
    //   行高跟着长、页位图重光栅。少最后一件就是"字进了文档、读回也对、屏幕上没有"。
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.flush();
      await e.insertTable({ rows: 1, cols: 2 });
      await e.flush();
      // 光标在第一格：先记第一行的高度与光标的位置，再敲够多的字把它顶成两行以上。
      const one = e.caret().rect.h;
      const y0 = e.caret().rect.y;
      await e.type('很长的一句话要在格子里折行很长的一句话要在格子里折行很长的一句话要在格子里折行');
      await e.flush();
      const caret = e.caret();
      const rects = e.selection().rects;
      const paragraphs = e.paragraphs();
      // 三次自审：屏幕上的帧、拼起来的底图、以及这一页的位图。
      const frame = (await e.diag.auditFrame()).diff;
      const base = (await e.diag.auditBase()).diff;
      const page = (await e.diag.auditPage(0)).diff;
      return { one, y0, caret, rects, paragraphs, frame, base, page };`);

    // 折行的直接证据是光标落到了下一行：行盒高度不变（它是一行的高度），变的是 y。
    check('★ 格内折行之后光标落到第二行', r.caret && r.caret.rect.y >= r.y0 + r.one * 0.9,
          JSON.stringify({ one: r.one, y0: r.y0, y1: r.caret && r.caret.rect.y }));
    check('★ 折行之后帧与全量重画逐字节相同（第二行画出来了）', r.frame === 0, String(r.frame));
    check('折行之后底图自审为 0', r.base === 0, String(r.base));
    check('折行之后页位图自审为 0', r.page === 0, String(r.page));
  });

  test('工具栏的表格按钮：先出尺寸面板，按选的行列插入', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.flush();
      const btn = document.querySelector('.heditor-ribbon__button[data-act="table"]');
      btn.click();
      await new Promise((r) => setTimeout(r, 30));
      const panel = document.querySelector('.heditor-popover:not([hidden])');
      const cells = panel ? panel.querySelectorAll('.heditor-picker__cell').length : 0;
      const before = e.paragraphs().length;
      // 挑 2 行 4 列：面板上的格按 data-row / data-col 标着位置。
      const pick = panel && panel.querySelector('.heditor-picker__cell[data-row="2"][data-col="4"]');
      if (pick) pick.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
      await e.flush();
      const after = e.paragraphs();
      const closed = !document.querySelector('.heditor-popover:not([hidden])');
      const json = await e.save();
      return { cells, before, after, closed, cells2: (json.match(/"TableCell"/g) || []).length };`);

    check('★ 点表格按钮先出尺寸面板（不直接插表）', r.cells === 64 && r.before === 1,
          JSON.stringify({ cells: r.cells, before: r.before }));
    check('★ 选 2x4 之后插出八格', r.after.length === 9 &&
          (r.after.filter((t: string) => t === '').length === 8), JSON.stringify(r.after));
    check('★ 表结构是 2 行 4 列', r.cells2 === 8, String(r.cells2));
    check('选完面板关掉了', r.closed === true, String(r.closed));
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

  test('空控件嵌套：表单模式下方向键进得去每一层', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 三个都不带内容：后一枚插在前一枚的内容里，于是套成 [ [ [ ] ] ]。三枚括号组的起点
    //   压在同一处文字偏移上，光标只能靠"第几格"（侧别）分辨层级 —— 内容区间一旦退化成
    //   一个点，方向键就寸步难行。
    const r = await p.read<any>(`
      const e = ${D};
      await e.setText('ABC');
      await e.setCaret({ para: 0, offset: 1 });
      await e.flush();
      await e.insertControl({ type: 'text', key: 'outer', label: '' });
      await e.insertControl({ type: 'text', key: 'mid', label: '' });
      await e.insertControl({ type: 'text', key: 'inner', label: '' });
      await e.configure({ mode: 'form' });
      await e.flush();

      // 归属按控件下标（文档顺序 = 外层先）；-1 = 光标不在任何控件里。
      const focusedIndex = () => e.form().findIndex((x) => x.focused);
      /**
       * 派一次真实方向键。
       * @param k key 字段
       * @returns 到下一拍为止
       */
      const press = async (k) => {
        IME().dispatchEvent(
          new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true }));
        await e.flush();
        return focusedIndex();
      };
      const seen = { start: focusedIndex(), right: [], left: [] };
      for (let i = 0; i < 4; ++i) seen.right.push(await press('ArrowRight'));
      for (let i = 0; i < 5; ++i) seen.left.push(await press('ArrowLeft'));
      return { seen, controls: e.form().length, text: e.text() };`);

    check('三个控件都在（嵌套，不是并排）', r.controls === 3, String(r.controls));
    check('文字没被控件切断', r.text === 'ABC', JSON.stringify(r.text));
    check('★ 插完之后光标在最内层（下标 2）', r.seen.start === 2, String(r.seen.start));
    check('★★ 右移逐层往外走：内层 → 中层 → 外层 → 停住',
          JSON.stringify(r.seen.right) === '[1,0,0,0]', JSON.stringify(r.seen.right));
    check('★★ 左移逐层往里走：外层 → 中层 → 内层 → 中层 → 外层',
          JSON.stringify(r.seen.left) === '[1,2,1,0,0]', JSON.stringify(r.seen.left));
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
