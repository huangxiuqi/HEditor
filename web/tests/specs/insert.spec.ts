// 插入判据：表格与内容控件都要真的进文档、落在该落的位置，并且画面跟着变。
//
// 为什么单独一份：这两条命令走的是 InsertNode 那条结构步骤，判据要同时钉住三件事 ——
//   结构对（段数、表格行列、控件区间）、落点对（光标进第一格 / 进控件内容末尾）、
//   以及"画面上真的看得见"（结构成功但没重画，命令返回 true、读回也对，只有像素能发现）。
import { createHash } from 'node:crypto';

import { check } from '../support/check';
import { expect, test } from '../support/probe';

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
        qx: q.x, qy: q.y, qw: q.w, qh: q.h,
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

    // 悬停把手上要换成缩放光标：这是纯输入层的行为，不需要按下。
    const cursorAt = async (xPt: number, yPt: number) => {
      const q = at(xPt, yPt);
      await p.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: q.x, y: q.y, buttons: 0 });
      await p.page.waitForTimeout(100);
      return p.page.evaluate(
        () => getComputedStyle(document.querySelector('.heditor-pages') as HTMLElement).cursor,
      );
    };
    const cursorCorner = await cursorAt(geo.qx, geo.qy);
    check('★ 悬停在角把手：光标变成对角缩放', cursorCorner === 'nwse-resize', cursorCorner);
    const cursorEdge = await cursorAt(geo.qx + geo.qw, geo.qy + geo.qh / 2);
    check('★ 悬停在右边把手：光标变成横向缩放', cursorEdge === 'ew-resize', cursorEdge);
    const cursorText = await cursorAt(geo.tx, geo.ty);
    check('★ 离开把手回到正文光标', cursorText === 'text', cursorText);

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

  test('拖右中把手：拖动中只画框，松手才改尺寸，撤销回到原尺寸且光标不动', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙丙丁戊己庚辛壬癸');
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
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               zoom: ZOOM(), qx: q.x, qy: q.y, qw: q.w, qh: q.h, w0: box.w,
               caret0: JSON.stringify(H.caret().pos) };
    `);
    const at = (xPt: number, yPt: number) => ({
      x: geo.left + xPt * geo.zoom * geo.sx,
      y: geo.top + yPt * geo.zoom * geo.sy,
    });
    const imgW = () => p.read<number>(`const b = (await H.images())[0]; return b.w;`);

    // 选中图片：把手才进瞬态层。
    const center = at(geo.qx + geo.qw / 2, geo.qy + geo.qh / 2);
    await p.page.mouse.click(center.x, center.y);
    await p.page.waitForTimeout(120);

    const h0 = at(geo.qx + geo.qw, geo.qy + geo.qh / 2 + 5);
    const h1 = at(geo.qx + geo.qw + 24, geo.qy + geo.qh / 2 + 5);
    await p.page.mouse.move(h0.x, h0.y);
    await p.page.waitForTimeout(100);
    await p.page.mouse.down();
    const during: number[] = [];
    for (let i = 1; i <= 4; i++) {
      await p.page.mouse.move(h0.x + ((h1.x - h0.x) * i) / 4, h0.y);
      await p.page.waitForTimeout(60);
      during.push(await imgW());
    }
    // 拖动期间文档里的尺寸一个都没动：只画框，松手才落地。
    check('★ 拖动过程中尺寸不变（只画预览框）',
          during.every((w) => Math.abs(w - geo.w0) < 0.01), JSON.stringify(during) + ' vs ' + geo.w0);
    const digestDuring = await p.read<string>(`return await D.frameDigest();`);
    await p.page.mouse.up();
    await p.page.waitForTimeout(250);
    const afterUp = await imgW();
    check('★ 松手之后尺寸才落地', afterUp > geo.w0 + 5, geo.w0 + ' → ' + afterUp);
    const digestAfter = await p.read<string>(`return await D.frameDigest();`);
    check('松手后画面又变了（框换成真尺寸）', digestAfter !== digestDuring,
          digestDuring + ' → ' + digestAfter);

    const undone = await p.read<any>(`
      await H.undo();
      await H.flush();
      const b = (await H.images())[0];
      return { w: b.w, caret: JSON.stringify(H.caret().pos) };`);
    check('★ 一次撤销回到拖动之前的尺寸', Math.abs(undone.w - geo.w0) < 0.01,
          undone.w + ' / 期望 ' + geo.w0);
    check('★ 撤销之后光标没有被甩到文档开头', undone.caret === geo.caret0,
          undone.caret + ' vs ' + geo.caret0);
  });

  test('大图：右中把手能拖、边框别处不能拖、选中时没有文本光标', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙丙丁');
      await H.setCaret({ para: 0, offset: 2 });
      await H.flush();
      const data = new Uint8Array(400 * 300 * 4).fill(180);
      await H.registerImage({ id: 'big', width: 400, height: 300, data });
      await H.insertImage({ resourceId: 'big', pxWidth: 400, pxHeight: 300 });
      await H.flush();
      await D.stopBlink();
      const box = (await H.images())[0];
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      const q = box.rects[0];
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               zoom: ZOOM(), qx: q.x, qy: q.y, qw: q.w, qh: q.h, w0: box.w,
               caretVisible0: H.caret().visible };`);
    check('前置：大图插进来了', geo.qw > 100 && geo.qh > 100, JSON.stringify(geo));
    check('前置：插完图光标是可见的', geo.caretVisible0 === true, String(geo.caretVisible0));

    const at = (xPt: number, yPt: number) => ({
      x: geo.left + xPt * geo.zoom * geo.sx,
      y: geo.top + yPt * geo.zoom * geo.sy,
    });
    const imgW = () => p.read<number>(`const b = (await H.images())[0]; return b.w;`);
    const dragBy = async (fromX: number, fromY: number, dxPt: number) => {
      const from = at(fromX, fromY);
      const to = at(fromX + dxPt, fromY);
      await p.page.mouse.move(from.x, from.y);
      await p.page.waitForTimeout(80);
      await p.page.mouse.down();
      await p.page.waitForTimeout(120);
      for (let i = 1; i <= 4; i++) {
        await p.page.mouse.move(from.x + ((to.x - from.x) * i) / 4, from.y);
        await p.page.waitForTimeout(60);
      }
      await p.page.mouse.up();
      await p.page.waitForTimeout(250);
    };

    // 选中：拖拽框与把手才进瞬态层。
    const center = at(geo.qx + geo.qw / 2, geo.qy + geo.qh / 2);
    await p.page.mouse.click(center.x, center.y);
    await p.page.waitForTimeout(150);
    const selected = await p.read<any>(`
      return { visible: H.caret().visible, sel: H.caret().pos };`);
    // 光标那条竖线正落在图的右边线上，留着只会盖住把手 —— 选中图片时不画。
    check('★ 选中图片时不画文本光标', selected.visible === false, String(selected.visible));

    // 边框线（离把手很远的那一段）拖不动：命中区就是画出来的那个方块。
    await dragBy(geo.qx + geo.qw, geo.qy + 20, 30);
    const afterEdge = await imgW();
    check('★ 拖右边框线（离把手远处）不改尺寸', Math.abs(afterEdge - geo.w0) < 0.01,
          geo.w0 + ' → ' + afterEdge);

    // 右中把手：离边线偏移 1 磅也能拖（把手是方块，不沿边线拉伸）。
    await dragBy(geo.qx + geo.qw, geo.qy + geo.qh / 2, 40);
    const afterHandle = await imgW();
    check('★ 拖右中把手改了尺寸', afterHandle > geo.w0 + 20, geo.w0 + ' → ' + afterHandle);
    const undone = await p.read<any>(`
      await H.undo();
      await H.flush();
      return { w: (await H.images())[0].w };`);
    check('撤销回到原尺寸', Math.abs(undone.w - geo.w0) < 0.01, undone.w + ' / ' + geo.w0);
  });

  test('光标压在右中把手上：重合处照样能拖（透明承接层不参与命中）', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 小图占满行高，光标那条竖线正好落在右中把手上 —— 这是"光标所在处拖不动"的原样。
    //   承接层按"从光标到页右、一行高"摆放，opacity:0 挡不住命中：那一片按下会被它接走，
    //   画布这侧收不到，于是把手在那条带子里失效。判据直接看命中层，不只看尺寸变没变。
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
      const c = H.caret();
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      const q = box.rects[0];
      // 光标矩形是设备像素，折成磅才与图片的页面坐标同一把尺子。
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               zoom: ZOOM(), qx: q.x, qy: q.y, qw: q.w, qh: q.h, w0: box.w,
               cx: c.rect.x / ZOOM(), cy: c.rect.y / ZOOM(), ch: c.rect.h / ZOOM() };`);
    const at = (xPt: number, yPt: number) => ({
      x: geo.left + xPt * geo.zoom * geo.sx,
      y: geo.top + yPt * geo.zoom * geo.sy,
    });
    const hx = geo.qx + geo.qw;
    const hy = geo.qy + geo.qh / 2;
    check('前置：光标确实压在右中把手上',
          Math.abs(hx - geo.cx) < 4 && hy > geo.cy && hy < geo.cy + geo.ch,
          JSON.stringify({ hx, hy, cx: geo.cx, cy: geo.cy, ch: geo.ch }));

    const onHandle = at(hx, hy);
    const topmost = await p.page.evaluate(({ x, y }) => {
      const el = document.elementFromPoint(x, y);
      const ime = document.querySelector('.heditor-ime');
      return { tag: el ? el.tagName : '(null)', cls: el ? String(el.className) : '',
               isIme: !!ime && el === ime };
    }, onHandle);
    check('★ 重合处最上层是画布，不是承接层',
          topmost.tag === 'CANVAS' && !topmost.isIme, JSON.stringify(topmost));

    await p.page.mouse.move(onHandle.x, onHandle.y);
    await p.page.waitForTimeout(120);
    const hover = await p.page.evaluate(
      () => getComputedStyle(document.querySelector('.heditor-pages') as HTMLElement).cursor);
    check('★ 重合处悬停仍是横向缩放光标', hover === 'ew-resize', hover);

    const center = at(geo.qx + geo.qw / 2, geo.qy + geo.qh / 2);
    await p.page.mouse.click(center.x, center.y);
    await p.page.waitForTimeout(150);
    await p.page.mouse.move(onHandle.x, onHandle.y);
    await p.page.waitForTimeout(80);
    await p.page.mouse.down();
    await p.page.waitForTimeout(120);
    for (let i = 1; i <= 4; i++) {
      await p.page.mouse.move(onHandle.x + (40 * geo.zoom * geo.sx * i) / 4, onHandle.y);
      await p.page.waitForTimeout(60);
    }
    await p.page.mouse.up();
    await p.page.waitForTimeout(250);
    const dragged = await p.read<number>(`const b = (await H.images())[0]; return b.w;`);
    check('★ 从重合处起拖：尺寸跟着变大', dragged > geo.w0 + 10, geo.w0 + ' → ' + dragged);
    const undone = await p.read<any>(`
      await H.undo();
      await H.flush();
      return { w: (await H.images())[0].w };`);
    check('撤销回到原尺寸', Math.abs(undone.w - geo.w0) < 0.01, undone.w + ' / ' + geo.w0);
  });

  test('图片进单元格：按格宽限宽，不横穿表格', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.flush();
      await e.insertTable({ rows: 1, cols: 2 });
      await e.flush();
      // 光标进第一格，插一张"按版心宽度"的大图（宿主就是这么算的）：版心比格宽得多。
      await e.setCaret({ para: 1, offset: 0 });
      await e.flush();
      const g = e.geometry();
      const page = e.config().page;
      const textW = g.page.w * 0.75 - page.margins.left - page.margins.right;
      const w = 400;
      const h = 200;
      await e.registerImage({ id: 'big', width: w, height: h, data: new Uint8Array(w * h * 4).fill(200) });
      const ok = await e.insertImage({
        resourceId: 'big', pxWidth: w, pxHeight: h, width: Math.min((w * 72) / 96, textW),
      });
      await e.flush();
      const box = (await e.images())[0];
      const ts = await e.tableState();
      // 格宽 = 表宽 / 列数（没给表宽时表撑满版心），再减两侧内边距。
      const cellW = ts.widthPt > 0 ? ts.widthPt / ts.cols : 0;
      return { ok, box, textW, cellW };`);

    check('图片插进去了', r.ok === true, String(r.ok));
    check('★ 格里的图不宽于格（没有横穿表格）',
          r.box.w < r.textW * 0.8 &&
          (r.cellW <= 0 || r.box.w <= r.cellW),
          JSON.stringify({ w: r.box.w, cellW: r.cellW, textW: r.textW }));
    check('限宽之后保持高宽比', Math.abs(r.box.h / r.box.w - 0.5) < 0.02,
          String(r.box.h / r.box.w));
  });

  test('选中图片后按退格删的是图片，不是它前面的正文', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙丙丁戊己');
      await H.setCaret({ para: 0, offset: 4 });
      await H.flush();
      const w = 40;
      const h = 20;
      await H.registerImage({ id: 'p1', width: w, height: h,
                              data: new Uint8Array(w * h * 4).fill(190) });
      await H.insertImage({ resourceId: 'p1', pxWidth: w, pxHeight: h });
      await H.flush();
      // 光标挪到行首：选中图片之后按退格，删的必须是图片（顺着光标找左邻就删错东西）。
      await H.setCaret({ para: 0, offset: 0 });
      await H.flush();
      await D.stopBlink();
      const box = (await H.images())[0];
      const q = box.rects[0];
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      return { left: r.left, top: r.top, Z: ZOOM(), cx: q.x + q.w / 2, cy: q.y + q.h / 2,
               text0: (await H.paragraphs())[0] };`);
    await p.page.mouse.click(geo.left + geo.cx * geo.Z, geo.top + geo.cy * geo.Z);
    await p.page.waitForTimeout(150);
    await p.page.keyboard.press('Backspace');
    await p.page.waitForTimeout(250);
    const after = await p.read<any>(`
      return { images: (await H.images()).length, text: (await H.paragraphs())[0] };`);
    check('★ 退格删掉了选中的图片', after.images === 0, String(after.images));
    check('★ 图片前面的正文字一个没少', after.text === geo.text0,
          JSON.stringify({ before: geo.text0, after: after.text }));
  });
});

test.describe('插入控件', () => {
  test('八种类型、单选组切换与异步前置回调', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const prep = await p.read<any>(`
      await H.setText('甲乙丙丁');
      await H.setCaret({ para: 0, offset: 2 });
      await H.flush();
      const opts = [{ value: 'a', label: '选项甲' }, { value: 'b', label: '选项乙' },
                    { value: 'c', label: '选项丙' }];
      const ok = await H.insertControl({ type: 'radioGroup', key: 'rg', options: opts });
      await H.flush();
      const form = H.form();
      // 引擎侧的切换（不走鼠标）：单选组切到第二项。
      const toggled = await H.toggleControl(0, 1);
      await H.flush();
      const afterToggle = H.form()[0];
      // 之后再点画布上的第三项：走的是命中 → 前置回调 → 切换这条路。
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      const q = D.controls().items[0].rects[0];
      return {
        ok, form, toggled,
        type: afterToggle.type, sel: afterToggle.value.text, text: afterToggle.text,
        left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height, zoom: ZOOM(),
        qx: q.x, qy: q.y, qw: q.w, qh: q.h,
        caret0: JSON.stringify(H.caret().pos),
      };`);

    check('insertControl 返回 true', prep.ok === true, String(prep.ok));
    check('控件有矩形（诊断读数）', prep.qw > 0, JSON.stringify(prep));
    check('★ 单选组带三个选项', prep.form[0]?.options?.length === 3,
          JSON.stringify(prep.form[0]?.options));
    check('★ 引擎侧切换生效：值落在 b 上', prep.sel === 'b', String(prep.sel));
    check('★ 内容是图形加标签', prep.text.includes('◉ 选项乙') && prep.text.includes('○ 选项甲'),
          prep.text);

    const at = (fx: number) => ({
      x: prep.left + (prep.qx + prep.qw * fx) * prep.zoom * prep.sx,
      y: prep.top + (prep.qy + prep.qh / 2) * prep.zoom * prep.sy,
    });
    const click = async (x: number, y: number) => {
      await p.page.mouse.click(x, y);
      await p.page.waitForTimeout(60);
    };

    await p.page.waitForTimeout(400);
    // 先全选：接下来那一下点在控件里，"点哪儿就选哪儿"要求旧选区先塌掉。
    await p.read<any>(`await H.selectAll(); await H.flush(); return null;`);
    const selectedBefore = await p.read<any>(`return H.selection();`);
    check('前置：全选之后确实有选区', selectedBefore.empty === false,
          JSON.stringify({ empty: selectedBefore.empty, rects: selectedBefore.rects?.length }));

    // 前置回调拒绝：值不变，但落点照摆（选区塌掉）。
    await p.page.evaluate(() => {
      (window as any).__calls = 0;
      (window as any).__demoBeforeChange = () => {
        (window as any).__calls = ((window as any).__calls || 0) + 1;
        return Promise.resolve(false);
      };
    });
    const third = at(0.8);
    await click(third.x, third.y);
    const denied = await p.read<any>(`
      return { form: H.form()[0], sel: H.selection(), caret: JSON.stringify(H.caret().pos) };`);
    check('★ 异步拒绝：切换没有生效', denied.form.value.text === 'b', denied.form.value.text);
    check('★ 点控件内部清掉已有选区（落点照摆，只是光标进不去）', denied.sel.empty === true,
          JSON.stringify({ empty: denied.sel.empty, rects: denied.sel.rects?.length }));

    // 前置回调放行：第三项选中。
    await p.page.evaluate(() => {
      (window as any).__demoBeforeChange = () => {
        (window as any).__calls = ((window as any).__calls || 0) + 1;
        return true;
      };
    });
    await click(third.x, third.y);
    const allowed = await p.read<any>(`return H.form()[0];`);
    check('★ 放行之后第三项选中', allowed.value.text === 'c', String(allowed.value.text));
  });

  test('从控件内部起拖：选区端点落在控件里，光标仍然进不去', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙丙丁戊己庚辛壬癸');
      await H.setCaret({ para: 0, offset: 4 });
      await H.flush();
      await H.insertControl({ type: 'checkBox', key: 'cb', label: '同意' });
      await H.flush();
      await D.stopBlink();
      const it = H.form()[0];
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      const q = D.controls().items[0].rects[0];
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               zoom: ZOOM(), start: it.start, end: it.end, qx: q.x, qy: q.y, qw: q.w, qh: q.h,
               caret: JSON.stringify(H.caret().pos) };
    `);
    check('前置：控件有矩形', geo.qw > 0, JSON.stringify(geo));

    const at = (xPt: number, yPt: number) => ({
      x: geo.left + xPt * geo.zoom * geo.sx,
      y: geo.top + yPt * geo.zoom * geo.sy,
    });
    // 从控件正中间按下，往右拖进正文 —— 起点在屏障里，这是"选区可以从里面开始"。
    const from = at(geo.qx + geo.qw / 2, geo.qy + geo.qh / 2);
    const to = at(geo.qx + geo.qw + 70, geo.qy + geo.qh / 2);
    await p.page.mouse.move(from.x, from.y);
    await p.page.mouse.down();
    for (let i = 1; i <= 5; i++) {
      await p.page.mouse.move(from.x + ((to.x - from.x) * i) / 5, from.y);
      await p.page.waitForTimeout(40);
    }
    await p.page.mouse.up();
    await p.page.waitForTimeout(150);

    const dragged = await p.read<any>(`
      return { sel: H.selection(), caret: JSON.stringify(H.caret().pos),
               visible: H.caret().visible, form: H.form()[0] };`);
    check('★ 从控件内部起手拖出了选区', dragged.sel.empty === false,
          JSON.stringify({ empty: dragged.sel.empty, rects: dragged.sel.rects?.length }));
    // 起点必须真正落在"按下的那一点"上：夹到控件边界（= start.offset）也算错，
    //   那样从标签中间按下，选区却从控件开头开始。
    check('★ 选区起点落在按下的那个字上（不是控件边界）',
          dragged.sel.anchor.para === geo.start.para &&
              dragged.sel.anchor.offset > geo.start.offset &&
              dragged.sel.anchor.offset <= geo.end.offset,
          JSON.stringify(dragged.sel.anchor) + ' / 控件 ' + JSON.stringify([geo.start, geo.end]));
    check('★ 焦点端已经拖到了控件右边', dragged.sel.focus.offset > geo.end.offset,
          JSON.stringify(dragged.sel.focus));
  });

  test('点控件同一处两次：光标停在同一边，不在控件左右来回跳', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙丙丁戊己庚辛壬癸');
      await H.setCaret({ para: 0, offset: 4 });
      await H.flush();
      await H.insertControl({ type: 'checkBox', key: 'cb', label: '同意' });
      await H.flush();
      await D.stopBlink();
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      const q = D.controls().items[0].rects[0];
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               zoom: ZOOM(), qx: q.x, qy: q.y, qw: q.w, qh: q.h };`);
    const at = (fx: number) => ({
      x: geo.left + (geo.qx + geo.qw * fx) * geo.zoom * geo.sx,
      y: geo.top + (geo.qy + geo.qh / 2) * geo.zoom * geo.sy,
    });
    const pos = () => p.read<string>(`return JSON.stringify(H.caret().pos);`);
    const click = async (x: number, y: number) => {
      await p.page.mouse.click(x, y);
      await p.page.waitForTimeout(80);
    };

    for (const [name, fx] of [['左半边', 0.25], ['右半边', 0.8]] as Array<[string, number]>) {
      const q = at(fx);
      await click(q.x, q.y);
      const first = await pos();
      await click(q.x, q.y);
      const second = await pos();
      await click(q.x, q.y);
      const third = await pos();
      check(`★ 点控件${name}三次：光标一个位置都不动`, first === second && second === third,
            [first, second, third].join(' → '));
    }
  });
});

test.describe('改控件定义', () => {
  test('改选项表重建内容、删掉选中项清空值、类型不可改', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.setCaret({ para: 0, offset: 4 });
      await e.flush();
      const opts = [{ value: 'a', label: '选项甲' }, { value: 'b', label: '选项乙' }];
      await e.insertControl({ type: 'radioGroup', key: 'rg', options: opts });
      await e.flush();
      await e.toggleControl(0, 1);
      await e.flush();
      const picked = e.form()[0];
      const digest0 = await e.diag.frameDigest();

      // 换标签 + 删掉正选中的第二项：内容按新表重建，值跟着清空。
      const ok = await e.updateControl(0, { options: [{ value: 'a', label: '甲（改）' }] });
      await e.flush();
      const after = e.form()[0];
      const digest1 = await e.diag.frameDigest();

      // 类型不可改：换类型是"删掉重插"，这里要显式拒掉。
      const bad = await e.updateControl(0, { type: 'checkBoxGroup' });
      const stillSame = e.form()[0];

      await e.undo();
      await e.flush();
      const undone = e.form()[0];
      return { picked, ok, after, digest0, digest1, bad, stillSame, undone };`);

    check('★ 改定义成功', r.ok === true, String(r.ok));
    check('★ 选项表只剩一项', r.after?.options?.length === 1,
          JSON.stringify(r.after?.options));
    check('★ 标签换成甲（改）', r.after?.options?.[0]?.label === '甲（改）',
          JSON.stringify(r.after?.options?.[0]));
    check('★ 被删掉的选中项：值清空', r.after?.value?.text === '',
          JSON.stringify(r.after?.value));
    check('★ 内容按新选项表重建',
          String(r.after?.text || '').includes('甲（改）') &&
            !String(r.after?.text || '').includes('选项乙'),
          String(r.after?.text));
    check('★ 画面跟着变', r.digest0 !== r.digest1, r.digest0 + ' → ' + r.digest1);
    check('★ 类型不可改（被拒）', r.bad === false, String(r.bad));
    check('被拒之后类型没动', r.stillSame?.type === 'radioGroup', String(r.stillSame?.type));
    check('★ 撤销回到改之前：选项与选中的值都回来',
          r.undone?.options?.length === 2 && r.undone?.value?.text === 'b',
          JSON.stringify({ n: r.undone?.options?.length, v: r.undone?.value?.text }));
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
    // 8 = 1 段正文 + 6 格 + 表后那段。表落在文档末尾时必补一段：文档不能以表格结尾，
    //   否则表后面没有能停光标的地方。
    check('★ 段数 = 1 段正文 + 6 格 + 表后一段', r.paras.length === 8, JSON.stringify(r.paras));
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
    // 10 = 1 段正文 + 8 格 + 表后那段（表落在末尾必补，见 insertTable）。
    check('★ 选 2x4 之后插出八格，末尾多一段空的', r.after.length === 10 &&
          (r.after.filter((t: string) => t === '').length === 9), JSON.stringify(r.after));
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

  test('表落在文档末尾：后面补一段空的，能接着写；删整表一步撤销回得来', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.setCaret({ para: 0, offset: 4 });
      await e.flush();
      await e.insertTable({ rows: 1, cols: 2 });
      await e.flush();
      const n = e.paragraphs().length;
      // 末尾那一段在表外：光标停得进去，敲的字落在表后面。
      await e.setCaret({ para: n - 1, offset: 0 });
      await e.flush();
      const tailInTable = (await e.tableState()).inTable;
      const typed = await e.type('表后');
      await e.flush();
      const tailText = e.paragraph(n - 1);

      // 光标回到表里：这时整表该删得动。
      await e.setCaret({ para: 1, offset: 0 });
      await e.flush();
      const before = { inTable: (await e.tableState()).inTable, can: (await e.tableState()).canRemoveTable,
                       n: e.paragraphs().length };
      const ok = await e.tableOp('removeTable');
      await e.flush();
      const after = { inTable: (await e.tableState()).inTable, n: e.paragraphs().length,
                      text: e.text() };
      await e.undo();
      await e.flush();
      const back = { inTable: (await e.tableState()).inTable, n: e.paragraphs().length };
      return { n, tailInTable, typed, tailText, before, ok, after, back };`);

    check('★ 表后补了一段（1 + 2 格 + 1）', r.n === 4, String(r.n));
    check('★ 表后那一段在表外', r.tailInTable === false, String(r.tailInTable));
    check('★ 表后接着打字真的落在表后面', r.typed === true && r.tailText === '表后',
          JSON.stringify(r.tailText));
    check('光标在表里时 engine 说删得动整表', r.before.can === true, JSON.stringify(r.before));
    check('★ 删整表把表与它的格一起去掉了', r.ok === true && r.after.inTable === false &&
          r.after.n < r.before.n, JSON.stringify(r));
    check('★ 一步撤销把表整张恢复', r.back.inTable === true && r.back.n === r.before.n,
          JSON.stringify(r.back));
  });

  test('拖选整张表的全部格：删除连表框一起走；只选一部分则只清内容', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 两格之间的拖选：起点在字上（从边距带起手会变成"点一下选整格"），终点落在第四格。
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
      return { cell1: await at(1), cell2: await at(2), cell4: await at(4),
               left: fr.left, top: fr.top };`);

    // 拖选整张表：四个格全在选区里。
    await p.page.mouse.move(geo.left + geo.cell1.x + 6, geo.top + geo.cell1.y + geo.cell1.h / 2);
    await p.page.mouse.down();
    await p.page.mouse.move(geo.left + geo.cell4.x + 3,
                            geo.top + geo.cell4.y + geo.cell4.h / 2, { steps: 10 });
    await p.page.mouse.up();
    const all = await p.read<any>(`
      await H.flush();
      const picked = (await H.tableState()).selCells;
      const ok = await H.deleteBackward();
      await H.flush();
      const gone = { inTable: (await H.tableState()).inTable, paras: (await H.paragraphs()).length,
                     text: await H.text(), caret: CARET() };
      await H.undo();
      await H.flush();
      const back = { inTable: (await H.tableState()).inTable,
                     paras: (await H.paragraphs()).length };
      return { picked, ok, gone, back };`);
    check('前提：拖选盖住了四格', all.picked === 4, String(all.picked));
    check('★ 删的是整张表（表框一起走，不是只剩一张空表）',
          all.ok === true && all.gone.inTable === false && all.gone.text === '甲\n',
          JSON.stringify({ ok: all.ok, gone: all.gone }));
    // 光标落回被删表的那一处：表后那一段的段首。整格选区的起点在表格内部，那个流位置
    //   随表一起消失、被后面的内容顶上来：拿它当落点，光标就落在段中间。
    check('★ 删完光标停在表原来的位置（表后那一段的段首）',
          all.gone.caret.para === 1 && all.gone.caret.offset === 0,
          JSON.stringify({ para: all.gone.caret.para, offset: all.gone.caret.offset }));
    check('★ 一步撤销把表整张恢复', all.back.inTable === true && all.back.paras === 6,
          JSON.stringify(all.back));

    // 反向一半：只拖两格（不盖满整张表）—— 表留着，选中的那两格内容清空。
    await p.page.mouse.move(geo.left + geo.cell1.x + 6, geo.top + geo.cell1.y + geo.cell1.h / 2);
    await p.page.mouse.down();
    await p.page.mouse.move(geo.left + geo.cell2.x + 3,
                            geo.top + geo.cell2.y + geo.cell2.h / 2, { steps: 10 });
    await p.page.mouse.up();
    const part = await p.read<any>(`
      await H.flush();
      const picked = (await H.tableState()).selCells;
      const ok = await H.deleteBackward();
      await H.flush();
      return { picked, ok, inTable: (await H.tableState()).inTable,
               paras: (await H.paragraphs()).length, text: await H.text() };`);
    check('前提：只选了两格', part.picked === 2, String(part.picked));
    check('★ 没盖满整张表：表留着，只有选中的两格清空',
          part.ok === true && part.inTable === true && part.text === '甲\n\n\nC\nD\n',
          JSON.stringify(part));
  });
});

test.describe('插入控件', () => {
  test('屏障类控件插完，光标落在整块之后（不进去）', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 四种选择控件的内容都是"图形 + 标签"的投影，光标停不进去；插入之后要把光标摆到
    //   整块之后，否则下一次插入会落进它的内容里，长出"选项套选项"那种嵌套。
    const r = await p.read<any>(`
      await H.setText('甲乙丙丁');
      await H.setCaret({ para: 0, offset: 4 });
      await H.flush();
      const out = [];
      for (const t of ['radioButton', 'checkBox', 'radioGroup', 'checkBoxGroup']) {
        const before = H.form().length;
        await H.insertControl(t === 'radioGroup' || t === 'checkBoxGroup'
          ? { type: t, options: [{ value: 'a', label: '选项甲' }] }
          : { type: t, label: '选项甲' });
        await H.flush();
        const list = H.form();
        const mine = list[list.length - 1];
        const caret = H.caret().pos;
        // 光标在不在这一块里面：把它的区间 [start, end) 与光标位置比一下。
        const inside = caret.offset > mine.end.offset === false &&
                       caret.offset >= mine.start.offset && caret.offset < mine.end.offset;
        out.push({ type: t, added: list.length - before, inside,
                   start: mine.start.offset, end: mine.end.offset, caret: caret.offset });
      }
      // 再插两个单选：两者必须是兄弟（第二个不能嵌进第一个）。
      await H.setCaret({ para: 0, offset: 4 });
      await H.flush();
      await H.insertControl({ type: 'radioButton', label: '甲' });
      await H.insertControl({ type: 'radioButton', label: '乙' });
      await H.flush();
      const list = H.form();
      const a = list[list.length - 2];
      const b = list[list.length - 1];
      const nested = a.start.offset <= b.start.offset && b.end.offset <= a.end.offset;
      return { out, nested };`);

    for (const item of r.out) {
      check(`★ 插入 ${item.type} 只多一个控件`, item.added === 1, String(item.added));
      check(`★ 插入 ${item.type} 之后光标不在块里`, item.inside === false,
            `caret=${item.caret} range=[${item.start}, ${item.end})`);
    }
    check('★ 连插两个单选是兄弟，不是嵌套', r.nested === false, JSON.stringify(r));
  });

  test('未配组的单选各自独立：点一下选、再点一下取消', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      await H.setText('甲乙丙丁');
      await H.setCaret({ para: 0, offset: 4 });
      await H.flush();
      await H.insertControl({ type: 'radioButton', label: '甲' });
      await H.insertControl({ type: 'radioButton', label: '乙' });
      await H.flush();
      await H.toggleControl(0, -1);
      await H.toggleControl(1, -1);
      await H.flush();
      const both = H.form().map((c) => c.value.flag);
      await H.toggleControl(0, -1);
      await H.flush();
      return { both, after: H.form().map((c) => c.value.flag) };`);
    check('两个未配组的单选都能选中', r.both[0] === true && r.both[1] === true,
          JSON.stringify(r.both));
    check('再点第一个只取消自己', r.after[0] === false && r.after[1] === true,
          JSON.stringify(r.after));
  });

  test('贴着选择类控件退格：删整块，不进去删字', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const prep = await p.read<any>(`
      await H.setText('甲乙丙丁');
      await H.setCaret({ para: 0, offset: 4 });
      await H.flush();
      await H.insertControl({ type: 'checkBoxGroup',
        options: [{ value: 'a', label: '选项甲' }, { value: 'b', label: '选项乙' }] });
      await H.flush();
      const c = H.form()[0];
      return { n: H.form().length, text: c.text, end: c.end.offset };`);

    check('插入之后有一个复选组', prep.n === 1, String(prep.n));
    check('内容是两项的投影', String(prep.text).includes('选项乙'), String(prep.text));

    // 按键：光标停在控件之后，退格一次应当整块删掉（而不是删掉"项乙"的最后一个字）。
    //   按既有判据的写法在承接层上派发 keydown —— 输入层就是在那里接键的。
    const after = await p.read<any>(`
      const e = new KeyboardEvent('keydown', { key: 'Backspace', bubbles: true, cancelable: true });
      IME().dispatchEvent(e);
      await H.flush();
      return { n: H.form().length, caret: H.caret().pos };`);
    check('★ 一次退格删掉整个控件', after.n === 0, JSON.stringify(after));

    // 再插一次，然后点控件内部再按退格：同样整块走，不进去删字。
    const again = await p.read<any>(`
      await H.setText('甲乙丙丁');
      await H.setCaret({ para: 0, offset: 4 });
      await H.flush();
      await H.insertControl({ type: 'radioGroup',
        options: [{ value: 'a', label: '选项甲' }, { value: 'b', label: '选项乙' }] });
      await H.flush();
      return { n: H.form().length, text: H.form()[0].text };`);
    check('再插一个单选组', again.n === 1, JSON.stringify(again));
  });

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

test.describe('空值的下拉与日期', () => {
  test('插进去看得见、点得开，连插两个不嵌套', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙丙丁');
      await H.setCaret({ para: 0, offset: 4 });
      await H.flush();
      await H.insertControl({ type: 'dropdownList', key: 'dd',
                              placeholder: '请选择科室',
                              options: [{ value: 'a', label: '选项甲' },
                                        { value: 'b', label: '选项乙' }] });
      await H.flush();
      // 屏障把光标摆到整块之后：第二个字段与第一个并排，不落进它的内容里。
      await H.insertControl({ type: 'date', key: 'dt' });
      await H.flush();
      const list = await H.form();
      const items = (await D.controls()).items;
      const boxes = items.map((c) => c.rects[0]);
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      const q = boxes[0];
      return {
        types: list.map((c) => c.type),
        starts: list.map((c) => c.start.offset),
        ends: list.map((c) => c.end.offset),
        texts: items.map((c) => c.text),
        phs: list.map((c) => c.placeholder),
        widths: boxes.map((b) => b.w),
        nested: boxes.length === 2 ? boxes[0].x + boxes[0].w > boxes[1].x + 1 : true,
        left: r.left, top: r.top, Z: ZOOM(),
        cx: q.x + q.w / 2, cy: q.y + q.h / 2,
      };`);

    // 提示不是内容：内容空着，提示记在控件上，几何由提示槽给（宽到点得中）。
    //   日期没写 placeholder ⇒ 记录上是空的，画的是内置那句（"选择日期"）。
    check('★ 空的下拉内容是空的（提示不写成内容）', geo.texts[0] === '',
          JSON.stringify(geo.texts));
    check('★ 空的日期内容是空的', geo.texts[1] === '', JSON.stringify(geo.texts));
    check('★ 作者写的提示记在控件上（日期没写，用内置那句）',
          geo.phs[0] === '请选择科室' && geo.phs[1] === '', JSON.stringify(geo.phs));
    check('★ 空控件仍有可点的几何（不是零宽控件）',
          geo.widths[0] > 20 && geo.widths[1] > 20, JSON.stringify(geo.widths));

    // 提示画成灰字（与文本控件的 placeholder 同一档墨色），不是正文字色。
    //   控件矩形的单位是磅（与选区矩形同一档），页画布是设备像素 —— 读像素前先乘 ZOOM。
    //   读数带从控件左边框再往右 4 像素起：括号是蓝的（三通道差得远），不参与中性色统计。
    const ink = await p.read<any>(`
      await H.flush();
      // 引擎重画进的是页位图，宿主贴到画布上是下一帧的事 —— 少等这两帧，读到的像素与呈现差一拍。
      await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
      const q = (await D.controls()).items[0].rects[0];
      const Z = ZOOM();
      const src = document.querySelector('.heditor-pages canvas[data-page="0"]');
      const w = Math.max(1, Math.round(q.w * Z) - 8);
      const h = Math.max(1, Math.round(q.h * Z));
      const cv = document.createElement('canvas');
      cv.width = w;
      cv.height = h;
      const g2 = cv.getContext('2d');
      g2.drawImage(src, Math.round(q.x * Z) + 5, Math.round(q.y * Z), w, h, 0, 0, w, h);
      const d = g2.getImageData(0, 0, w, h).data;
      let gray = 0;
      let darkest = 255;
      for (let k = 0; k < d.length; k += 4) {
        const R = d[k], G = d[k + 1], B = d[k + 2];
        if (Math.abs(R - G) < 12 && Math.abs(G - B) < 12 && R < 235) {
          if (R > 90 && R < 220) ++gray;
          if (R < darkest) darkest = R;
        }
      }
      return { gray, darkest, w, h };`);
    check('★ 空下拉的提示是灰的（不是当正文画的）', ink.gray > 30 && ink.darkest > 100,
          JSON.stringify(ink));
    check('★ 两个字段是并排的，没嵌在一起', geo.nested === false,
          JSON.stringify({ starts: geo.starts, ends: geo.ends, nested: geo.nested }));

    await p.page.mouse.click(geo.left + geo.cx * geo.Z, geo.top + geo.cy * geo.Z);
    const panel = p.page.locator('.heditor-popover--control');
    await expect(panel).toBeVisible();
    const items = await panel.locator('.heditor-popover-item').count();
    check('★ 点空下拉就弹出选项列表', items === 2, String(items));
    // 方向：候选项竖着排（横着排的一行"下拉"不是下拉）。
    const stacked = await panel.locator('.heditor-popover-item').evaluateAll((els) => {
      if (els.length < 2) return true;
      const a = els[0].getBoundingClientRect();
      const b = els[1].getBoundingClientRect();
      return b.top >= a.bottom - 1 && Math.abs(b.left - a.left) < 2;
    });
    check('★ 选项是一列（竖排），不是横着一行', stacked === true, String(stacked));

    await panel.locator('.heditor-popover-item').first().click();
    await p.read(`await H.flush(); return true;`);
    const got = await p.read<any>(`return (await H.form())[0].value.text;`);
    check('★ 选中之后值落到控件上', got === 'a', String(got));
  });
});

test.describe('空控件的提示文字', () => {
  test('灰字占位：点得进、打字就消失、只读态不画', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙');
      await H.setCaret({ para: 0, offset: 2 });
      await H.flush();
      await H.insertControl({ type: 'text', key: 'name', placeholder: '请输入姓名' });
      await H.flush();
      await D.stopBlink();
      const c = (await H.form())[0];
      const q = CARET();
      const fr = FRAME().getBoundingClientRect();
      return {
        text: c.text, ph: c.placeholder,
        x: q.x, y: q.y, h: q.h, Z: ZOOM(),
        left: fr.left, top: fr.top,
      };`);
    check('★ 插进去的内容是空的（提示不是内容）', geo.text === '', JSON.stringify(geo.text));
    check('★ 提示文字记在控件上', geo.ph === '请输入姓名', JSON.stringify(geo.ph));

    // 一条带里的两种墨：括号是蓝色（三通道差得远），提示是中性灰，正文是黑。
    //   读数带从提示的左沿起（caret.x 就是它），所以左边的"甲乙"不会混进来。
    // 页画布的控制权在引擎手里（OffscreenCanvas），只能 drawImage 出来再读 ——
    //   在它自己身上 getContext 会直接抛 InvalidStateError。
    const rowOf = (x0: number, y0: number, w: number, h: number) => p.read<any>(`
      // 先排空再等两帧：引擎重画进的是页位图，宿主把它贴到画布上是下一帧的事 ——
      //   少等这两帧，读出来的像素与呈现差一拍。
      await H.flush();
      await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
      const src = document.querySelector('.heditor-pages canvas[data-page="0"]');
      const cv = document.createElement('canvas');
      cv.width = ${w};
      cv.height = ${h};
      const g2 = cv.getContext('2d');
      g2.drawImage(src, ${x0}, ${y0}, ${w}, ${h}, 0, 0, ${w}, ${h});
      const d = g2.getImageData(0, 0, ${w}, ${h}).data;
      let gray = 0;
      let ink = 0;
      let darkest = 255;
      let sig = 0;
      for (let k = 0; k < d.length; k += 4) {
        const R = d[k], G = d[k + 1], B = d[k + 2];
        // 只看中性色：括号是蓝的，三通道差得远，不参与。
        if (Math.abs(R - G) < 12 && Math.abs(G - B) < 12) {
          if (R < 235) {
            ++ink;
            if (R < darkest) darkest = R;
            if (R > 90 && R < 220) ++gray;
          }
        }
        sig = (sig * 31 + R * 3 + G * 5 + B) | 0;
      }
      return { gray, ink, darkest, sig };`);

    // 读数带从提示左沿再往右 6 像素起：光标那一竖条（2 像素宽、纯黑）正好压在那个位置上，
    //   它会把"这是灰字"读成"有黑字"。
    const x0 = Math.round(geo.x) + 6;
    const y0 = Math.max(0, Math.round(geo.y) - 2);
    const w = Math.round(250 * geo.Z);
    const h = Math.round(geo.h) + 4;
    const empty = await rowOf(x0, y0, w, h);
    check('★ 空字段画出灰字（提示真的在纸上）', empty.ink > 40 && empty.gray > 40,
          JSON.stringify(empty));
    // 灰字与黑字的区别只在灰度：有正文时最暗的那一像素会压到 60 以下。
    check('★ 提示是灰的，不是当正文画的黑字', empty.darkest > 100, JSON.stringify(empty));

    // 换一句等宽的提示：槽宽一个字都不变，画面必须跟着变 —— 否则页缓存会一直贴旧字。
    const swapped = await p.read<any>(`
      await H.updateControl(0, { placeholder: '请输入名字' });
      await H.flush();
      await D.stopBlink();
      return (await H.form())[0].placeholder;`);
    const after = await rowOf(x0, y0, w, h);
    check('提示文字改到了控件上', swapped === '请输入名字', JSON.stringify(swapped));
    check('★ 等宽换一句：灰字仍在，像素确实换了（行指纹认得出内容变了）',
          after.ink > 40 && after.sig !== empty.sig,
          JSON.stringify({ before: empty, after }));

    // 只读（呈现与打印）态：提示不画 —— 印在纸上就是凭空多一句话。
    await p.read(`
      await H.configure({ mode: 'readOnly' });
      await H.flush();
      await D.stopBlink();
      return true;`);
    const printed = await rowOf(x0, y0, w, h);
    check('★ 只读态不画提示（也不给它占位）', printed.ink === 0, JSON.stringify(printed));
    await p.read(`
      await H.configure({ mode: 'document' });
      await H.flush();
      await D.stopBlink();
      return true;`);

    // 真实鼠标点在提示上：光标落进这个字段（不是落到字段外）。
    await p.page.mouse.click(geo.left + (geo.x + 30 * geo.Z), geo.top + geo.y + geo.h / 2);
    const focused = await p.read<any>(`
      await H.flush();
      return { caret: (await D.controls()).caret, count: (await D.controls()).count };`);
    check('★ 点在提示文字上，光标落进这个字段', focused.caret === 0 && focused.count === 1,
          JSON.stringify(focused));

    const typed = await p.read<any>(`
      await H.type('张');
      await H.flush();
      await D.stopBlink();
      const c = (await H.form())[0];
      return { text: c.text, ph: c.placeholder };`);
    check('★ 打进去的字就是字段的值（提示没被当成内容吃掉）', typed.text === '张',
          JSON.stringify(typed));
    const filled = await rowOf(x0, y0, w, h);
    // 提示是五个灰字，内容只有一个黑字：墨的量掉一大截，而且最暗的那一像素是黑的。
    check('★ 有内容之后不再画提示（灰字没了、换成黑字）',
          filled.darkest < 60 && filled.ink < empty.ink * 0.6,
          JSON.stringify({ empty, filled }));
  });
});
