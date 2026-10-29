// 表格判据：跨页切片之后，续排片段必须仍然是"点得到、选得中、画得对"的一片；
// 列宽行高由格线拖动改，改完表宽与相邻列的和都不变。
//
// 为什么单独一份：切片是排版层唯一一处"同一条行盒换到另一页"的地方，行盒、字形与
//   格框三者的页号必须一起走。它错了的时候命令全部返回成功、增量与全量自审也全绿
//   （两边共用同一份错），只有"点一下、看一眼像素"能发现。
//
// 两把尺子要分清：caret().rect 是设备像素，selection().rects 是磅（现状如此，
//   见 types.ts 里这两个类型下面各写各的）。凡是要送进 diag.hit 的坐标都先折成磅。
import { check } from '../support/check';
import { test } from '../support/probe';

const D = 'window.__demo';

// 造一张会跨页的表：3x3，第一格塞一段很长的文字，其余格空着。
//
// 为什么用"一段长文字"而不是"多按几次回车"：要跨页，这一行必须比一整页还高。
//   多段要几十次往返，而一段长文字一次输入就够 —— 折行把它撑到一页以上，
//   于是它同时是"格内折行"与"行跨页切片"两个场景。
// 段号是稳定的：0 是正文，1..9 是九个格（行优先）。
const LONG = '一二三四五六七八九十'.repeat(220);
const BUILD = `
  await e.setText('首段');
  await e.setCaret({ para: 0, offset: 2 });
  await e.flush();
  await e.insertTable({ rows: 3, cols: 3 });
  await e.flush();
  await e.type(LONG_TEXT);
  await e.flush();
`;

test.describe('表格：跨页切片', () => {
  test('第二页上点空格子，光标留在那一格', async ({ open }) => {
    test.setTimeout(300_000);
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const e = ${D};
      const LONG_TEXT = ${JSON.stringify(LONG)};
      ${BUILD}
      // 段号固定：0 是正文，1 是第一格，2、3 是行 0 另外两格，4..6 / 7..9 是后面两行。
      const zp = await D.pagePx();
      const Z = ZOOM();
      const stepPt = (zp.h + zp.gap) / Z;
      const at = async (para) => {
        await e.setCaret({ para, offset: 0 });
        await e.flush();
        const c = e.caret();
        // caret().rect 是设备像素，换成磅再送进 diag.hit。
        return { page: c.page, x: c.rect.x / Z, y: c.rect.y / Z };
      };
      const c1 = await at(2);
      const c2 = await at(3);
      const row1 = await at(4);
      // 行 0 的续排片段横跨后面几页：点第 2 页那一片的中段。
      const page = 1;
      const localY = (zp.h / Z * 0.06 + row1.y) * 0.5;
      const yDoc = page * stepPt + localY;
      const hits = [];
      for (const c of [c1, c2]) {
        // 命中给的是位置；这条判据要比"落在哪一段"，用诊断的换算读回地址。
        const q = await D.hit(c.x + 40, yDoc);
        hits.push(q && q.position && q.position.flow >= 0
          ? await D.textPosOf(q.position.flow)
          : null);
      }
      // 端到端：真的点一下，光标要跟着走。
      await D.clickAt({ x: (c1.x + 40) * Z, y: yDoc * Z });
      await e.flush();
      const after = e.caret();
      return { c1, c2, row1, page, localY, yDoc, hits,
               after: { para: after.pos.para, page: after.page },
               pages: e.geometry().pages };`);

    check('前提：表格确实跨页了', r.pages >= 2, `${r.pages} 页`);
    check('前提：第二页那一片在行 1 的上面（点的确实是续排片段）', r.localY < r.row1.y,
          `局部 y=${r.localY}，行 1 顶=${r.row1.y}`);
    check('命中：点续排片段第 2 列的空白区落在这一格', r.hits[0]?.para === 2,
          JSON.stringify(r.hits[0]));
    check('命中：点续排片段第 3 列的空白区落在这一格', r.hits[1]?.para === 3,
          JSON.stringify(r.hits[1]));
    check('端到端：点下去光标真的进了那一格（不是旁边有字的格）', r.after.para === 2,
          JSON.stringify(r.after));
  });

  test('整格选中空格子：反白铺满这条行跨过的每一页', async ({ open }) => {
    test.setTimeout(300_000);
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const e = ${D};
      const LONG_TEXT = ${JSON.stringify(LONG)};
      ${BUILD}
      await D.selectCell({ para: 2, offset: 0 });
      await e.flush();
      const sel = SEL();
      return { rects: sel.rects, pages: sel.rects.map((x) => x.page),
               empty: sel.empty, text: e.paragraph(2) };`);
    // 空格子的选区在流里是一对相等的端点（selection().empty 因此是 true），
    //   但整格反白照样要画满 —— 这一条判的就是"别拿 empty 当'没有选区'"。
    check('前提：选的是那个空格子（它没有内容）', r.text === '', JSON.stringify(r.text));
    check('反白铺到 3 页以上（这条行确实跨了多页）', r.rects.length >= 3,
          `${r.rects.length} 条`);
    check('页号逐页递增、从第 1 页起',
          r.pages.every((v: number, i: number) => v === i), JSON.stringify(r.pages));
    check('每一条都不是零面积', r.rects.every((x: any) => x.w > 10 && x.h > 5),
          JSON.stringify(r.rects.map((x: any) => [x.w, x.h])));
    check('同一条行的各片宽度相同（列宽不随页变）',
          new Set(r.rects.map((x: any) => Math.round(x.w))).size === 1,
          JSON.stringify(r.rects.map((x: any) => x.w)));
  });

  // 插入点钉在焦点格的末尾，不是矩形右下角：整格拖选之后敲的字进哪一格由它决定。
  //   从右下拖到左上时焦点在左上格，取矩形右下角会让后续输入落到对角那一格。
  test('整格拖选：插入点钉在焦点那一格的末尾', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲');
      await H.flush();
      await H.insertTable({ rows: 2, cols: 2 });
      await H.flush();
      const put = async (para, txt) => {
        await H.setCaret({ para, offset: 0 });
        await H.flush();
        await H.type(txt);
        await H.flush();
      };
      await put(1, 'A'); await put(2, 'B'); await put(3, 'C'); await put(4, 'D');
      const at = async (para) => {
        await H.setCaret({ para, offset: 1 });
        await H.flush();
        const q = CARET();
        return { x: q.x, y: q.y, h: q.h };
      };
      const fr = FRAME().getBoundingClientRect();
      return { a: await at(1), d: await at(4), left: fr.left, top: fr.top };`);

    // 反向拖：从右下格（D）起手，落在左上格（A）。焦点是后松手的那一端。
    await p.page.mouse.move(geo.left + geo.d.x + 3, geo.top + geo.d.y + geo.d.h / 2);
    await p.page.mouse.down();
    await p.page.mouse.move(geo.left + geo.a.x + 6, geo.top + geo.a.y + geo.a.h / 2,
                            { steps: 10 });
    await p.page.mouse.up();
    const r = await p.read<any>(`
      await H.flush();
      const st = await H.tableState();
      const sel = H.selection();
      const c = CARET();
      return { cells: st.selCells, caret: c, range: sel.cells,
               selRow: st.cellRow, selCol: st.cellCol,
               selRows: st.selRows, selCols: st.selCols };`);
    check('前提：拖选盖住了四格', r.cells === 4, String(r.cells));
    check('\u2605 插入点在焦点格（左上，段 1）的末尾，不在矩形右下角',
          r.caret.para === 1 && r.caret.offset === 1,
          JSON.stringify({ para: r.caret.para, offset: r.caret.offset }));
    // 矩形是选区的原值，读它而不是从端点或几何反算：行号列号不是几何量。
    check('前提：公开面的选区给出了整格矩形', !!r.range, JSON.stringify(r.range));
    check('\u2605 整格矩形是 2x2，行列都含两端',
          !!r.range && r.range.rows[0] === 0 && r.range.rows[1] === 1 &&
          r.range.cols[0] === 0 && r.range.cols[1] === 1,
          JSON.stringify(r.range));
    check('行列跨度与 tableState 的整格范围一致',
          !!r.range && r.range.rows[1] - r.range.rows[0] + 1 === r.selRows &&
          r.range.cols[1] - r.range.cols[0] + 1 === r.selCols,
          JSON.stringify({ range: r.range, selRows: r.selRows, selCols: r.selCols }));
    // 焦点格是松手那一端（左上），与光标所在格是同一格 —— 两条读数各自算，互为交叉验证。
    check('\u2605 焦点格是松手的左上格，与光标所在格一致',
          !!r.range && r.range.focus[0] === r.selRow && r.range.focus[1] === r.selCol,
          JSON.stringify({ focus: r.range?.focus, row: r.selRow, col: r.selCol }));

    const text = await p.read<any>(`
      await H.select({ para: 0, offset: 0 }, { para: 0, offset: 1 });
      await H.flush();
      const sel = H.selection();
      return { range: sel.cells, empty: sel.empty };`);
    check('反向：文字选区没有整格矩形（有字被选中，但不是整格）',
          text.range === null && text.empty === false,
          JSON.stringify({ range: text.range, empty: text.empty }));
  });

  test('格内换行：折行之后的字必须画出来', async ({ open }) => {
    test.setTimeout(300_000);
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const e = ${D};
      await e.setText('首段');
      await e.setCaret({ para: 0, offset: 2 });
      await e.flush();
      await e.insertTable({ rows: 2, cols: 2 });
      await e.flush();
      await e.type('一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十' +
                   '一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十' +
                   '一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十');
      await e.flush();
      const text = e.paragraph(1);
      const cp = Array.from(text).length;
      const at = async (off) => {
        await e.setCaret({ para: 1, offset: off });
        await e.flush();
        const c = e.caret();
        return { page: c.page, x: c.rect.x, y: c.rect.y };
      };
      const head = await at(0);
      const tail = await at(cp);
      await D.stopBlink();
      await at(0);
      const frame = await D.rawFrame();
      const w = D.frameW();
      // 帧与 caret 都是设备像素：直接取第二行的带，数一块矩形里的墨。
      const ink = (yTop, h) => {
        let n = 0;
        for (let y = Math.round(yTop); y < Math.round(yTop) + h; y++) {
          for (let x = Math.round(head.x) + 2; x < Math.round(head.x) + 180; x++) {
            const at4 = (y * w + x) * 4;
            if (at4 + 3 < frame.length && frame[at4] < 160) n++;
          }
        }
        return n;
      };
      const inkFirst = ink(head.y, 20);
      const inkSecond = ink(tail.y, 20);
      const pages = e.geometry().pages;
      const audits = [];
      for (let pg = 0; pg < pages; pg++) audits.push((await D.auditPage(pg)).diff);
      return { head, tail, inkFirst, inkSecond, pages, audits, cp };`);

    check('前提：这一段折成了两行（两个偏移的光标 y 不同）', r.tail.y > r.head.y,
          JSON.stringify([r.head.y, r.tail.y]));
    check('前提：第一行有墨（采样位置对）', r.inkFirst > 40, `${r.inkFirst} 个深色像素`);
    check('★ 第二行也有墨（折行之后的字画出来了）', r.inkSecond > 40,
          `${r.inkSecond} 个深色像素`);
    check('每一页的位图都与从零光栅相同', r.audits.every((d: number) => d === 0),
          JSON.stringify(r.audits));
    check('文本一字不差（折行不改内容）', r.cp === 90, `${r.cp} 个码点`);
  });

  test('连页模式：多行的表逐行下推，不塌回带顶', async ({ open }) => {
    test.setTimeout(300_000);
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const e = ${D};
      await e.configure({ pageMode: 'continuous' });
      await e.setText('首段');
      await e.setCaret({ para: 0, offset: 2 });
      await e.flush();
      await e.insertTable({ rows: 3, cols: 2 });
      await e.flush();
      for (let i = 0; i < 20; i++) {
        await e.type('连页表格第' + i + '行');
        await e.splitParagraph();
      }
      await e.flush();
      const n = e.paragraphs().length;
      // 行 1、行 2 的第一格：段号从末尾数（每行两格）。
      const ys = [];
      for (const para of [1, n - 4, n - 2]) {
        await e.setCaret({ para, offset: 0 });
        await e.flush();
        const c = e.caret();
        ys.push({ para, page: c.page, y: c.rect.y });
      }
      const layoutDiff = (await D.auditLayout()).diff;
      const pages = e.geometry().pages;
      await e.configure({ pageMode: 'paged' });
      return { ys, layoutDiff, pages, n };`);
    check('三行的首行位置严格递增（表没有塌回带顶）',
          r.ys[0].y < r.ys[1].y && r.ys[1].y < r.ys[2].y, JSON.stringify(r.ys));
    check('行 1、行 2 落在第一条带里（页号 0）',
          r.ys[1].page === 0 && r.ys[2].page === 0, JSON.stringify(r.ys));
    check('连页：增量排版 == 全量', r.layoutDiff === 0, String(r.layoutDiff));
  });

  test('空格的段落标记：每一页上每一格都要画出来', async ({ open }) => {
    test.setTimeout(300_000);
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const e = ${D};
      const LONG_TEXT = ${JSON.stringify(LONG)};
      ${BUILD}
      // 每个空格的插入点就是那一格空行的行首 —— 空行没有字，段落标记是这一格在版面上
      //   唯一画得出来的东西，所以"这个位置有没有墨"就是"标记画了没有"。
      const spots = [];
      for (const para of [2, 3, 4, 5, 6]) {
        await e.setCaret({ para, offset: 0 });
        await e.flush();
        const c = e.caret();
        spots.push({ para, page: c.page, x: c.rect.x, y: c.rect.y });
      }
      // 光标挪走再数墨：帧里那一条竖线会盖住采样区。
      await D.stopBlink();
      await e.setCaret({ para: 0, offset: 0 });
      await e.flush();
      const frame = await D.rawFrame();
      const w = D.frameW();
      const zp = await D.pagePx();
      const step = zp.h + zp.gap;
      const ink = (s) => {
        const x0 = s.x + 1;
        const y0 = s.page * step + s.y + 2;
        let n = 0;
        for (let y = Math.round(y0); y < Math.round(y0 + 20); y++) {
          for (let x = Math.round(x0); x < Math.round(x0 + 14); x++) {
            const at4 = (y * w + x) * 4;
            if (at4 + 3 < frame.length && frame[at4] < 160) n++;
          }
        }
        return n;
      };
      return {
        spots: spots.map((s) => ({ ...s, ink: ink(s) })),
        pages: e.geometry().pages,
        // 每一页的位图都要与从零光栅相同（标记漏画时这一步也会红）。
        audits: await (async () => {
          const out = [];
          const n = e.geometry().pages;
          for (let pg = 0; pg < n; pg++) out.push((await D.auditPage(pg)).diff);
          return out;
        })(),
      };`);
    // 第 2、3 段在第 1 页，第 4~6 段在续排之后的第 2 页 —— 两边都要有墨。
    check('前提：表格确实跨页了', r.pages >= 2, `${r.pages} 页`);
    check('前提：两类格子落在不同页上（前面两格在这页、后面三格在下一页）',
          r.spots[0].page === 0 && r.spots[1].page === 0 && r.spots[4].page >= 1,
          JSON.stringify(r.spots.map((s: any) => [s.para, s.page])));
    check('★ 第 1 页上那两个空格的标记画出来了',
          r.spots.slice(0, 2).every((s: any) => s.ink > 0),
          JSON.stringify(r.spots.map((s: any) => [s.para, s.ink])));
    check('★ 第 2 页上那三个空格的标记也画出来了',
          r.spots.slice(2).every((s: any) => s.ink > 0),
          JSON.stringify(r.spots.map((s: any) => [s.para, s.ink])));
    check('每一页的位图都与从零光栅相同', r.audits.every((d: number) => d === 0),
          JSON.stringify(r.audits));
  });

  test('切片之后每一页的位图都自洽，帧指纹稳定', async ({ open }) => {
    test.setTimeout(300_000);
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const e = ${D};
      const LONG_TEXT = ${JSON.stringify(LONG)};
      ${BUILD}
      const pages = e.geometry().pages;
      const digests = [];
      for (let pg = 0; pg < pages; pg++) digests.push(await D.auditPage(pg));
      await D.stopBlink();
      await e.setCaret({ para: 0, offset: 0 });
      await e.flush();
      const before = await D.frameDigest();
      await e.configure({ pageMode: 'paged', zoom: 1 });
      await e.flush();
      const after = await D.frameDigest();
      const text = e.text();
      return { pages, digests, before, after, textLen: Array.from(text).length,
               layoutDiff: (await D.auditLayout()).diff };`);
    check('跨页的表至少两页', r.pages >= 2, String(r.pages));
    check('每一页的位图都与从零光栅相同', r.digests.every((d: any) => d.diff === 0),
          JSON.stringify(r.digests));
    check('重配一次（同档）帧指纹不变', r.before === r.after, `${r.before} vs ${r.after}`);
    check('增量排版 == 全量', r.layoutDiff === 0, String(r.layoutDiff));
    check('文本没被切片改掉', r.textLen > 2000, String(r.textLen));
  });

  // 控件是格子里的行内对象：连点两下选值、第三下落到控件上时，那一下表达的仍然是
  //   "再点一次这个选项"。连击计数不能穿透过去变成"我要这一格"。
  test('格内控件连点三次：只切换取值，不整格选中', async ({ open }) => {
    test.setTimeout(120_000);
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      const e = ${D};
      await e.setText('首段');
      await e.setCaret({ para: 0, offset: 2 });
      await e.flush();
      await e.insertTable({ rows: 1, cols: 2 });
      await e.flush();
      await e.setCaret({ para: 1, offset: 0 });
      await e.flush();
      await H.insertControl({ type: 'radioButton', key: 'r1', label: '甲' });
      await e.flush();
      await D.stopBlink();
      const cv = FRAME();
      const rect = cv.getBoundingClientRect();
      const q = D.controls().items[0].rects[0];
      return { left: rect.left, top: rect.top, sx: rect.width / cv.width,
               sy: rect.height / cv.height, zoom: ZOOM(),
               qx: q.x, qy: q.y, qw: q.w, qh: q.h,
               flag: !!H.form()[0].value.flag,
               cells: (await H.tableState()).selCells };`);
    check('前置：控件在格里且有矩形', geo.qw > 0 && geo.cells === 0, JSON.stringify(geo));

    const at = {
      x: geo.left + (geo.qx + geo.qw / 2) * geo.zoom * geo.sx,
      y: geo.top + (geo.qy + geo.qh / 2) * geo.zoom * geo.sy,
    };
    for (let i = 0; i < 3; i++) {
      await p.page.mouse.click(at.x, at.y);
      await p.page.waitForTimeout(60);
    }
    await p.page.waitForTimeout(200);

    const after = await p.read<any>(`
      const e = ${D};
      const ts = await H.tableState();
      return { flag: !!H.form()[0].value.flag, cells: ts.selCells,
               sel: H.selection().empty, text: e.text() };`);
    check('★ 连点三次没有把整格选中（第三下落在控件上）', after.cells === 0,
          JSON.stringify(after));
    check('★ 三次切换一次不落（单选从关到开、关、再开）', after.flag === true,
          JSON.stringify(after));
  });

  // 格内插图之后敲字：字必须真的画在那一格里。三份自审可能同错（TRAPS 210），所以这一条
  //   直接数像素：图片下沿之下那块区域里必须数得出墨。
  test('格内插图后敲字：字画在格子里，同一行另一格也画得对', async ({ open }) => {
    test.setTimeout(120_000);
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const e = ${D};
      await e.setText('首段');
      await e.setCaret({ para: 0, offset: 2 });
      await e.flush();
      await e.insertTable({ rows: 1, cols: 2 });
      await e.flush();
      await e.setCaret({ para: 1, offset: 0 });
      await e.flush();
      const data = new Uint8Array(32 * 16 * 4).fill(180);
      await H.registerImage({ id: 'pic', width: 32, height: 16, data });
      await H.insertImage({ resourceId: 'pic', pxWidth: 32, pxHeight: 16 });
      await e.flush();
      // 光标此刻在图片之后：接着敲的字落在同一段、图片右边。
      await e.type('戊己庚辛壬癸子丑寅卯');
      await e.flush();
      await e.setCaret({ para: 2, offset: 0 });
      await e.type('右格的字');
      await e.flush();
      await D.stopBlink();

      const box = (await H.images())[0];
      const q = box.rects[0];
      const Z = ZOOM();
      // 页画布是逐页一张（帧只是它们的读视图），像素要从页画布上取。
      const pageCanvas = document.querySelector('.heditor-pages canvas[data-page="0"]');
      // 一块区域里"墨"的像素数：深色像素才算，白纸与浅底纹不算。
      const inkIn = (xPt, yPt, wPt, hPt) => {
        const c = document.createElement('canvas');
        c.width = Math.max(1, Math.round(wPt * Z));
        c.height = Math.max(1, Math.round(hPt * Z));
        const g2 = c.getContext('2d');
        g2.drawImage(pageCanvas, Math.round(xPt * Z), Math.round(yPt * Z), c.width, c.height,
                     0, 0, c.width, c.height);
        const d = g2.getImageData(0, 0, c.width, c.height).data;
        let n = 0;
        for (let k = 0; k < d.length; k += 4) if (d[k] < 160) ++n;
        return n;
      };
      const below = inkIn(q.x, q.y + q.h, q.w + 60, 40);
      const other = inkIn(q.x + q.w + 80, q.y, 60, 30);
      return { text: e.paragraph(1), otherText: e.paragraph(2), below, other,
               pages: e.geometry().pages, layoutDiff: (await D.auditLayout()).diff };`);
    check('前提：格内那一段有图也有字', r.text.includes('戊己庚辛') && r.layoutDiff === 0,
          JSON.stringify({ text: r.text, diff: r.layoutDiff }));
    check('★ 图片下沿之下数得出墨（字真的画出来了）', r.below > 20, String(r.below));
    check('★ 同一行另一格的字也在（没被这一格挤掉）', r.other > 10,
          JSON.stringify({ other: r.other, text: r.otherText }));
  });

  // 3x3 表格：第一格插图并缩小到比格宽窄，第二格连续输入 —— 折到第二行之后后面的字
  //   必须仍画得出来。与上一条的区别是"图在一格、字敲在另一格"：同一表格行里两格
  //   互相牵连，行高由较高的那格结算，于是两格的行盒会纵向重叠。
  test('3x3 表格：左格缩放过的图、右格连续输入折行', async ({ open }) => {
    test.setTimeout(180_000);
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const e = ${D};
      await e.setText('首段');
      await e.setCaret({ para: 0, offset: 2 });
      await e.flush();
      await H.insertTable({ rows: 3, cols: 3 });
      await e.flush();
      const ts0 = await H.tableState();
      // 第一格（表格第一行第一列的那一段）
      await e.setCaret({ para: 1, offset: 0 });
      await e.flush();
      const data = new Uint8Array(32 * 16 * 4).fill(180);
      await H.registerImage({ id: 'pic', width: 32, height: 16, data });
      await H.insertImage({ resourceId: 'pic', pxWidth: 32, pxHeight: 16 });
      await e.flush();
      // 拖把手缩小：宿主松手发的就是这一次 resizeImage。
      await H.selectImage(0);
      await e.resizeImage(0, 60, 30);
      await e.flush();
      const box = (await H.images())[0];
      // 第二格（表格第一行第二列）：段号 = 第一格之后的那一段。
      await e.setCaret({ para: 2, offset: 0 });
      await e.flush();
      const keys = 'asdfa asdf a a asdf asdf asdf asf asf as asdf asdf';
      const first = [];
      for (const ch of keys) {
        const ok = await H.type(ch);
        const st = D.editStats() || {};
        first.push({ ok: !!ok, full: !!st.layoutWasFull, relaid: st.relaidParagraphs,
                     glyphs: st.glyphs, lines: st.lines });
      }
      await e.flush();
      await D.stopBlink();
      const pz = D.pagePx();
      const Z = ZOOM();
      const pageCanvas = document.querySelector('.heditor-pages canvas[data-page="0"]');
      const inkIn = (xPt, yPt, wPt, hPt) => {
        const c = document.createElement('canvas');
        c.width = Math.max(1, Math.round(wPt * Z));
        c.height = Math.max(1, Math.round(hPt * Z));
        const g2 = c.getContext('2d');
        g2.drawImage(pageCanvas, Math.round(xPt * Z), Math.round(yPt * Z), c.width, c.height,
                     0, 0, c.width, c.height);
        const d = g2.getImageData(0, 0, c.width, c.height).data;
        let n = 0;
        for (let k = 0; k < d.length; k += 4) if (d[k] < 160) ++n;
        return n;
      };
      const q = box.rects[0];
      const cw = ts0.widthPt > 0 ? ts0.widthPt / ts0.cols : 0;
      const cell2x = q.x + cw;
      const cell2 = inkIn(cell2x, q.y, Math.max(20, cw - 4), 90);
      const cell2row2 = inkIn(cell2x, q.y + 22, Math.max(20, cw - 4), 30);
      const ts = await H.tableState();
      const lay = await D.auditLayout();
      const page = await D.auditPage(0);
      return { tableWidth: ts0.widthPt, cols: ts0.cols, row: ts.row, cell2, cell2row2,
               img: { x: q.x, y: q.y, w: q.w, h: q.h },
               text1: e.paragraph(1), text2: e.paragraph(2),
               layoutDiff: lay.diff, firstLine: lay.first, pageDiff: page.diff,
               pageBox: page.box, pagePx: [page.w, page.h], zoom: Z,
               dirty: D.pagesWrittenThisFrame(), steps: first.length,
               fulls: first.filter((s) => s.full).length,
               lastSteps: first.slice(-4) };`);
    check('前置：3x3、图缩到比格宽窄、第二格有字',
          r.cols === 3 && r.img.w < 100 && r.text2.length > 20, JSON.stringify(r.lastSteps));
    check('★ 增量排版与全量一致', r.layoutDiff === 0,
          JSON.stringify({ diff: r.layoutDiff, first: r.firstLine, text2: r.text2 }));
    check('★ 页位图与重光栅一致', r.pageDiff === 0,
          JSON.stringify({ pageDiff: r.pageDiff, box: r.pageBox, dirty: r.dirty }));
    check('★ 第二格整个格子里数得出墨', r.cell2 > 40, String(r.cell2));
    check('★ 第二行位置上也有墨（字画在第二行）', r.cell2row2 > 8,
          JSON.stringify({ row2: r.cell2row2, cell: r.cell2, img: r.img, tw: r.tableWidth }));
    check('★ 每次击键都是增量重排', r.fulls === 0, String(r.fulls));
  });
});

test.describe('表格拖拽', () => {
  test('表格：栅格查得出来，拖列宽只换两列之间的地方', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const pre = await p.read<any>(`
      await H.setText('表前');
      await H.flush();
      await H.insertTable({ rows: 2, cols: 2 });
      await H.flush();
      // 光标进第一格：格几何从 tableGrid 拿。
      await H.setCaret({ para: 1, offset: 0 });
      await H.flush();
      const box = await H.images();
      const q = CARET();
      const Z = ZOOM();
      const grid = await H.tableGrid(q.x / Z + 4, q.y / Z + 4);
      return { grid, w0: grid.cols[1] - grid.cols[0], table: grid.table, before: await H.text() };`);
    check('前置：栅格查得出来（两列三条边界）',
          pre.grid.ok === true && pre.grid.cols.length === 3, JSON.stringify(pre.grid));

    const res = await p.read<any>(`
      const g = ${JSON.stringify(pre.grid)};
      const w = g.w;
      await H.setTableColWidths(g, [w * 0.25, w * 0.75]);
      await H.flush();
      const again = await H.tableGrid(g.cols[0] + g.x + 2, g.rows[0].y + 2);
      const end = await H.tableGrid(g.x + g.w - 2, g.rows[0].y + 2);
      return { again, end, w };`);
    const c = res.again.cols;
    check('★ 拖完之后第一条内部边界挪到 1/4 处',
          Math.abs(c[1] - c[0] - Number(res.w) * 0.25) < 1.0,
          JSON.stringify(c));
    check('★ 表宽没变（相邻两列和不变）',
          Math.abs(c[2] - c[0] - Number(res.w)) < 1.0,
          `表宽 ${res.w} → ${c[2] - c[0]}`);

    // 反向：列数对不上要拒绝（多一条少一条都会把表排歪）。
    const bad = await p.read<any>(`
      const g = ${JSON.stringify(pre.grid)};
      let threw = false;
      try { await H.setTableColWidths(g, [100]); } catch (e) { threw = true; }
      return threw;`);
    check('列宽数量与栅格不符时拒绝', bad === true, String(bad));

    // 行高：行记录的 heightPt 落成"不小于"。
    const row = await p.read<any>(`
      await H.setTableRow({ heightPt: 60, heightRule: 'atLeast' });
      await H.flush();
      const g = ${JSON.stringify(pre.grid)};
      const again = await H.tableGrid(g.x + 4, g.rows[0].y + 4);
      return again.rows[0].h;`);
    check('★ 行高钉到 60 磅之后，这一页量出来也不小于 60',
          Number(row) >= 59.0, String(row));
  });

  test('拖表格的右边框与底边框：加宽最后一列、加高最后一行', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1200, height: 900 } });
    const geo = await p.read<any>(`
      await H.setText('表前');
      await H.flush();
      await H.insertTable({ rows: 2, cols: 2 });
      await H.flush();
      await H.setCaret({ para: 1, offset: 0 });
      await H.flush();
      const q = CARET();
      const Z = ZOOM();
      const g = await H.tableGrid(q.x / Z + 4, q.y / Z + 4);
      const fr = FRAME().getBoundingClientRect();
      await D.stopBlink();
      return { g, left: fr.left, top: fr.top, z: Z };`);
    const g = geo.g;
    check('前置：两列三条边界', g.cols.length === 3, JSON.stringify(g.cols));

    // 右边框：拖它加宽最后一列，表宽跟着走。
    const dxPt = 60 / geo.z;
    const rightX = geo.left + (g.x + g.w) * geo.z;
    const midY = geo.top + (g.rows[0].y + g.rows[0].h / 2) * geo.z;
    await p.page.mouse.move(rightX, midY);
    await p.page.mouse.down();
    await p.page.mouse.move(rightX + 60, midY, { steps: 6 });
    await p.page.mouse.up();
    await p.page.waitForTimeout(320);
    const wide = await p.read<any>(`
      const g = ${JSON.stringify(g)};
      return H.tableGrid(g.x + 4, g.rows[0].y + 4);`);
    check('★ 拖右边框：表宽变宽了（Word 同一条）',
          Math.abs(wide.w - (g.w + dxPt)) < 2.0, `${g.w} → ${wide.w}，期望 ${g.w + dxPt}`);
    check('★ 拖右边框：只有最后一列变宽，第一列不动',
          Math.abs((wide.cols[1] - wide.cols[0]) - (g.cols[1] - g.cols[0])) < 1.0 &&
            Math.abs((wide.cols[2] - wide.cols[1]) - (g.cols[2] - g.cols[1] + dxPt)) < 2.0,
          JSON.stringify(wide.cols) + ' vs ' + JSON.stringify(g.cols));

    // 底边框：拖它加高最后一行。
    const dyPt = 40 / geo.z;
    const last = wide.rows[wide.rows.length - 1];
    const botY = geo.top + (last.y + last.h) * geo.z;
    const midX = geo.left + (wide.x + wide.w / 2) * geo.z;
    await p.page.mouse.move(midX, botY);
    await p.page.mouse.down();
    await p.page.mouse.move(midX, botY + 40, { steps: 6 });
    await p.page.mouse.up();
    await p.page.waitForTimeout(320);
    const tall = await p.read<any>(`
      const g = ${JSON.stringify(wide)};
      return H.tableGrid(g.x + 4, g.rows[0].y + 4);`);
    check('★ 拖底边框：最后一行变高了',
          tall.rows[tall.rows.length - 1].h >= last.h + dyPt - 2.0,
          `${last.h} → ${tall.rows[tall.rows.length - 1].h}，期望 +${dyPt}`);
  });

  test('压在表格内部格线上换光标：列边界 col-resize、行边界 row-resize', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1200, height: 900 } });
    const geo = await p.read<any>(`
      await H.setText('甲乙丙丁');
      await H.setCaret({ para: 0, offset: 4 });
      await H.flush();
      await H.insertTable({ rows: 2, cols: 3 });
      await H.flush();
      for (const pg of [1, 2, 3]) {
        await H.setCaret({ para: pg, offset: 0 });
        await H.flush();
        await H.type('X');
        await H.flush();
      }
      await H.setCaret({ para: 1, offset: 1 });
      await H.flush();
      const c = CARET();
      const z = ZOOM();
      const docY = (c.page * Math.round(H.geometry().page.h) + c.y) / z;
      const grid = await H.tableGrid(c.x / z, docY);
      const fr = FRAME().getBoundingClientRect();
      return { grid, z, left: fr.left, top: fr.top };`);
    const g = geo.grid;
    check('拿到表格栅格', !!g && g.ok === true, JSON.stringify(g));
    if (!g || !g.ok) return;
    const colX = geo.left + (g.x + g.cols[1]) * geo.z;
    const rowY = geo.top + (g.rows[0].y + g.rows[0].h) * geo.z;
    await p.page.mouse.move(colX - 1, geo.top + (g.rows[0].y + g.rows[0].h / 2) * geo.z);
    await p.page.waitForTimeout(160);
    const colCur = await p.read<any>(`return EL('.heditor-pages').style.cursor;`);
    check('★ 压在列边界上：光标变成 col-resize', colCur === 'col-resize', colCur);
    await p.page.mouse.move(colX - 40, rowY - 1);
    await p.page.waitForTimeout(160);
    const rowCur = await p.read<any>(`return EL('.heditor-pages').style.cursor;`);
    check('★ 压在行边界上：光标变成 row-resize', rowCur === 'row-resize', rowCur);
    await p.page.mouse.move(colX - 40, rowY - 12);
    await p.page.waitForTimeout(160);
    const textCur = await p.read<any>(`return EL('.heditor-pages').style.cursor;`);
    check('格子里还是文本光标（不是全页都变成拖拽形）', textCur === 'text', textCur);
  });
});
