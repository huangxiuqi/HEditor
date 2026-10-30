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
        framePx: D.frameW() * D.frameH(),
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

  test('★ 单段文档打字同样只重贴一行（已知缺口，红了就是它）', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });

    // 单段文档是"新建之后开始写"的那条路：整篇只有一段，段落级重排等于整篇重排。
    //   重排全量本身不算错（一段就是全篇），错的是脏矩形跟着退化成整页 ——
    //   页上只有一行有字，却要把 89 万像素重新 putImageData 一遍。
    //   多段文档没有这个问题（见上一条：脏矩形落在 104 行像素上）。
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
