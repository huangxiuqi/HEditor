// 表格判据：跨页切片之后，续排片段必须仍然是"点得到、选得中、画得对"的一片。
//
// 为什么单独一份：切片是排版层唯一一处"同一条行盒换到另一页"的地方，行盒、字形与
//   格框三者的页号必须一起走。它错了的时候命令全部返回成功、增量与全量自审也全绿
//   （两边共用同一份错），只有"点一下、看一眼像素"能发现 —— 这一份就按这两件事写。
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
      for (const c of [c1, c2]) hits.push(await D.hit(c.x + 40, yDoc));
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
});
