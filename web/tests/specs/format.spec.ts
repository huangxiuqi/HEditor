// 格式判据：字符格式（粗/斜/下划线/字号）、段落格式（行距倍数与段前距）与文档水印都要
// 落到文档上，能在工具栏那套公开面上读写回来，并且画面真的跟着变。
//
// 段落格式的纵向口径只有两种量法：行高取反白矩形的行距（磅值，不经过设备像素），
//   段间距取两段首行行顶之差（行顶就是光标矩形的顶边）。两者都不必自己投影。
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
      const ok = await e.paragraphFormat({ lineSpacing: { rule: 'auto', factor: 2 } });
      await e.flush();
      const after = e.geometry().pages;
      await e.setCaret({ para: 0, offset: 1 });
      const para = await e.paragraphFormatAt();
      return { before, after, ok, para };`);
    check('命令返回 true', r.ok === true, String(r.ok));
    check('行距写进了段落格式', r.para.lineSpacing?.factor === 2, JSON.stringify(r.para));
    check('★ 行距放大之后页数增加', r.after > r.before, r.before + ' → ' + r.after);
  });

  test('★ 固定值行距比字矮时，墨迹在行槽底停住', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });

    // 裁字只在"固定值比字矮"时发生，而且改完行距画面必须跟着重画 —— 后半句不是多余的：
    //   页位图的新鲜度只比指纹，而"字形坐标一个都没动、只有裁切变了"是常态
    //   （单行段落换行距规则时基线不动）。行指纹里漏了裁切，命令返回 true、读回也对，
    //   屏幕上仍是旧帧。
    const r = await p.read<any>(`
      await H.setText('甲乙丙丁');
      await H.setCaret({ para: 0, offset: 0 });
      await D.stopBlink();
      await H.flush();
      const pm = await D.pageMetrics();
      const depth = async () => {
        // 呈现挂在 rAF 上：不等两帧读到的是上一拍的画布。
        await new Promise((res) => requestAnimationFrame(() => requestAnimationFrame(res)));
        const c = CARET();
        const el = [...EL('.heditor-pages').querySelectorAll('canvas')]
          .filter((x) => x.dataset.page === String(c.page))[0];
        const t = document.createElement('canvas');
        t.width = el.width;
        t.height = el.height;
        const g = t.getContext('2d');
        g.drawImage(el, 0, 0);
        const d = g.getImageData(0, 0, el.width, el.height).data;
        const dens = el.width / pm.widthPt;          // 设备像素 / 磅
        // 行槽顶 = 光标矩形的上边；"往下多深还有墨"就是裁边到哪。
        const y0 = Math.floor(c.y);
        const x0 = Math.max(0, Math.floor(c.x));
        const x1 = Math.min(el.width, Math.floor(c.x + 200));
        let last = y0;
        for (let y = Math.max(0, y0); y < Math.min(el.height, y0 + 80); y++) {
          let n = 0;
          for (let x = x0; x < x1; x++) if (d[(y * el.width + x) * 4] < 128) n++;
          if (n >= 3) last = y;
        }
        return { depth: (last - y0) / dens, slot: c.h / dens };
      };
      const auto = await depth();
      await H.paragraphFormat({ lineSpacing: { rule: 'exact', exact: 8 } });
      await H.flush();
      const exact8 = await depth();
      await H.paragraphFormat({ lineSpacing: { rule: 'auto', factor: 1 } });
      await H.flush();
      const back = await depth();
      return { auto, exact8, back };
    `);

    check('★ 单倍行距下墨迹落到基线以下（不是贴着行顶）',
      r.auto.depth > 9.5, `深 ${r.auto.depth.toFixed(1)} 磅 / 行槽 ${r.auto.slot.toFixed(1)}`);
    check('★ 固定值 8 磅：行槽压到 8 磅那一档（比单倍矮两磅以上）',
      r.exact8.slot < r.auto.slot - 1.5, `${r.auto.slot.toFixed(2)} → ${r.exact8.slot.toFixed(2)} 磅`);
    check('★ 固定值 8 磅：墨迹在行槽底停住（裁字生效，不是照旧画到基线下面）',
      r.exact8.depth <= r.exact8.slot + 0.5 && r.exact8.depth < r.auto.depth - 1,
      `auto ${r.auto.depth.toFixed(1)} / exact ${r.exact8.depth.toFixed(1)} / 行槽 ${r.exact8.slot.toFixed(2)}`);
    check('★ 改回单倍：墨迹又落到基线以下（页位图跟着重画了，不是留着旧帧）',
      Math.abs(r.back.depth - r.auto.depth) < 1.0, `深 ${r.back.depth.toFixed(1)} 磅`);
  });

  // 行高以单倍行高（字体的 ascent + descent）为基准乘倍数，倍数小于 1 时行槽跟着变矮。
  //   单倍行高与字号之比是字体度量，不是字号本身：自带的三款字体是 1.448 em，行间隙为 0。
  test('行距：0.8 倍把行高压到单倍的八成，页数跟着少', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      const lines = [];
      for (let i = 0; i < 200; i++) lines.push('第' + (i + 1) + '段的文字内容，用来撑出多页。');
      await H.setText(lines.join('\\n'));
      await H.flush();
      const pages1 = H.geometry().pages;
      await H.selectAll();
      await H.paragraphFormat({ lineSpacing: { rule: 'auto', factor: 0.8 } });
      await H.flush();
      const pages08 = H.geometry().pages;
      // 行高改用反白矩形的行距来量：那是磅值，没有设备像素的取整误差。
      await H.setText('甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥'.repeat(6));
      await H.flush();
      const pitch = async () => {
        await H.selectAll();
        await H.flush();
        const rs = SEL().rects;
        return { v: rs[1].y - rs[0].y, h: rs[0].h, n: rs.length,
                 size: (await H.paragraphFormatAt()).size };
      };
      const one = await pitch();
      await H.selectAll();
      await H.paragraphFormat({ lineSpacing: { rule: 'auto', factor: 0.8 } });
      const tight = await pitch();
      await H.selectAll();
      await H.paragraphFormat({ lineSpacing: { rule: 'auto', factor: 1.5 } });
      const loose = await pitch();
      return { one, tight, loose, pages1, pages08 };`);
    check('前提：这一段折成两行以上，单倍行高量得出来',
              r.one.n >= 2 && r.one.v > 10, `行距 ${r.one.v} 磅，字号 ${r.one.size}`);
    check('★ 单倍行高是字体行高，不是字号（自带字体 1.448 em）',
              Math.abs(r.one.v / r.one.size - 1.448) < 0.01,
              `行高 ${r.one.v} / 字号 ${r.one.size} = ${r.one.v / r.one.size}`);
    check('★ 0.8 倍的行高就是单倍的八成',
              Math.abs(r.tight.v - r.one.v * 0.8) < 0.05,
              `单倍 ${r.one.v}，0.8 倍 ${r.tight.v}`);
    check('★ 0.8 倍的反白高度等于压过之后的行高（不是被单倍顶住）',
              Math.abs(r.tight.h - r.tight.v) < 0.05,
              `行距 ${r.tight.v}，反白高 ${r.tight.h}`);
    check('反向：1.5 倍的行高是单倍的一倍半',
              Math.abs(r.loose.v - r.one.v * 1.5) < 0.05,
              `单倍 ${r.one.v}，1.5 倍 ${r.loose.v}`);
    check('★ 0.8 倍下同样的段落排得更少页',
              r.pages08 < r.pages1, `${r.pages1} → ${r.pages08}`);
  });

  // 三态的后两档按磅值排：fixed 就是那个数（比字矮时按行裁字），至少取固定值与内容高的
  //   较大者。裁字只在绘制那一步：行高与字面盒各归各的，压行不改基线以上。
  test('行距：固定值与最小值按磅值排，固定值比字矮时裁掉字的底部', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      await H.configure({ zoom: 2 });
      await H.setText('甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥');
      await H.flush();
      const band = async (spacing) => {
        await H.selectAll();
        await H.paragraphFormat({ lineSpacing: spacing });
        await H.flush();
        await H.selectAll();
        await H.flush();
        const rs = SEL().rects;
        await H.setCaret({ para: 0, offset: 0 });
        await H.flush();
        const back = (await H.paragraphFormatAt()).lineSpacing;
        return { h: rs[0] ? rs[0].h : 0, back };
      };
      const one = await band({ rule: 'auto', factor: 1 });
      const fixed20 = await band({ rule: 'exact', exact: 20 });
      const least30 = await band({ rule: 'atLeast', exact: 30 });
      const least5 = await band({ rule: 'atLeast', exact: 5 });
      return { one, fixed20, least30, least5 };`);
    check('前提：单倍行距量得出来', r.one.h > 10, `反白高 ${r.one.h}`);
    check('★ 固定值 20：反白高就是 20 磅',
              Math.abs(r.fixed20.h - 20) < 0.05, `反白高 ${r.fixed20.h}`);
    check(' 读回是固定值 20 磅',
              r.fixed20.back?.rule === 'exact' && r.fixed20.back?.exact === 20,
              JSON.stringify(r.fixed20.back));
    check('★ 最小值 30：内容更矮，按 30 磅排',
              Math.abs(r.least30.h - 30) < 0.05, `反白高 ${r.least30.h}`);
    check('★ 最小值 5：内容更高，退回单倍（不是 5）',
              Math.abs(r.least5.h - r.one.h) < 0.05,
              `单倍 ${r.one.h}，最小值 5 排出来 ${r.least5.h}`);
    check(' 读回仍是最小值 5（读回的是写进去的那一份）',
              r.least5.back?.rule === 'atLeast' && r.least5.back?.exact === 5,
              JSON.stringify(r.least5.back));

    // 裁字：同一行字，量"从行槽顶往下多深还有墨"。固定值 8 磅比字矮，墨迹必须在 8 磅处
    //   停住；改回多倍之后必须又落回基线下面 —— 这一条同时盯着"改完段落格式画面必须重画"：
    //   单行段落换行距规则时字形坐标一点不动，只有裁切变了，行指纹必须带着裁切与行高，
    //   否则页位图被判成还新鲜，命令返回 true、读回也对，画布上还是旧墨。
    //   文档必须短到只有一行：多行时量到的是下一行的墨。
    const depth = async (spacing: any) => {
      await p.read(`
        await H.configure({ showMarks: 'none' });
        await H.selectAll();
        await H.paragraphFormat({ lineSpacing: ${JSON.stringify(spacing)} });
        await H.flush();
        return true;`);
      // 换行距要重排重画；读帧前等两帧，否则拿到的是上一拍的位图。
      await p.page.evaluate(
        () =>
          new Promise<void>((done) => {
            requestAnimationFrame(() => requestAnimationFrame(() => done()));
          }),
      );
      return p.read<any>(`
        await H.selectAll();
        await H.flush();
        const z = ZOOM();
        const r = SEL().rects[0];
        const step = D.pagePx().h + D.pagePx().gap;
        const first = D.editStats().windowFirstPage;
        const f = await D.rawFrame();
        const w = D.frameW();
        const x0 = Math.max(0, Math.floor(r.x * z));
        const x1 = Math.min(w, Math.ceil((r.x + r.w) * z));
        const top = Math.floor((r.page - first) * step + r.y * z);
        let last = top;
        for (let dy = 0; dy < Math.round(40 * z); ++dy) {
          let n = 0;
          for (let x = x0; x < x1; ++x) {
            const i = ((top + dy) * w + x) * 4;
            if (f[i] < 128 && f[i + 1] < 128 && f[i + 2] < 128) ++n;
          }
          if (n >= 3) last = top + dy;
        }
        return { band: r.h, depth: (last - top) / z };`);
    };
    // 编辑标记要关掉：段落标记是另一条绘制路径，不跟着字形一起裁，留着会污染"墨到多深"。
    const marks0 = await p.read<string>('return H.config().showMarks;');
    await p.read(`await H.setText('甲乙丙丁戊己庚辛'); await H.flush(); return true;`);
    const full = await depth({ rule: 'auto', factor: 1 });
    const cut = await depth({ rule: 'exact', exact: 8 });
    const back = await depth({ rule: 'auto', factor: 1 });
    check('前提：单倍行距下字的墨迹落到基线下面', full.depth > 12, JSON.stringify(full));
    check('★ 固定值 8 磅：墨迹在行槽底停住（字的底部被裁掉）', cut.depth <= 8.6, JSON.stringify(cut));
    check('★ 改回多倍之后墨迹又落下去（改完段落格式画面跟着重画）', back.depth > 12,
          JSON.stringify(back));
    await p.read('await H.configure({ showMarks: ' + JSON.stringify(marks0) + ' }); return true;');
  });

  // 段前距与上一段的段后距相加，不折叠（CSS 相邻 margin 会折叠成 6，Word 是 12）。
  //   页顶那一段不加：分页之后段首那一截属于页边距；文档第一段照加。
  test('段前距：与上一段的段后距相加，页顶那一段不加', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.read<any>(`
      await H.configure({ zoom: 2 });
      await H.setText('甲\\n乙\\n丙');
      await H.flush();
      const top = async (para) => {
        await H.setCaret({ para, offset: 0 });
        await H.flush();
        const c = CARET();
        return { y: c.y, h: c.h, page: c.page };
      };
      const gap = async () => {
        const a = await top(0);
        const b = await top(1);
        return { v: (b.y - a.y - a.h) / ZOOM(), a };
      };
      // 段落格式挂在段末那枚标记上：选区要盖到下一段开头，那枚标记才在范围内。
      const setPara1 = async (patch) => {
        await H.select({ para: 1, offset: 0 }, { para: 2, offset: 0 });
        return H.paragraphFormat(patch);
      };
      const before = await gap();
      const ok1 = await setPara1({ spaceBefore: 10 });
      const after = await gap();
      const kept = (await H.paragraphFormatAt()).spaceBefore;
      await H.undo();
      const undone = await gap();
      const cleared = (await H.paragraphFormatAt()).spaceBefore;
      await H.select({ para: 0, offset: 0 }, { para: 1, offset: 0 });
      const ok2 = await H.paragraphFormat({ spaceBefore: 12 });
      const first = await top(0);
      const ok3 = await setPara1({ pageBreakBefore: true, spaceBefore: 10 });
      const paged = await top(1);
      const ok4 = await setPara1({ spaceBefore: 0 });
      const pagedNo = await top(1);
      return { before, after, kept, undone, cleared, first, paged, pagedNo, z: ZOOM(),
               ok: [ok1, ok2, ok3, ok4] };`);
    check('四条命令都返回 true', r.ok.every((v: boolean) => v === true), JSON.stringify(r.ok));
    check('前提：段间只有上一段的段后距（6 磅）',
              Math.abs(r.before.v - 6) < 0.8, `量得 ${r.before.v}`);
    check('★ 段前距与上一段的段后距相加（6 + 10 = 16，不是折叠成 10）',
              Math.abs(r.after.v - 16) < 0.8, `量得 ${r.after.v}`);
    check(' 段前距写进了段落格式', r.kept === 10, String(r.kept));
    check(' 一步撤销把段前距退掉，段间回到 6 磅',
              (r.cleared === 0 || r.cleared === undefined) &&
              Math.abs(r.undone.v - 6) < 0.8,
              `读回 ${r.cleared}，量得 ${r.undone.v}`);
    check('★ 文档第一段照样让出段前距（12 磅）',
              Math.abs((r.first.y - r.before.a.y) / r.z - 12) < 1,
              `行顶 ${r.before.a.y} → ${r.first.y} px，密度 ${r.z}`);
    check('前提：第二段段前分页之后落在第二页',
              r.paged.page === 1, `第 ${r.paged.page} 页`);
    check('★ 页顶那一段不加段前距（把 10 磅改成 0，页内行顶一模一样）',
              r.paged.y === r.pagedNo.y, `带段前距 ${r.paged.y} px，改成 0 之后 ${r.pagedNo.y} px`);
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
