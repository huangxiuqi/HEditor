// 上屏面：脏区重贴的范围、设备像素比、缩放下的点击落点。
//
// 这一域之前是空的。引擎侧有 200 多条呈现断言（native），但那些停在"光栅函数往 Bitmap
//   里写了字节"。从字节到画布隔着四处 —— wasm 边界拷贝、Worker 往返、putImageData 与视口
//   换算、主画布合成 —— 每一处都能让"引擎说画了"变成"屏幕上没动"或者"整页白搬"。
//
// 三条判据各钉一处：重贴范围（增量更新）、DPR 换算、缩放换算。它们的共同点是换个 DPR 或
//   换个缩放倍数结论就会变，所以只能在浏览器里判。

import { check } from '../support/check';
import { test } from '../support/probe';

test.describe('上屏：脏区、设备像素比、缩放', () => {
  test('★ 打字只重贴脏区，不是整页', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });

    // 两臂都要有同一把尺子：光标移动（几十像素）与滚动（整窗）——
    //   只用"打字很小"判不出读数是不是恒为 0，只用"滚动很大"判不出打字有没有整页搬。
    const r = await p.read(`
      await LOAD.text('甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥', 24);
      await H.setCaret({ para: 0, offset: 4 });
      await H.flush();
      const idle = D.blitStats();
      await TYPE('字');
      await H.flush();
      const typed = D.blitStats();
      const g = H.geometry();
      return {
        idle: { px: idle.px, pages: idle.pages },
        typed: { px: typed.px, pages: typed.pages },
        pagePx: g.page.w * g.page.h,
        linePx: g.page.w * Math.max(1, Math.round(g.page.h / 30)),
      };
    `);

    // 打字改动的是当前行：重贴的范围该以行为量级，而不是整页。
    //   门限取页高的十分之一（约三行），宽到能容纳"这一行 + 上下各一行受行距影响"，
    //   窄到抓得住整页搬（那会超出三十倍）。
    check('★ 打字重贴的像素远小于整页',
      r.typed.px > 0 && r.typed.px < r.pagePx / 10,
      `打字 ${r.typed.px} / 页 ${r.pagePx}（${(r.typed.px / r.pagePx * 100).toFixed(1)}%）`);
    check('★ 打字重贴的页数是一页（不是整窗）',
      r.typed.pages === 1, `pages=${r.typed.pages}`);
    check('光标未动时的那一拍几乎不贴（反向臂：读数不是恒为整页）',
      r.idle.px < r.pagePx / 10, `idle ${r.idle.px} / 页 ${r.pagePx}`);
  });

  test('★ 单段文档打字同样只重贴一行', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });

    // 单段文档是"新建之后开始写"的那条路：整篇只有一段，段落级重排等于整篇重排。
    //   重排全量本身不算错（一段就是全篇），错的是脏矩形跟着退化成整页 ——
    //   页上只有一行有字，却要把 89 万像素重新 putImageData 一遍。
    //   多段文档没有这个问题（见上一条：脏矩形落在 104 行像素上）。
    //   行带的上界必须止于这一组自己的墨迹：首组拉到页顶会让单行页整页重画。
    //   新旧两拍的带取并集，所以"行变高、旧墨在上方"那一档仍被覆盖。
    const r = await p.read(`
      await LOAD.text('甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥', 1);
      await H.setCaret({ para: 0, offset: 4 });
      await D.stopBlink();
      await H.flush();
      await TYPE('字');
      await H.flush();
      const b = D.blitStats();
      const g = H.geometry();
      const st = D.editStats() || {};
      return { px: b.px, pages: b.pages, pagePx: g.page.w * g.page.h,
               relaidLines: st.relaidLines, wasFull: st.layoutWasFull };
    `);

    check('★ 单段文档打字不整页搬',
      r.px > 0 && r.px < r.pagePx / 10,
      `贴了 ${r.px} / 页 ${r.pagePx}（${(r.px / r.pagePx * 100).toFixed(1)}%，` +
      `重排 ${r.relaidLines} 行，全量=${r.wasFull}）`);
  });

  test('★ 裁剪标记只画四个角，关掉就消失', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });

    // 标记标的是正文可用区（页边距的内沿），所以它必须是四个角的短线，不是一整圈框；
    //   关掉之后缓存要按光栅签名作废，墨必须真的消失。
    //   取样只取页边距那一侧的两条臂：第一个字就贴在内容框左上角，伸进正文会把字算成标记。
    const r = await p.read(`
      await LOAD.text('甲', 1);
      await H.configure({ marginMarks: true });
      await H.flush();
      const pm = await D.pageMetrics();
      // 读的是页画布，不是引擎的帧：帧是被测对象的自述，只能证明"光栅器认为画对了"。
      //   画布跨了 wasm 拷贝、Worker 传递、putImageData 三层，才是用户看见的那一层。
      const grab = async () => {
        // 呈现挂在 rAF 上：不等两帧就读，拿到的是上一拍的画布 ——
        //   而 selfCheck 比的是"画布 vs 引擎页缓存"，两边都旧时差额也是 0，判据会假绿。
        await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
        // selfCheck 先把欠着的上屏贴完再比（它内部调 blitPages），顺带给出
        //   "画布 vs 引擎页像素"的差额：差额 0 才说明画布确实是最新那一拍。
        const sc = await D.selfCheck();
        const el = [...EL('.heditor-pages').querySelectorAll('canvas')]
          .filter((c) => c.dataset.page === '0')[0];
        const t = document.createElement('canvas');
        t.width = el.width;
        t.height = el.height;
        const g2 = t.getContext('2d');
        g2.drawImage(el, 0, 0);
        const d = g2.getImageData(0, 0, el.width, el.height).data;
        const fw = el.width;
        const fh = el.height;
        const k = fw / pm.widthPt;              // 设备像素 / 磅
        const m = pm.margins.top * k;           // 上边距（四边同一档）
        const len = 18 * k;                     // kMarginMarkPt = 18 磅
        const dark = (x0, y0, x1, y1) => {
          let n = 0;
          for (let y = Math.max(0, Math.floor(y0)); y < Math.min(fh, Math.ceil(y1)); y++) {
            for (let x = Math.max(0, Math.floor(x0)); x < Math.min(fw, Math.ceil(x1)); x++) {
              if (d[(y * fw + x) * 4] < 200) n++;
            }
          }
          return n;
        };
        const h = (x, y, w) => dark(x, y, x + w, y + 1);
        const v = (x, y, hh) => dark(x, y, x + 1, y + hh);
        const x1 = fw - m;
        const y1 = fh - m;
        const cx = fw / 2;
        const cy = fh / 2;
        return {
          diff: sc && sc.screenVsEngineDiffPx,
          // 取样严格在内容框外侧（到 m-1 为止）：第一个字就贴在内容框左上角，
          //   窗口只要含 m 这一列/行，关掉标记后残留的就是字的顶横、不是标记。
          corners: [
            h(m - len, m, len - 1) + v(m, m - len, len - 1),
            h(x1 + 1, m, len - 1) + v(x1, m - len, len - 1),
            h(m - len, y1, len - 1) + v(m, y1, len - 1),
            h(x1 + 1, y1, len - 1) + v(x1, y1, len - 1),
          ],
          edges: dark(cx - 8, m - 2, cx + 8, m + 8) + dark(cx - 8, y1 - 8, cx + 8, y1 + 2) +
                 dark(m - 2, cy - 8, m + 8, cy + 8) + dark(x1 - 8, cy - 8, x1 + 2, cy + 8),
        };
      };
      const on = await grab();
      await H.configure({ marginMarks: false });
      await H.flush();
      const off = await grab();
      return { on, off };
    `);

    check('画布与引擎页像素一致（差额 0）',
      r.on.diff === 0 && r.off.diff === 0,
      `on ${r.on.diff} / off ${r.off.diff}`);
    check('★ 打开时四个角都有墨',
      r.on.corners.every((n: number) => n > 0), JSON.stringify(r.on.corners));
    check('★ 四条边的中段没有墨（标的是四角，不是一整圈框）',
      r.on.edges === 0, String(r.on.edges));
    check('★ 关掉之后四角的墨消失（页位图缓存按签名作废）',
      r.off.corners.every((n: number) => n === 0), JSON.stringify(r.off.corners));

    // 连页没有页，四角没有意义：开与关必须给出同一张帧（差分判，绝对值分不清
    //   "标记没画"与"语料本来有墨"）。
    const cont = await p.read(`
      await H.configure({ pageMode: 'continuous', marginMarks: true });
      await H.flush();
      const grabCv = async () => {
        await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
        const el = [...EL('.heditor-pages').querySelectorAll('canvas')]
          .filter((c) => c.dataset.page === '0')[0];
        const t = document.createElement('canvas');
        t.width = el.width;
        t.height = el.height;
        const g2 = t.getContext('2d');
        g2.drawImage(el, 0, 0);
        return Array.from(g2.getImageData(0, 0, el.width, el.height).data);
      };
      const onF = await grabCv();
      await H.configure({ marginMarks: false });
      await H.flush();
      const offF = await grabCv();
      let diff = 0;
      const n = Math.min(onF.length, offF.length);
      for (let i = 0; i < n; i++) if (onF[i] !== offF[i]) { diff++; }
      return { diff, sameSize: onF.length === offF.length };
    `);
    check('★ 连页模式下不画裁剪标记（开与关同一张画布）',
      cont.sameSize && cont.diff === 0, `差 ${cont.diff} 字节 / 同尺寸=${cont.sameSize}`);
  });

  test('★ 同一份文档两次冷启动，光栅逐字节相同', async ({ open }) => {
    // 光栅必须确定：字形 strike、预热、共享 typeface 这些隐藏状态只能影响命不命中，
    //   不能影响画出来的像素。两次冷启动各起一个引擎，比整帧的摘要。
    const shot = async (): Promise<{ hash: number; w: number; h: number }> => {
      const p = await open('index.html', { query: { debug: '1' } });
      return p.read(`
        await LOAD.text('甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥', 8);
        await D.stopBlink();
        await H.flush();
        const F = await PIX();
        const f = F.data;
        let h = 2166136261 >>> 0;
        for (let i = 0; i < f.length; i++) { h ^= f[i]; h = Math.imul(h, 16777619) >>> 0; }
        return { hash: h >>> 0, w: F.w, h: F.h };
      `);
    };
    const a = await shot();
    const b = await shot();
    check('两次的帧尺寸一致', a.w === b.w && a.h === b.h, `${a.w}x${a.h} / ${b.w}x${b.h}`);
    check('★ 同一文档两次冷启动逐字节相同（没有隐藏状态泄漏到像素里）',
      a.hash === b.hash, `${a.hash} / ${b.hash}`);
  });

  test.describe('设备像素比 2', () => {
    test.use({ deviceScaleFactor: 2 });

    test('★ 点击落点与光标位置一致（DPR 换算整条链）', async ({ open }) => {
      const p = await open('index.html', { query: { debug: '1' } });

      // 光标放到第四个字之后，读它的设备像素矩形与帧在屏幕上的矩形。
      //   两者是两套坐标：帧内是设备像素，屏幕是 CSS 像素，中间差一个 dpr。
      const at = await p.read(`
        await LOAD.text('甲乙丙丁戊己庚辛壬癸', 1);
        await H.setCaret({ para: 0, offset: 4 });
        await D.stopBlink();
        await H.flush();
        const c = CARET();
        const f = FRAME().getBoundingClientRect();
        return { offset: c.offset, rect: { x: c.x, y: c.y, w: c.w, h: c.h },
                 frame: { left: f.left, top: f.top }, dpr: H.geometry().dpr,
                 zoom: H.config().zoom };
      `);
      check('设备像素比真的是 2', Math.abs(at.dpr - 2) < 0.01, `dpr=${at.dpr}`);

      // 点插入点右侧四分之一字宽处：落在同一个字里，落点必须还是这个偏移。
      const cssX = at.frame.left + (at.rect.x + at.rect.w * 0.25) / at.dpr;
      const cssY = at.frame.top + (at.rect.y + at.rect.h * 0.5) / at.dpr;
      await p.page.mouse.click(cssX, cssY);

      const back = await p.read(`
        await H.flush();
        const c = CARET();
        return { offset: c.offset, para: c.para, visible: c.visible };
      `);
      check('★ 点自己刚才的位置，落回同一个偏移',
        back.offset === at.offset && back.para === 0,
        `点 (${cssX.toFixed(1)}, ${cssY.toFixed(1)}) → offset ${back.offset}，期望 ${at.offset}`);
      check('光标可见（DPR 换算错了会落到版心外）', back.visible === true, String(back.visible));
    });
  });

  test.describe('设备像素比 2 且缩放 1.25', () => {
    test.use({ deviceScaleFactor: 2 });

    test('★ 缩放之后点击落点仍落在同一个字上', async ({ open }) => {
      const p = await open('index.html', { query: { debug: '1' } });

      // 缩放改的是密度而不是页度量：字变大了，设备像素矩形跟着放大，
      //   屏幕上的位置由"设备像素 ÷ dpr"给出 —— 两处换算错一处，落点就偏一格。
      const at = await p.read(`
        await H.configure({ zoom: 1.25 });
        await LOAD.text('甲乙丙丁戊己庚辛壬癸', 1);
        await H.setCaret({ para: 0, offset: 4 });
        await D.stopBlink();
        await H.flush();
        const c = CARET();
        const f = FRAME().getBoundingClientRect();
        return { offset: c.offset, rect: { x: c.x, y: c.y, w: c.w, h: c.h },
                 frame: { left: f.left, top: f.top }, dpr: H.geometry().dpr,
                 zoom: H.config().zoom };
      `);
      check('缩放真的落在 1.25', Math.abs(at.zoom - 1.25) < 0.01, `zoom=${at.zoom}`);

      const cssX = at.frame.left + (at.rect.x + at.rect.w * 0.25) / at.dpr;
      const cssY = at.frame.top + (at.rect.y + at.rect.h * 0.5) / at.dpr;
      await p.page.mouse.click(cssX, cssY);

      const back = await p.read(`
        await H.flush();
        const c = CARET();
        return { offset: c.offset, para: c.para, visible: c.visible };
      `);
      check('★ 缩放 1.25 下点同一处仍落回同一个偏移',
        back.offset === at.offset && back.para === 0,
        `点 (${cssX.toFixed(1)}, ${cssY.toFixed(1)}) → offset ${back.offset}，期望 ${at.offset}`);
      check('光标可见（缩放换算错了会落到版心外）', back.visible === true, String(back.visible));
    });
  });
});
