// 新功能的浏览器判据：双击三击、水印、图片环绕、拖列宽行高、拖拽移动。
//
// 为什么单独一份：这几项在引擎层有对应判据（tests/feature_test.cpp），但它们的"手感"
//   全在输入层与宿主那一侧 —— 连击怎么数、命中一次要回答几件事、拖拽什么时候落地。
//   引擎判据全绿而这里红过好几次（拖选拖到一半就断、把手一磅不动），所以端到端要有一份。
import { check } from '../support/check';
import { test } from '../support/probe';

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

test.describe('新功能', () => {
  test('真鼠标：单击不选、双击选词、三击选段', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 两段：第一段纯汉字（整类连着走），第二段拉丁文（按空格与字母分词的边界更清楚）。
    const geo = await p.read<any>(`
      await H.setText('甲乙丙丁\\none two three');
      await H.flush();
      await H.setCaret({ para: 1, offset: 5 });
      await H.flush();
      await D.stopBlink();
      const cv = FRAME(), r = cv.getBoundingClientRect();
      const q = CARET();
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               x: q.x, y: q.y, h: q.h };`);
    const at = { x: geo.left + geo.x * geo.sx, y: geo.top + (geo.y + geo.h / 2) * geo.sy };

    // 单击：没有选区（连击计数从这里起算，下面两下才是第二、第三击）。
    await p.page.mouse.click(at.x, at.y);
    await p.page.waitForTimeout(80);
    const one = await p.read<any>(`return SEL();`);
    check('单击不产生选区', one.empty === true, JSON.stringify(one));

    // 第二击：选词。'two' 是三个字母。
    await p.page.mouse.click(at.x, at.y);
    await p.page.waitForTimeout(80);
    const two = await p.read<any>(`return SEL();`);
    check('★ 双击选词：选中的就是这个词（3 个字母）',
          two.empty === false && Math.abs(Number(two.focus) - Number(two.anchor)) === 3,
          JSON.stringify(two));

    // 第三击：选整段。'one two three' 整段 13 个字符，明显多于一个词。
    await p.page.mouse.click(at.x, at.y);
    await p.page.waitForTimeout(80);
    const three = await p.read<any>(`return { sel: SEL(), text: await H.text() };`);
    const len = Math.abs(Number(three.sel.focus) - Number(three.sel.anchor));
    check('★ 三击选整段：选区远长于一个词', three.sel.empty === false && len >= 13,
          `长度 ${len} / ${JSON.stringify(three.text)}`);
    check('三击不越出这一段（还是 2 段）', String(three.text).split('\n').length === 2,
          JSON.stringify(three.text));
  });

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

  test('真鼠标：按住选中的文字拖到别处（一步撤销）', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 用两处光标坐标算出"按住哪儿、放到哪儿"：起点在选区里，终点在段末。
    const geo = await p.read<any>(`
      await H.setText('abcdef');
      await H.flush();
      const at = async (off) => {
        await H.setCaret({ para: 0, offset: off });
        await H.flush();
        const q = CARET();
        return { x: q.x, y: q.y, h: q.h };
      };
      const cv = FRAME(), r = cv.getBoundingClientRect();
      await D.stopBlink();
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               o2: await at(2), o3: await at(3), o4: await at(4), o6: await at(6) };`);
    const P = (q: any) => ({
      x: geo.left + q.x * geo.sx,
      y: geo.top + (q.y + q.h / 2) * geo.sy,
    });

    // 先用真鼠标拖出 "cd"（off 2 → 4）。
    const a = P(geo.o2);
    const b = P(geo.o4);
    await p.page.mouse.move(a.x, a.y);
    await p.page.mouse.down();
    await p.page.mouse.move(b.x, b.y, { steps: 4 });
    await p.page.mouse.up();
    await p.page.waitForTimeout(120);
    const sel = await p.read<any>(`return SEL();`);
    check('前置：拖出了两个字', sel.empty === false && Math.abs(sel.focus - sel.anchor) === 2,
          JSON.stringify(sel));

    // 按住选区中间（off 3，离上一次按下够远，不会被当成双击）拖到段末。
    const from = P(geo.o3);
    const to = P(geo.o6);
    await p.page.mouse.move(from.x, from.y);
    await p.page.mouse.down();
    await p.page.mouse.move(to.x + 20, to.y, { steps: 6 });
    await p.page.mouse.up();
    await p.page.waitForTimeout(200);
    const moved = await p.read<any>(`return await H.text();`);
    check('★ 选中的两个字搬到了段末（abefcd）', moved === 'abefcd', JSON.stringify(moved));

    const undone = await p.read<any>(`
      await H.undo();
      await H.flush();
      return await H.text();`);
    check('★ 一步撤销把搬走的字放回原处', undone === 'abcdef', JSON.stringify(undone));
  });
});
