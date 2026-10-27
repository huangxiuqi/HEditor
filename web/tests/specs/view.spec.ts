// 视图判据：缩放之后画面停在同一处，连页接缝上的墨迹与页内的同形。
//
// 缩放同时改页面高度与窗口页数：宿主的滚动容器按像素记位置，引擎按页记窗口。
//   两侧只要有一侧按旧倍率算，表现都是画面跳回开头，而文档内容分毫未动 ——
//   所以那一组判据全落在滚动位置与窗口号上。
//
// 连页把一整条长纸切成页位图：跨接缝的那一行，两半分别画在相邻两页上。
//   两半要用同一个位移拼起来，位移只要不是整像素，压在最上面的那枚段落标记就会
//   被抗锯齿摊成两行浅灰 —— 看着比别的标记粗一档，而文字那半只是略微发虚。
import { check } from '../support/check';
import { test } from '../support/probe';

test.describe('缩放与滚动', () => {
  test('缩放停在原处：滚动位置按倍率折算，引擎窗口跟着走', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1400, height: 900 } });
    const r = await p.read<any>(`
      const lines = [];
      for (let i = 0; i < 120; i++) lines.push('第' + i + '段内容一二三四五六七八九十');
      await H.setText(lines.join('\\n'));
      await H.flush();
      const sc = EL('.heditor-stage');
      sc.scrollTop = 900;
      await new Promise((r) => setTimeout(r, 320));
      const before = { top: sc.scrollTop, off: EL('.heditor-pages').offsetTop,
                       h: EL('.heditor-pages').clientHeight };
      await H.setZoom(1.25);
      await H.flush();
      await new Promise((r) => setTimeout(r, 320));
      const zoomIn = { top: sc.scrollTop, off: EL('.heditor-pages').offsetTop,
                       h: EL('.heditor-pages').clientHeight, win: H.geometry().window };
      await H.setZoom(0.5);
      await H.flush();
      await new Promise((r) => setTimeout(r, 320));
      const zoomOut = { top: sc.scrollTop, win: H.geometry().window };
      return { before, zoomIn, zoomOut };`);
    // 折算的是"页顶之上那一段边距之下"的部分：上边距是固定 CSS 值，不随缩放变。
    const want = (r.before.top - r.before.off) * 1.25 + r.before.off;
    check('★ 放大后仍停在原来那一处（滚动位置按 1.25 折算）',
          Math.abs(r.zoomIn.top - want) <= 2, r.zoomIn.top + ' / 期望 ' + want);
    check('放大之后页面确实变高了（说明缩放真的生效）',
          r.zoomIn.h > r.before.h, r.before.h + ' → ' + r.zoomIn.h);
    const wantOut = (r.before.top - r.before.off) * 0.5 + r.before.off;
    check('★ 缩小后同样停在那一处（不是被拽回开头，也不是被夹到底）',
          Math.abs(r.zoomOut.top - wantOut) <= 2 ||
            Math.abs(r.zoomOut.top - (r.before.top - r.before.off) * 0.5) <= 2,
          r.zoomOut.top + ' / 期望 ' + wantOut);
    check('缩小之后窗口页数与总页数对得上（没有留下空窗）',
          r.zoomOut.win.count >= 1 && r.zoomOut.win.first >= 0, JSON.stringify(r.zoomOut.win));
  });
});

test.describe('连页接缝', () => {
  test('压在接缝上的段落标记只有一道横笔，与页内的同形', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1280, height: 900 } });
    const r = await p.read<any>(`
      await H.configure({ pageMode: 'continuous' });
      const lines = [];
      for (let i = 0; i < 80; i++) lines.push('第' + i + '段：连页接缝处的段落标记应当与页内的行同形。');
      await H.setText(lines.join(String.fromCharCode(10)));
      await H.flush();

      const cvOf = (page) => [...EL('.heditor-pages').querySelectorAll('canvas')]
        .filter((el) => el.dataset.page === String(page))[0];
      const grab = (el) => {
        const t = document.createElement('canvas');
        t.width = el.width;
        t.height = el.height;
        t.getContext('2d').drawImage(el, 0, 0);
        return t.getContext('2d').getImageData(0, 0, el.width, el.height);
      };
      // 一窄条里的行剖面：每一行有几个墨点。横笔那一行最宽（约一个字宽的六成）。
      const profile = (img, x0, x1, y0, y1) => {
        const rows = [];
        for (let y = y0; y < y1; y++) {
          let n = 0;
          for (let x = x0; x < x1; x++) {
            const i = (y * img.width + x) * 4;
            if ((img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3 < 235) ++n;
          }
          if (n > 0) rows.push({ y, n });
        }
        return rows;
      };
      const ph = cvOf(0).height;
      // 上边距一动，整篇就上下平移：总有一步让某一行的标记正好压在接缝上。
      //   步长要比命中窗口（标记高出页底的那几像素）小，才不会跳过去。
      let hit = null;
      for (let top = 60; top <= 92 && !hit; top += 4) {
        await H.configure({ page: { paper: 'A4', landscape: false,
                                    margins: { top, right: 72, bottom: 72, left: 72 } } });
        await H.flush();
        for (let i = 30; i < 38 && !hit; i++) {
          const len = (await H.paragraph(i)).length;
          await H.setCaret({ para: i, offset: len });
          await H.flush();
          const c = CARET();
          if (c.page !== 0) continue;
          // 命中条件：横笔与三角那一段已经落到下一页，页 1 顶部至少能看到半枚。
          if (c.y + 12 <= ph && c.y + 17 >= ph) hit = { para: i, x: c.x, y: c.y, top };
        }
      }
      if (!hit) return { hit: null, ph };

      // 跨缝那一枚落在下一页顶部的那一截；对照是同字号、整行在页内的另一枚。
      const seam = profile(grab(cvOf(1)), hit.x + 2, hit.x + 14, 0, 30);
      const len0 = (await H.paragraph(5)).length;
      await H.setCaret({ para: 5, offset: len0 });
      await H.flush();
      const c0 = CARET();
      const plain = profile(grab(cvOf(0)), c0.x + 2, c0.x + 14, c0.y + 4, c0.y + 24);
      const bars = (rows) => rows.filter((q) => q.n >= 6).length;
      const widest = (rows) => rows.reduce((m, q) => Math.max(m, q.n), 0);
      return { hit, ph, seam, plain, seamBars: bars(seam), plainBars: bars(plain),
               seamWide: widest(seam), plainWide: widest(plain), seamRows: seam.length };`);

    check('构造出"标记正好压在接缝上"的那一行（否则这条判据恒真）',
          r.hit !== null && r.ph > 0, JSON.stringify(r.hit));
    if (!r.hit) return;
    check('接缝那一截确实画在下一页上（至少半枚标记）',
          r.seamRows >= 4, '只有 ' + r.seamRows + ' 行，剖面 ' + JSON.stringify(r.seam));
    check('★ 接缝上那枚标记只有一道横笔（半像素位移会把它摊成两行浅灰）',
          r.seamBars === 1, '横笔行数 ' + r.seamBars + '，剖面 ' + JSON.stringify(r.seam));
    check('对照：页内的标记同样只有一道横笔', r.plainBars === 1, JSON.stringify(r.plain));
    check('接缝上的横笔与页内的一样长（没有被摊薄）',
          r.seamWide >= r.plainWide - 1, r.seamWide + ' vs ' + r.plainWide);
  });
});
