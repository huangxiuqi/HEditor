// 图片判据：插入与命中、尺寸把手、环绕、拖动、对象选区。
//
// 图片在引擎里有两条独立的路：行内图跟着行盒排版，浮动图走绕排与叠放。
//   两条路的失效方式不同 —— 行内图错在命中与把手（点不中、拖不动），浮动图错在
//   换档位不重画或锚点被搬走。两种都要在页面上看，读回的结构与几何看不见。
import { check } from '../support/check';
import { test } from '../support/probe';

// 页面里的公共入口。
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
        digest0: await PIXH(),
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
      return { digest: await PIXH(), caret: JSON.stringify(H.caret().pos) };`);
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
      return { digest: await PIXH(), caret: JSON.stringify(H.caret().pos) };`);
    check('★ 点正文：光标移过去了（图片没把这一下吞掉）', afterText.caret !== geo.caret0,
          afterText.caret + ' vs ' + geo.caret0);
    check('★ 点正文之后拖拽框消失（帧回到没有框的样子）',
          afterText.digest !== afterImage.digest, afterText.digest);
  });

  test('注册像素、插入、命中把手、改尺寸与撤销', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
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
      const digestBefore = await PIXH();
      const ok = await e.insertImage({ resourceId: 'pic', pxWidth: w, pxHeight: h });
      await e.flush();
      const digestAfter = await PIXH();

      const list = await e.images();
      const box = list[0];
      const rect = box ? box.rects[0] : null;
      // 图心那个像素必须是图里的颜色 —— 读的是页画布，不是引擎的帧：
      //   帧只能证明"光栅器认为画对了"，画布才跨着 wasm 拷贝、Worker、putImageData 三层。
      //   呈现挂在 rAF 上，先等两帧再取，否则拿到的是上一拍的画布。
      let center = null;
      let drawDiff = null;
      if (rect) {
        await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
        const sc = await e.diag.selfCheck();
        drawDiff = sc && sc.screenVsEngineDiffPx;
        const pm = await e.diag.pageMetrics();
        const cv = [...document.querySelector('.heditor-pages').querySelectorAll('canvas')]
          .filter((c) => c.dataset.page === String(rect.page))[0];
        const t = document.createElement('canvas');
        t.width = cv.width;
        t.height = cv.height;
        const g2 = t.getContext('2d');
        g2.drawImage(cv, 0, 0);
        const px = g2.getImageData(0, 0, cv.width, cv.height).data;
        const dens = cv.width / pm.widthPt;      // 设备像素 / 磅（页内坐标）
        const cx = Math.round((rect.x + rect.w / 2) * dens);
        const cy = Math.round((rect.y + rect.h / 2) * dens);
        const at = (cy * cv.width + cx) * 4;
        center = [px[at], px[at + 1], px[at + 2]];
      }
      const body = rect ? await e.imageHit(rect.x + rect.w / 2, rect.y + rect.h / 2) : null;
      const corner = rect ? await e.imageHit(rect.x, rect.y) : null;
      const outside = rect ? await e.imageHit(rect.x + rect.w + 60, rect.y + rect.h + 60) : null;

      // 选中 → 拖拽框与把手进瞬态层（帧上多出蓝色像素）；取消 → 逐像素回到没选中的样子。
      await e.selectImage(0);
      await e.flush();
      const digestSelected = await PIXH();
      await e.selectImage(-1);
      await e.flush();
      const digestCleared = await PIXH();

      const width0 = box ? box.w : 0;
      const height0 = box ? box.h : 0;
      const resized = await e.resizeImage(0, width0 * 2, height0 * 2);
      await e.flush();
      const afterResize = (await e.images())[0];
      await e.undo();
      await e.flush();
      const afterUndo = (await e.images())[0];

      return { ok, list, center, drawDiff, body, corner, outside, digestBefore, digestAfter, digestSelected,
               digestCleared, width0, height0, resized, afterResize, afterUndo };`);

    check('insertImage 返回 true', r.ok === true, String(r.ok));
    check('★ 文档里有一张图', Array.isArray(r.list) && r.list.length === 1,
          JSON.stringify(r.list));
    check('★ 像素在资源表里', r.list[0]?.ready === true, String(r.list[0]?.ready));
    check('★ 显示尺寸按 96 DPI 折算：32x16 像素 = 24 x 12 磅',
          Math.abs(r.width0 - 24) < 0.01 && Math.abs(r.height0 - 12) < 0.01,
          r.width0 + ' x ' + r.height0);
    check('画布与引擎页像素一致（差额 0）', r.drawDiff === 0, String(r.drawDiff));
    check('★ 图心像素是图里的颜色（不是占位框的底色）',
          !!r.center && Math.abs(r.center[0] - 20) <= 2 && Math.abs(r.center[1] - 120) <= 2 &&
            Math.abs(r.center[2] - 220) <= 2,
          JSON.stringify(r.center));
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
    const digestDuring = await p.read<number>(`return await PIXH();`);
    await p.page.mouse.up();
    await p.page.waitForTimeout(250);
    const afterUp = await imgW();
    check('★ 松手之后尺寸才落地', afterUp > geo.w0 + 5, geo.w0 + ' → ' + afterUp);
    const digestAfter = await p.read<number>(`return await PIXH();`);
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

    // 边框线（离把手很远的那一段）不缩放：改尺寸只发生在画出来的那八个方块上。
    //   但按在已选中的图身上拖是"搬这张图"，所以这一拖会换位置、
    //   不换尺寸 —— 下面量把手之前必须重新取一次几何，否则去找的是一个已经不存在的
    //   把手，看到的现象是"把手拖不动"。
    await dragBy(geo.qx + geo.qw, geo.qy + 20, 30);
    const afterEdge = await imgW();
    check('★ 拖右边框线（离把手远处）不改尺寸', Math.abs(afterEdge - geo.w0) < 0.01,
          geo.w0 + ' → ' + afterEdge);
    const moved = await p.read<any>(`return (await H.images())[0].rects[0];`);
    check('★ 按住已选中的图身拖：换的是位置，不是尺寸', moved.x > geo.qx + 10,
          geo.qx + ' → ' + moved.x);

    // 右中把手：离边线偏移 1 磅也能拖（把手是方块，不沿边线拉伸）。
    await dragBy(moved.x + moved.w, moved.y + moved.h / 2, 40);
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

test.describe('图片环绕', () => {
  test('图片环绕：四周型让文字绕开，叠放档位只影响前后', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      let s = '';
      for (let i = 0; i < 40; i++) s += '文字内容';
      await H.setText(s);
      await H.flush();
      await H.setCaret({ para: 0, offset: 0 });
      await H.flush();
      const data = new Uint8Array(400 * 300 * 4).fill(180);
      await H.registerImage({ id: 'pic', width: 400, height: 300, data });
      await H.insertImage({ resourceId: 'pic', pxWidth: 400, pxHeight: 300, width: 180, height: 120 });
      await H.flush();
      await D.stopBlink();
      const box = (await H.images())[0];
      // 段末那个光标的纵向位置：四周型下文字绕到图旁边，整个段落矮下来，它就往上走。
      const readEnd = async (n) => {
        await H.setCaret({ para: 0, offset: n });
        await H.flush();
        return CARET().y;
      };
      const n = (await H.paragraph(0)).length;
      return { layout0: box.layout, endInline: await readEnd(n), w: box.w, h: box.h };`);
    check('前置：新插的图是行内', geo.layout0 === 'inline', String(geo.layout0));

    const square = await p.read<any>(`
      await H.setImageLayout(0, 'square');
      await H.flush();
      const box = (await H.images())[0];
      await H.setCaret({ para: 0, offset: (await H.paragraph(0)).length });
      await H.flush();
      return { layout: box.layout, y: CARET().y, w: box.w, h: box.h };`);
    check('★ 换成四周型，读回的档位就是四周型', square.layout === 'square', String(square.layout));
    check('★ 四周型：文字绕到图旁边，段落的末尾比行内时更靠上',
          Number(square.y) < Number(geo.endInline) - 10,
          `行内 y=${geo.endInline} → 四周 y=${square.y}`);
    check('换环绕方式不改图的尺寸',
          Math.abs(Number(square.w) - Number(geo.w)) < 0.01 &&
              Math.abs(Number(square.h) - Number(geo.h)) < 0.01,
          `${geo.w}x${geo.h} → ${square.w}x${square.h}`);

    const behind = await p.read<any>(`
      await H.setImageLayout(0, 'behind');
      await H.flush();
      return (await H.images())[0].layout;`);
    check('衬于文字下方是另一档', behind === 'behind', String(behind));

    // 反向：不认识的档位要拒绝，文档不动。
    const bad = await p.read<any>(`
      let threw = false;
      try { await H.setImageLayout(0, 'nope'); } catch (e) { threw = true; }
      return { threw, layout: (await H.images())[0].layout };`);
    check('不认识的环绕方式被拒绝，且不改文档', bad.threw === true && bad.layout === 'behind',
          JSON.stringify(bad));
  });

  test('换环绕方式不搬图：四周型文字绕右、上下型文字下推、改回行内不留残影', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1200, height: 900 } });
    const r = await p.read<any>(`
      const text = [];
      for (let i = 0; i < 6; i++) text.push('第一段文字内容' + i + '，用来观察文字在图片周围怎么排。');
      await H.setText(text.join('\\n'));
      await H.setCaret({ para: 1, offset: 3 });
      await H.flush();
      const data = new Uint8Array(200 * 150 * 4).fill(150);
      await H.registerImage({ id: 'p1', width: 200, height: 150, data });
      await H.insertImage({ resourceId: 'p1', pxWidth: 200, pxHeight: 150 });
      await H.flush();
      await D.stopBlink();
      const probe = async () => {
        await H.setCaret({ para: 1, offset: 0 });
        await H.flush();
        const a = CARET();
        await H.setCaret({ para: 2, offset: 0 });
        await H.flush();
        const b = CARET();
        return { a: [a.x, a.y], b: [b.x, b.y] };
      };
      const out = {};
      out.inline = { rect: (await H.images())[0].rects[0], p: await probe() };
      await H.setImageLayout(0, 'square');
      await H.flush();
      // 逐偏移读插入点：图旁边那一行上分居两侧，就是"文字绕开图"的直接证据。
      const scan = async () => {
        const n = (await H.paragraph(1)).length;
        const pts = [];
        for (let off = 0; off <= n; off += 2) {
          await H.setCaret({ para: 1, offset: off });
          await H.flush();
          const c = CARET();
          pts.push({ x: c.x / ZOOM(), y: c.y / ZOOM(), h: c.h / ZOOM() });
        }
        return pts;
      };
      out.square = { rect: (await H.images())[0].rects[0], p: await probe(), scan: await scan(),
                     audit: (await D.auditPage(0)).diff };
      await H.setImageLayout(0, 'topAndBottom');
      await H.flush();
      out.top = { rect: (await H.images())[0].rects[0], p: await probe(),
                  audit: (await D.auditPage(0)).diff };
      await H.setImageLayout(0, 'behind');
      await H.flush();
      out.behind = { rect: (await H.images())[0].rects[0], p: await probe() };
      await H.setImageLayout(0, 'inFront');
      await H.flush();
      out.front = { rect: (await H.images())[0].rects[0] };
      await H.setImageLayout(0, 'inline');
      await H.flush();
      out.back = { rect: (await H.images())[0].rects[0], audit: (await D.auditPage(0)).diff };
      return out;`);

    const near = (a: any, b: any) => Math.abs(a.x - b.x) < 6 && Math.abs(a.y - b.y) < 6;
    const inl = r.inline.rect;
    for (const k of ['square', 'top', 'behind', 'front', 'back']) {
      check('★ 切到 ' + k + ' 之后图还在原位置（不跳到段首）', near(r[k].rect, inl),
            JSON.stringify(r[k].rect) + ' vs ' + JSON.stringify(inl));
    }
    // 四周型：图左边的空间不够排字时整行让到右边，够的话左边也排 —— 两种情况都
    //   不许有插入点落在图身上，且图的右侧一定有字。
    {
      const im = r.square.rect;
      const pts = r.square.scan.filter((q: any) => q.y + q.h > im.y + 1 && q.y < im.y + im.h - 1);
      check('★ 四周型：文字绕到图的右边',
            pts.some((q: any) => q.x > im.x + im.w + 2),
            '图右 ' + (im.x + im.w).toFixed(1) + ' / 插入点 ' +
              JSON.stringify(pts.map((q: any) => Math.round(q.x))));
      check('★ 四周型：没有字压在图身上',
            !pts.some((q: any) => q.x > im.x - 1 && q.x < im.x + im.w),
            JSON.stringify(pts.filter((q: any) => q.x > im.x - 1 && q.x < im.x + im.w)));
    }
    // 上下型：段落文字被推到底边之下。
    check('★ 上下型：文字被推到图片底边之下',
          r.top.p.a[1] > r.top.rect.y + r.top.rect.h - 2,
          r.top.p.a[1] + ' / 图底 ' + (r.top.rect.y + r.top.rect.h));
    // 衬于下方 / 浮于上方不占版面：文字位置与行内时一样。
    check('★ 衬于文字下方不占版面（文字位置与行内一致）',
          Math.abs(r.behind.p.a[0] - r.inline.p.a[0]) < 1 &&
            Math.abs(r.behind.p.a[1] - r.inline.p.a[1]) < 1,
          JSON.stringify(r.behind.p) + ' vs ' + JSON.stringify(r.inline.p));
    // 改回行内：页位图上不许留下那张浮动图的残影（整页重画之后与现排版逐字节相同）。
    check('★ 改回行内：旧的那张图没有留在页面上（自审 diff = 0）',
          r.back.audit === 0 && r.square.audit === 0 && r.top.audit === 0,
          String(r.back.audit) + ' / ' + String(r.square.audit) + ' / ' + String(r.top.audit));
  });

  test('四周型：图在中间时一行左右都有字，字与选区都不压图', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1200, height: 900 } });
    const geo = await p.read<any>(`
      let s = '';
      for (let i = 0; i < 240; i++) s += '环绕测试';
      await H.setText(s);
      await H.flush();
      await H.setCaret({ para: 0, offset: 0 });
      await H.flush();
      const data = new Uint8Array(120 * 90 * 4).fill(160);
      await H.registerImage({ id: 'mid', width: 120, height: 90, data });
      await H.insertImage({ resourceId: 'mid', pxWidth: 120, pxHeight: 90 });
      await H.flush();
      await H.setImageLayout(0, 'square');
      await H.flush();
      await D.stopBlink();
      const a = (await H.images())[0];
      const fr = FRAME().getBoundingClientRect();
      return { r: a.rects[0], left: fr.left, top: fr.top, z: ZOOM() };`);
    const cx = geo.left + (geo.r.x + geo.r.w / 2) * geo.z;
    const cy = geo.top + (geo.r.y + geo.r.h / 2) * geo.z;
    // 把图拖到版心中间：贴着左边时只剩右侧一段，看不出两段的差别。
    await p.page.mouse.click(cx, cy);
    await p.page.waitForTimeout(120);
    await p.page.mouse.move(cx, cy);
    await p.page.mouse.down();
    await p.page.mouse.move(cx + 150 * geo.z, cy, { steps: 6 });
    await p.page.mouse.up();
    await p.page.waitForTimeout(320);
    const r = await p.read<any>(`
      const img = (await H.images())[0].rects[0];
      // 逐偏移读一次插入点：同一行上分居图两侧，就是"一行两段"的直接证据。
      const n = (await H.paragraph(0)).length;
      const pts = [];
      for (let off = 0; off <= n; off += 2) {
        await H.setCaret({ para: 0, offset: off });
        await H.flush();
        const c = CARET();
        pts.push({ off, x: c.x / ZOOM(), y: c.y / ZOOM(), h: c.h / ZOOM() });
      }
      await H.selectAll();
      await H.flush();
      const sel = SEL();
      return { img, pts, sel };`);

    const img = r.img;
    const near = r.pts.filter((q: any) => q.y + q.h > img.y && q.y < img.y + img.h);
    const twoSided = near.some(
      (a: any) => near.some(
        (b: any) => Math.abs(a.y - b.y) < 4 && a.x < img.x - 2 && b.x > img.x + img.w + 2));
    check('★ 图在中间：同一行上图的左边有插入点、右边也有',
          twoSided, '图旁 ' + near.length + ' 个插入点');
    const badSel = (r.sel.rects || []).filter(
      (q: any) => q.page === img.page && q.y < img.y + img.h - 1 && q.y + q.h > img.y + 1 &&
        Math.min(q.x + q.w, img.x + img.w) - Math.max(q.x, img.x) > 2);
    check('★ 整段选区不横跨图：每一条矩形都让开图', badSel.length === 0,
          JSON.stringify(badSel.slice(0, 2)));
  });
});

test.describe('图片拖动', () => {
  test('按住浮动图拖：整张图跟着指针走（相对位移），不改尺寸、不留残影', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1200, height: 900 } });
    const geo = await p.read<any>(`
      await H.setText('第一段文字内容很多很多\\n第二段文字内容也很多');
      await H.setCaret({ para: 0, offset: 3 });
      await H.flush();
      const data = new Uint8Array(120 * 90 * 4).fill(160);
      await H.registerImage({ id: 'pic', width: 120, height: 90, data });
      await H.insertImage({ resourceId: 'pic', pxWidth: 120, pxHeight: 90 });
      await H.flush();
      await H.setImageLayout(0, 'square');
      await H.flush();
      await D.stopBlink();
      const a = (await H.images())[0];
      const fr = FRAME().getBoundingClientRect();
      return { r: a.rects[0], left: fr.left, top: fr.top, z: ZOOM() };`);
    const px = (v: number) => geo.left + v * geo.z;
    const py = (v: number) => geo.top + v * geo.z;
    const cx = px(geo.r.x + geo.r.w / 2);
    const cy = py(geo.r.y + geo.r.h / 2);
    // 先在图上点一下选中（顺带走一遍"点图不落文本光标"），再按住图身拖。
    await p.page.mouse.click(cx, cy);
    await p.page.waitForTimeout(150);
    await p.page.mouse.move(cx, cy);
    await p.page.mouse.down();
    await p.page.mouse.move(cx + 60, cy + 40, { steps: 5 });
    await p.page.waitForTimeout(160);
    const mid = await p.read<any>(`
      const divs = [...EL('.heditor-pages').children].filter((e) => e.tagName === 'DIV');
      const b = (await H.images())[0];
      return { bars: divs.filter((e) => e.style.display === 'block').length,
               rect: b.rects[0] };`);
    await p.page.mouse.up();
    await p.page.waitForTimeout(280);
    const after = await p.read<any>(`
      const b = (await H.images())[0];
      return { rect: b.rects[0], w: b.w, au: (await D.auditPage(0)).diff };`);
    const dx = after.rect.x - geo.r.x;
    const dy = after.rect.y - geo.r.y;
    check('★ 浮动图跟着指针走了（位移 = 拖动的那一段）',
          Math.abs(dx - 60 / geo.z) < 4 && Math.abs(dy - 40 / geo.z) < 4,
          dx.toFixed(1) + ', ' + dy.toFixed(1) + ' 期望 ' + (60 / geo.z).toFixed(1) + ', ' +
            (40 / geo.z).toFixed(1));
    check('拖图不改尺寸', Math.abs(after.rect.w - geo.r.w) < 0.01 && Math.abs(after.rect.h - geo.r.h) < 0.01,
          JSON.stringify(after.rect) + ' vs ' + JSON.stringify(geo.r));
    check('★ 拖完之后页面上没有残影（自审 diff = 0）', after.au === 0, String(after.au));
    check('★ 拖动中图就跟着指针走（不是松手才动）',
          Math.abs(mid.rect.x - geo.r.x - 60 / geo.z) < 6 &&
            Math.abs(mid.rect.y - geo.r.y - 40 / geo.z) < 6,
          JSON.stringify(mid.rect) + ' vs ' + JSON.stringify(geo.r));
    check('★ 浮动图拖动不画插入点：它不占文字流，没有"松手插在这儿"',
          mid.bars === 0, String(mid.bars));
  });

  test('衬于文字下方拖动：同样跟手、同样没有插入点，撤销一步回原位', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1200, height: 900 } });
    const geo = await p.read<any>(`
      await H.setText('第一段文字内容很多很多\\n第二段文字内容也很多');
      await H.setCaret({ para: 0, offset: 3 });
      await H.flush();
      const data = new Uint8Array(100 * 80 * 4).fill(170);
      await H.registerImage({ id: 'pic', width: 100, height: 80, data });
      await H.insertImage({ resourceId: 'pic', pxWidth: 100, pxHeight: 80 });
      await H.flush();
      await H.setImageLayout(0, 'behind');
      await H.flush();
      await D.stopBlink();
      const a = (await H.images())[0];
      const fr = FRAME().getBoundingClientRect();
      return { r: a.rects[0], left: fr.left, top: fr.top, z: ZOOM() };`);
    const cx = geo.left + (geo.r.x + geo.r.w / 2) * geo.z;
    const cy = geo.top + (geo.r.y + geo.r.h / 2) * geo.z;
    await p.page.mouse.click(cx, cy);
    await p.page.waitForTimeout(150);
    await p.page.mouse.move(cx, cy);
    await p.page.mouse.down();
    await p.page.mouse.move(cx + 50, cy + 30, { steps: 5 });
    await p.page.waitForTimeout(160);
    const mid = await p.read<any>(`
      const divs = [...EL('.heditor-pages').children].filter((e) => e.tagName === 'DIV');
      const b = (await H.images())[0];
      return { bars: divs.filter((e) => e.style.display === 'block').length, rect: b.rects[0] };`);
    await p.page.mouse.up();
    await p.page.waitForTimeout(300);
    const landed = await p.read<any>(`return (await H.images())[0].rects[0];`);
    const undone = await p.read<any>(`
      await H.undo();
      await H.flush();
      return (await H.images())[0].rects[0];`);

    check('★ 衬于下方：拖动中图也跟手',
          Math.abs(mid.rect.x - geo.r.x - 50 / geo.z) < 6 &&
            Math.abs(mid.rect.y - geo.r.y - 30 / geo.z) < 6,
          JSON.stringify(mid.rect) + ' vs ' + JSON.stringify(geo.r));
    check('★ 衬于下方：拖动中同样不画插入点', mid.bars === 0, String(mid.bars));
    check('★ 松手之后落在拖动到的位置（预览不是假的）',
          Math.abs(landed.x - mid.rect.x) < 3 && Math.abs(landed.y - mid.rect.y) < 3,
          JSON.stringify(landed) + ' vs ' + JSON.stringify(mid.rect));
    check('★ 一步撤销回到原位', Math.abs(undone.x - geo.r.x) < 1 && Math.abs(undone.y - geo.r.y) < 1,
          JSON.stringify(undone) + ' vs ' + JSON.stringify(geo.r));
  });

  test('拖衬于文字下方的图：一帧只重画它盖住的那一页，不跟着重画后面每一页', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1200, height: 900 } });
    const geo = await p.read<any>(`
      let s = '';
      for (let i = 0; i < 150; i++) s += '拖动只重画一页的正文内容，用来撑出多页文档。';
      await H.setText(s);
      await H.flush();
      await H.setCaret({ para: 5, offset: 0 });
      await H.flush();
      const data = new Uint8Array(120 * 90 * 4).fill(160);
      await H.registerImage({ id: 'one', width: 120, height: 90, data });
      await H.insertImage({ resourceId: 'one', pxWidth: 120, pxHeight: 90 });
      await H.flush();
      // 这一下是整篇重排：下面拿它的重画页数当反向的一半（证明"只画一页"不是恒真）。
      //   衬于下方不占版面，拖动只影响它盖住的那一页 —— 这条走的是轻量那一路。
      await H.setImageLayout(0, 'behind');
      await H.flush();
      await D.stopBlink();
      const a = (await H.images())[0];
      const fr = FRAME().getBoundingClientRect();
      const st = D.editStats();
      return { r: a.rects[0], left: fr.left, top: fr.top, z: ZOOM(),
               pages: st.totalPages, window: st.windowPages, atLayout: st.repaintedPages };`);
    check('文档够多页、窗口也铺得开（否则这条判据恒真）',
          geo.pages >= 3 && geo.window >= 2, JSON.stringify({ pages: geo.pages, window: geo.window }));
    check('反向：换环绕档位是整篇重排，重画的不止一页', geo.atLayout > 1, String(geo.atLayout));

    const cx = geo.left + (geo.r.x + geo.r.w / 2) * geo.z;
    const cy = geo.top + (geo.r.y + geo.r.h / 2) * geo.z;
    await p.page.mouse.click(cx, cy);
    await p.page.waitForTimeout(150);
    await p.page.mouse.move(cx, cy);
    await p.page.mouse.down();
    await p.page.mouse.move(cx + 12 * geo.z, cy + 6 * geo.z, { steps: 3 });
    await p.page.waitForTimeout(160);
    const mid = await p.read<any>(`
      const st = D.editStats();
      const b = (await H.images())[0];
      return { repainted: st.repaintedPages, paint: st.paintMs, page: b.rects[0].page };`);
    await p.page.mouse.up();
    await p.page.waitForTimeout(280);
    check('★ 拖动一帧只重画图盖住的那一页', mid.repainted === 1, JSON.stringify(mid));
  });

  test('拖四周型图：文字当场让开，不等松手', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1200, height: 900 } });
    const geo = await p.read<any>(`
      let s = '';
      for (let i = 0; i < 60; i++) s += '拖动时文字要跟着让开的正文内容，一行排满才有绕排可言。';
      await H.setText(s);
      await H.flush();
      await H.setCaret({ para: 2, offset: 0 });
      await H.flush();
      const data = new Uint8Array(120 * 90 * 4).fill(160);
      await H.registerImage({ id: 'w', width: 120, height: 90, data });
      await H.insertImage({ resourceId: 'w', pxWidth: 120, pxHeight: 90 });
      await H.flush();
      await H.setImageLayout(0, 'square');
      await H.flush();
      await D.stopBlink();
      const a = (await H.images())[0];
      const fr = FRAME().getBoundingClientRect();
      return { r: a.rects[0], left: fr.left, top: fr.top, z: ZOOM() };`);
    const cx = geo.left + (geo.r.x + geo.r.w / 2) * geo.z;
    const cy = geo.top + (geo.r.y + geo.r.h / 2) * geo.z;
    await p.page.mouse.click(cx, cy);
    await p.page.waitForTimeout(150);
    await p.page.mouse.move(cx, cy);
    await p.page.mouse.down();
    await p.page.mouse.move(cx + 70 * geo.z, cy, { steps: 6 });
    await p.page.waitForTimeout(240);
    // 全程没有松手：下面读到的版面就是"拖动中"的那一份。
    const mid = await p.read<any>(`
      const im = (await H.images())[0].rects[0];
      const n = (await H.paragraph(2)).length;
      const pts = [];
      for (let off = 0; off <= n; off += 2) {
        await H.setCaret({ para: 2, offset: off });
        await H.flush();
        const c = CARET();
        pts.push({ x: c.x / ZOOM(), y: c.y / ZOOM(), h: c.h / ZOOM() });
      }
      return { im, pts };`);
    await p.page.mouse.up();
    await p.page.waitForTimeout(300);

    const band = mid.pts.filter(
      (q: any) => q.y + q.h > mid.im.y + 1 && q.y < mid.im.y + mid.im.h - 1);
    check('★ 拖动中图已经挪到位', Math.abs(mid.im.x - geo.r.x - 70) < 8,
          mid.im.x + ' vs ' + (geo.r.x + 70));
    check('★ 拖动中没有字压在图身上（文字当场让开，不是松手才排）',
          !band.some((q: any) => q.x > mid.im.x - 1 && q.x < mid.im.x + mid.im.w),
          JSON.stringify(band.filter((q: any) => q.x > mid.im.x - 1 && q.x < mid.im.x + mid.im.w)));
  });

  test('拖完浮动图松手：视口停在原处，不被光标拽走', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1200, height: 900 } });
    await p.read<any>(`
      let s = '';
      for (let i = 0; i < 400; i++) s += '松手之后视口不应该被光标拽走的正文内容，一行排满。\\n';
      await H.setText(s);
      await H.flush();
      const data = new Uint8Array(100 * 80 * 4).fill(170);
      await H.registerImage({ id: 's', width: 100, height: 80, data });
      await H.setCaret({ para: 3, offset: 0 });
      await H.flush();
      await H.insertImage({ resourceId: 's', pxWidth: 100, pxHeight: 80 });
      await H.flush();
      await H.setImageLayout(0, 'square');
      await H.flush();
      // 光标放到文档末尾，视口滚回第一页：松手时两者分居两端，一拽就看得出来。
      await H.setCaret({ para: 399, offset: 0 });
      await H.flush();
      await D.stopBlink();
      const stage = document.querySelector('.heditor-stage');
      stage.scrollTop = 0;
      await H.flush();
      return stage.scrollTop;`);
    await p.page.waitForTimeout(500);
    const geo = await p.read<any>(`
      const stage = document.querySelector('.heditor-stage');
      const a = (await H.images())[0];
      const fr = FRAME().getBoundingClientRect();
      const st = D.editStats();
      return { r: a.rects[0], left: fr.left, top: fr.top, z: ZOOM(),
               scroll: stage.scrollTop, pages: st.totalPages, caretPage: st.caretPage,
               pos: (await H.caret()).pos, paras: st.totalParagraphs };`);
    check('文档够长、光标与视口分居两端（否则这条判据恒真）',
          geo.pages >= 3 && geo.caretPage + 1 >= geo.pages - 1,
          JSON.stringify(geo));

    const cx = geo.left + (geo.r.x + geo.r.w / 2) * geo.z;
    const cy = geo.top + (geo.r.y + geo.r.h / 2) * geo.z;
    await p.page.waitForTimeout(320);
    await p.page.mouse.click(cx, cy);
    await p.page.waitForTimeout(200);
    await p.page.mouse.move(cx, cy);
    await p.page.mouse.down();
    await p.page.mouse.move(cx + 40 * geo.z, cy + 20 * geo.z, { steps: 5 });
    await p.page.mouse.up();
    await p.page.waitForTimeout(320);
    const after = await p.read<any>(`
      const stage = document.querySelector('.heditor-stage');
      const st = D.editStats();
      return { scroll: stage.scrollTop, caretPage: st.caretPage,
               x: (await H.images())[0].rects[0].x };`);
    check('★ 松手之后视口停在原地（没被旧光标拽回末尾）',
          Math.abs(after.scroll - geo.scroll) < 4,
          geo.scroll + ' → ' + after.scroll);
    check('图确实落到了新位置（不是因为没搬才不滚）', Math.abs(after.x - geo.r.x - 40) < 8,
          String(after.x));

    // 反向：光标真的挪了的时候，视口照样要跟过去 —— 证明上面那条不是"把滚动关了"。
    await p.read<any>(`
      await H.setCaret({ para: 399, offset: 0 });
      await H.flush();
      return 1;`);
    await p.page.keyboard.type('x');
    await p.page.waitForTimeout(420);
    const typed = await p.read<any>(`
      const stage = document.querySelector('.heditor-stage');
      return { scroll: stage.scrollTop, caretPage: D.editStats().caretPage };`);
    check('反向：真的打字（光标动了）时视口照样跟过去',
          typed.caretPage >= geo.pages - 1 && typed.scroll > geo.scroll + 100,
          JSON.stringify(typed) + ' vs ' + geo.scroll);
  });

  test('拖出版心：图停在版心上，不越界也不被压扁', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 1200, height: 900 } });
    const geo = await p.read<any>(`
      await H.setText('四周型拖到页边');
      await H.flush();
      await H.setCaret({ para: 0, offset: 0 });
      await H.flush();
      const data = new Uint8Array(120 * 90 * 4).fill(160);
      await H.registerImage({ id: 'edge', width: 120, height: 90, data });
      await H.insertImage({ resourceId: 'edge', pxWidth: 120, pxHeight: 90 });
      await H.flush();
      await H.setImageLayout(0, 'square');
      await H.flush();
      await D.stopBlink();
      const z = ZOOM();
      const g = H.geometry();
      const b = (await H.images())[0];
      const fr = FRAME().getBoundingClientRect();
      return { pageW: g.page.w / z, pageH: g.page.h / z, w: b.w, h: b.h, r: b.rects[0],
               x: fr.left, y: fr.top, z };`);
    const at = (r: any): { x: number; y: number } => ({
      x: geo.x + (r.x + r.w / 2) * geo.z,
      y: geo.y + (r.y + r.h / 2) * geo.z,
    });
    const dragBy = async (from: any, dx: number): Promise<any> => {
      const c = at(from);
      await p.page.mouse.move(c.x, c.y);
      await p.page.mouse.down();
      await p.page.mouse.move(c.x + dx, c.y, { steps: 8 });
      await p.page.mouse.up();
      await p.page.waitForTimeout(320);
      return p.read<any>(`return (await H.images())[0].rects[0];`);
    };
    // 一路往右拖到拖不动（中途的位移量远大于版心宽度）。
    const r1 = await dragBy(geo.r, 900);
    const r2 = await dragBy(r1, 200);
    check('★ 拖出版心就停住：再往右拖 200 px 一点不动',
          Math.abs(r2.x - r1.x) < 1.0, `x ${r1.x.toFixed(1)} → ${r2.x.toFixed(1)}`);
    check('★ 停住的位置还在纸上（没被页边界切掉）',
          r1.x + r1.w <= geo.pageW - 1.0 && r1.x >= 1.0,
          JSON.stringify(r1) + `，页宽 ${geo.pageW.toFixed(1)}`);
    check('★ 拖出界也不压扁：尺寸一点没变',
          Math.abs(r1.w - geo.w) < 0.01 && Math.abs(r1.h - geo.h) < 0.01,
          JSON.stringify(r1) + ' vs ' + geo.w + ' x ' + geo.h);

    // 反向：从落点往回拖 60 px 要立刻动 —— 存下来的偏移也是夹过的，
    //   否则前 60 px 只是把出界的那一截走回来（图不动）。
    const r3 = await dragBy(r1, -60);
    check('★ 再往回拖 60 px：图当场跟着回来（没有先空走一段）',
          Math.abs(r3.x - (r1.x - 60 / geo.z)) < 3.0,
          `${r1.x.toFixed(1)} → ${r3.x.toFixed(1)}，期望 ${(r1.x - 60 / geo.z).toFixed(1)}`);
  });
});
