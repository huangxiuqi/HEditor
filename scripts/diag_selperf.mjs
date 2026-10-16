// 诊断：选区与滚动的三笔开销分开量（不入判据）。
//
// 要看的现象：
//   其一，2000 段文档全选之后，滚动时新页要很久才画出来；
//   其二，换成 Worker 宿主之后，这个示例滚动新页本来就很慢；
//   其三，拖选不跟手，快速来回拖时选区一直在画，松手后还要几秒才停。
//
// 先把"钱花在哪一层"分开：引擎的绘制分解（铺面积 / 字形 / 帧搬运）、
//   选区矩形条数、每拍状态消息里选区那一份的大小、以及宿主侧的帧间隔。
//
// 用法：node scripts/diag_selperf.mjs --port 9222 --url http://127.0.0.1:8781/web/index.html
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { CDP, sleep } from './cdp.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};
const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://127.0.0.1:8781/web/index.html');
const PARAS = Number(opt('paras', '2000'));

const cdp = { conn: null };

// cdp.eval 把整段包成 (function(){ ... })()，所以带 await 的体要自己再包一层 async IIFE。
//   两个入口：run 跑语句体、ev 取表达式的值。
const run = (body) => cdp.conn.eval('return (async () => { ' + body + ' })();');
const ev = (expr) => cdp.conn.eval('return (async () => { return ' + expr + '; })();');

const q = (a, p) => {
  const s = a.slice().sort((x, y) => x - y);
  return s[Math.min(s.length - 1, Math.floor(s.length * p))];
};

/**
 * 连做 N 次同一个调用，每次记一次墙钟，并把引擎那一拍的账一起带回来。
 * 墙钟的最小值才是"这一层真正要花的钱"，均值里掺着调度抖动。
 */
const timed = (call, n) => `
return (async () => {
  const H = window.__heditor;
  const rows = [];
  for (let i = 0; i < ${n}; ++i) {
    const t0 = performance.now();
    ${call}
    const wall = performance.now() - t0;
    const st = H.editStats;
    const ss = H.stats;
    rows.push({ wall, paint: st.paintMs, engine: st.totalMs,
                fill: ss.paintFillMs, glyph: ss.paintGlyphMs, present: ss.paintPresentMs,
                pixels: st.repaintedPixels, repainted: st.repaintedPages,
                windowPages: st.windowPages, framePx: st.pixels === undefined ? 0 : 0 });
  }
  return JSON.stringify(rows);
})()`;

async function main() {
  const target = await CDP.findPageTarget(PORT);
  const c = await CDP.connect(target.webSocketDebuggerUrl);
  cdp.conn = c;
  await c.send('Page.enable');
  await c.send('Runtime.enable');
  await c.disableHttpCache();
  await c.send('Emulation.setDeviceMetricsOverride',
               { width: 1500, height: 1200, deviceScaleFactor: 1, mobile: false });
  await c.send('Page.navigate', { url: URL_ });
  await c.waitFor('return !!(window.__heditor && window.__heditor.ready)', 'ready', 120000);
  await c.eval('return window.__heditor.stopBlink();');

  console.log(`URL ${URL_}`);

  const mouse = async (type, x, y, buttons) => {
    await c.send('Input.dispatchMouseEvent',
                 { type, x: Math.round(x), y: Math.round(y), button: 'left',
                   buttons, clickCount: 1 });
  };

  // 装载 2000 段长文档
  const t0 = Date.now();
  await run('return await window.__heditor.run(7, 1.5);');
  await c.eval('return window.__heditor.stopBlink();');
  await sleep(300);
  const doc = JSON.parse(await c.eval(`
    const H = window.__heditor;
    const st = H.editStats;
    return JSON.stringify({ paras: st.totalParagraphs, lines: st.totalLines,
      pages: st.totalPages, doc: st.docCodepoints, fw: H.frameW(), fh: H.frameH(),
      wp: st.windowPages, wf: st.windowFirstPage, ms: ${Date.now()} - ${t0} });`));
  console.log(`装载 ${doc.paras} 段 / ${doc.lines} 行 / ${doc.pages} 页 / ${doc.doc} 码点` +
              `  帧 ${doc.fw} × ${doc.fh}（窗口 ${doc.wp} 页@${doc.wf}）  合计 ${doc.ms} ms`);

  // 量 A：无选区时的呈现代价
  console.log('\n=== A. 呈现（无选区）===');
  {
    const rows = JSON.parse(await run(timed('await H.present();', 8)));
    console.log(`  wall min ${q(rows.map(r => r.wall), 0).toFixed(2)} ms` +
                `  绘制 ${q(rows.map(r => r.paint), 0).toFixed(2)} ms` +
                `（铺面积 ${q(rows.map(r => r.fill), 0).toFixed(2)}` +
                ` + 字形 ${q(rows.map(r => r.glyph), 0).toFixed(2)}` +
                ` + 帧搬运 ${q(rows.map(r => r.present), 0).toFixed(2)}）` +
                `  写入 ${q(rows.map(r => r.pixels), 0)} px  重画 ${q(rows.map(r => r.repainted), 0)} 页`);
  }

  // 量 B：全选之后
  console.log('\n=== B. 全选（整篇）===');
  {
    const t = await run(`
      const H = window.__heditor;
      const t0 = performance.now();
      await H.selectAll();
      const wall = performance.now() - t0;
      const sel = H.selection;
      return JSON.stringify({ wall, rects: sel.rects.length, empty: sel.empty,
        anchor: sel.anchor, focus: sel.focus,
        json: JSON.stringify(sel).length });`);
    const o = JSON.parse(t);
    console.log(`  selectAll 一次 ${o.wall.toFixed(1)} ms` +
                `  选区矩形 ${o.rects} 条  序列化 ${(o.json / 1024).toFixed(1)} KB` +
                `  两端 ${o.anchor}..${o.focus}`);
    const rows = JSON.parse(await run(timed('await H.present();', 8)));
    console.log(`  之后 present：wall min ${q(rows.map(r => r.wall), 0).toFixed(2)} ms` +
                `  绘制 ${q(rows.map(r => r.paint), 0).toFixed(2)} ms` +
                `（铺面积 ${q(rows.map(r => r.fill), 0).toFixed(2)}` +
                ` + 字形 ${q(rows.map(r => r.glyph), 0).toFixed(2)}` +
                ` + 帧搬运 ${q(rows.map(r => r.present), 0).toFixed(2)}）` +
                `  写入 ${q(rows.map(r => r.pixels), 0)} px`);
  }

  // 量 C：滚动换窗（无选区 vs 全选）
  //   每拍跳一整页 ⇒ 必然换窗口（这是"滚动新页"最贵的那一档）。
  const scrollTest = `
return (async () => {
  const H = window.__heditor;
  const stage = document.getElementById('stage');
  const dpr = window.devicePixelRatio || 1;
  const step = (H.pagePx[1] + H.pagePx[2]) / dpr;
  stage.scrollTop = 0;
  await new Promise((r) => setTimeout(r, 120));
  const gaps = [];
  let last = 0;
  let n = 0;
  const N = 10;
  return new Promise((resolve) => {
    const tick = (ts) => {
      if (last) gaps.push(ts - last);
      last = ts;
      if (n < N) {
        ++n;
        stage.scrollTop = n * step;
        requestAnimationFrame(tick);
      } else {
        requestAnimationFrame(() => requestAnimationFrame(() => {
          resolve(JSON.stringify({
            gaps, p50: 0, winPages: H.editStats.windowPages, canvases: H.viewport.canvases,
          }));
        }));
      }
    };
    requestAnimationFrame(tick);
  });
})()`;

  console.log('\n=== C. 滚动换窗（每拍跳一整页）===');
  for (const label of ['无选区', '全选']) {
    if (label === '全选') {
      await run('return await window.__heditor.selectAll();');
      await sleep(200);
    }
    const r = JSON.parse(await c.eval(scrollTest));
    const g = r.gaps.filter((x) => x > 0);
    console.log(`  ${label}：帧间隔 p50 ${q(g, 0.5).toFixed(1)} / p90 ${q(g, 0.9).toFixed(1)}` +
                ` / max ${Math.max(...g).toFixed(1)} ms（>20ms 的慢帧 ${g.filter(x => x > 20).length}` +
                `/${g.length}）  窗口 ${r.winPages} 页  画布 ${r.canvases} 张`);
    // 连续单页下滚 8 跳，逐跳读数：哪几跳落在余量里（不换窗）、哪几跳平移、哪几跳全建。
    //   只看"一跳"没用 —— 那一跳可能恰好没换窗，读到的是上一拍的账。
    const rows = JSON.parse(await run(`
      const H = window.__heditor;
      const stage = document.getElementById('stage');
      const dpr = window.devicePixelRatio || 1;
      const step = (H.pagePx[1] + H.pagePx[2]) / dpr;
      const out = [];
      const settle = () => new Promise((res) => setTimeout(res, 260));
      stage.scrollTop = 0;
      await settle();
      for (let i = 1; i <= 8; ++i) {
        stage.scrollTop = i * step;
        await settle();
        const st = H.editStats, ss = H.stats;
        out.push({ page: i, paint: st.paintMs, fill: ss.paintFillMs, glyph: ss.paintGlyphMs,
                   present: ss.paintPresentMs, glyphs: ss.paintedGlyphs,
                   repainted: st.repaintedPages, windowFirst: st.windowFirstPage,
                   windowPages: st.windowPages, pixels: st.repaintedPixels });
      }
      return JSON.stringify(out);`));
    for (const r of rows) {
      console.log(`      滚到第 ${r.page} 页：窗口 ${r.windowFirst}+${r.windowPages}` +
                  `  绘制 ${r.paint.toFixed(1)} ms（铺面积 ${r.fill.toFixed(1)}` +
                  ` + 字形 ${r.glyph.toFixed(1)}（${r.glyphs} 个）` +
                  ` + 帧搬运 ${r.present.toFixed(1)}）` +
                  `  重画页 ${r.repainted}  写入 ${r.pixels} px`);
    }
  }

  // 量 D：状态消息里选区那一份有多大（Worker 宿主才有意义）
  {
    const s = await c.eval(`
      const H = window.__heditor;
      const sel = H.selection;
      return JSON.stringify({ rects: sel.rects.length, bytes: JSON.stringify(sel).length });`);
    console.log(`\n=== D. 每拍状态里的选区 ===`);
    console.log(`  H.selection ${JSON.parse(s).rects} 条矩形 / ${(JSON.parse(s).bytes / 1024).toFixed(1)} KB` +
                `（Worker 宿主每拍都推一份）`);
  }

  // 量 E：同一个窗口来回跳（分清"首见光栅"与"重建开销"）
  //   若第二次起明显变快 ⇒ 大头是字形光栅（首见），缓存对位置不敏感；
  //   若每次都一样 ⇒ 大头是"重建整窗"这个动作本身（清底图、搬帧、建画布）。
  console.log('\n=== E. 同一窗口来回跳 6 次 ===');
  {
    await run('return await window.__heditor.setCaret(0, 0);');
    const rows = JSON.parse(await run(`
      const H = window.__heditor;
      const stage = document.getElementById('stage');
      const dpr = window.devicePixelRatio || 1;
      const step = (H.pagePx[1] + H.pagePx[2]) / dpr;
      const out = [];
      const jump = (n) => new Promise((res) => {
        stage.scrollTop = n * step * 2;
        // 等固定时长而不是两帧：滚动事件是在下一个渲染时机才派发的，宿主的 rAF 因此
        //   排在探针的 rAF 之后，两帧读数会落在宿主更新之前（症状：6 次读数一模一样）。
        setTimeout(() => {
          const st = H.editStats, ss = H.stats;
          res({ paint: st.paintMs, fill: ss.paintFillMs, glyph: ss.paintGlyphMs,
                present: ss.paintPresentMs, glyphs: ss.paintedGlyphs,
                repainted: st.repaintedPages, windowFirst: st.windowFirstPage,
                windowPages: st.windowPages, pixels: st.repaintedPixels });
        }, 260);
      });
      for (let i = 0; i < 6; ++i) out.push(await jump(i % 2 === 0 ? 1 : 0));
      return JSON.stringify(out);`));
    rows.forEach((r, i) => {
      console.log(`  第 ${i + 1} 次：绘制 ${r.paint.toFixed(1)} ms` +
                  `（铺面积 ${r.fill.toFixed(1)} + 字形 ${r.glyph.toFixed(1)}（${r.glyphs} 个）` +
                  ` + 帧搬运 ${r.present.toFixed(1)}）` +
                  `  重画 ${r.repainted}/${r.windowPages} 页 ${r.pixels} px` +
                  `  窗口首页 ${r.windowFirst}`);
    });
  }

  // 量 F：拖选的真实延迟（真鼠标事件）
  //   三件事分开：单步的往返、连续来回拖时"跟不跟得上"、以及松手之后还要画多久。
  console.log('\n=== F. 拖选延迟（真鼠标事件，来回 40 步）===');
  {
    await run('return await window.__heditor.selectAll();');
    await sleep(150);
    await run('return await window.__heditor.setCaret(0, 0);');
    // 装一个 mousemove 计数器：分清"事件根本没到页面"与"到了但拖选没处理"。
    await run('window.__mm = 0; window.addEventListener("mousemove", () => { window.__mm++; }, true);');
    const geo = JSON.parse(await run(`
      const H = window.__heditor;
      const cv = H.canvas, r = cv.getBoundingClientRect();
      const s = document.getElementById('stage').getBoundingClientRect();
      const c = H.caret;
      return JSON.stringify({ left: r.left, top: r.top, sx: r.width / cv.width,
        sy: r.height / cv.height, cx: r.left + c.x * (r.width / cv.width),
        cy: r.top + (c.y + c.h / 2) * (r.height / cv.height),
        midX: (s.left + s.right) / 2, top: s.top + 40,
        stageTop: s.top, stageBottom: s.bottom, cvW: cv.width, cvH: cv.height,
        rw: r.width, rh: r.height, caret: [c.x, c.y, c.h] });`));
    console.log(`  几何 ${JSON.stringify(geo)}`);
    await mouse('mousePressed', geo.cx, geo.cy, 1);
    await sleep(60);
    const afterPress = JSON.parse(await run(`
      const H = window.__heditor;
      return JSON.stringify({ drag: H.drag, mm: window.__mm || 0 });`));
    // 连发 40 次移动、不等待每一条：这才是"快速来回拖"的真实形态（事件排在渲染
    //   进程里，主线程一次只能处理一个）。每步 await 的话 CDP 往返会把节拍摊平，
    //   量到的只是往返时延，看不出积压。
    const sends = [];
    for (let i = 0; i < 40; ++i) {
      const y = i % 2 === 0 ? geo.top + 300 + (i % 5) * 12 : geo.cy + 20 + (i % 7) * 10;
      sends.push(mouse('mouseMoved', geo.midX + (i % 3) * 40, y, 1));
    }
    await Promise.all(sends);
    // 每次移动都让出一帧，直到"引擎不再画新的选区"。
    const readExt = () => run('return (window.__heditor.drag || {}).extends || 0;');
    let last = -1;
    let stable = 0;
    const t0 = Date.now();
    while (Date.now() - t0 < 6000) {
      const n = Number(await readExt());
      if (n === last) { if (++stable >= 4) break; } else { stable = 0; last = n; }
      await sleep(30);
    }
    const drainMs = Date.now() - t0;
    // 松手前读一次拖动状态：extends 才是"到底画了多少次"的正面证据。
    const during = JSON.parse(await run(`
      const H = window.__heditor;
      return JSON.stringify({ drag: H.drag, rects: H.selection.rects.length });`));
    const t1 = Date.now();
    await mouse('mouseReleased', geo.midX, geo.top + 300, 0);
    await run('return await window.__heditor.dragSettled();');
    const settleMs = Date.now() - t1;
    console.log(`  按下后 drag=${JSON.stringify(afterPress.drag)} 页面收到 mousemove ${afterPress.mm} 次`);
    console.log(`  连发 40 次移动之后：拖选又画了 ${last} 次，共 ${drainMs} ms 才停` +
                `  松手到静止 ${settleMs} ms`);
    console.log(`  拖动中的状态 ${JSON.stringify(during.drag)}` +
                `  选区矩形 ${during.rects} 条`);
  }

  // 量 G：边缘自动滚动（指针停在容器下缘之外）
  //   与 [20](i) 同一件事：按下在正文上，指针移到容器外，容器应当自己滚。
  console.log('\n=== G. 边缘自动滚动 ===');
  {
    const g = JSON.parse(await run(`
      const H = window.__heditor;
      const cv = H.canvas, r = cv.getBoundingClientRect();
      const s = document.getElementById('stage').getBoundingClientRect();
      const c = H.caret;
      return JSON.stringify({
        press: { x: Math.round(r.left + (c.x + 1) * (r.width / cv.width)),
                 y: Math.round(r.top + (c.y + c.h / 2) * (r.height / cv.height)) },
        outY: Math.round(s.bottom + 30), outX: Math.round((s.left + s.right) / 2),
        stageTop: s.top, stageBottom: s.bottom,
        viewH: window.innerHeight, viewW: window.innerWidth,
        scrollTop: document.getElementById('stage').scrollTop });`));
    console.log(`  按下点 ${JSON.stringify(g.press)}  容器外点 ` +
                `(${g.outX},${g.outY})  容器 ${Math.round(g.stageTop)}..${Math.round(g.stageBottom)}` +
                `  视口 ${g.viewW}×${g.viewH}`);
    await mouse('mousePressed', g.press.x, g.press.y, 1);
    await mouse('mouseMoved', g.outX, g.outY, 1);
    await sleep(300);
    const a = JSON.parse(await run(`
      const H = window.__heditor;
      return JSON.stringify({ drag: H.drag, mm: window.__mm || 0,
        scrollTop: document.getElementById('stage').scrollTop });`));
    await sleep(700);
    const b = JSON.parse(await run(`
      const H = window.__heditor;
      return JSON.stringify({ drag: H.drag, mm: window.__mm || 0,
        scrollTop: document.getElementById('stage').scrollTop });`));
    await mouse('mouseReleased', g.outX, g.outY, 0);
    console.log(`  移到容器外 300ms：scrollTop ${a.scrollTop}  drag=${JSON.stringify(a.drag)}` +
                `  页面累计收到 mousemove ${a.mm} 次`);
    console.log(`  再等 700ms：scrollTop ${b.scrollTop}  drag=${JSON.stringify(b.drag)}` +
                `  页面累计收到 mousemove ${b.mm} 次`);
  }

  // H. 平移复用的底图自审：逐页下滚，每滚一页就比一次"底图 vs 从零全建"。
  console.log('\n=== H. 平移复用的底图自审 ===');
  {
    const rows = JSON.parse(await run(`
      const H = window.__heditor;
      const stage = document.getElementById('stage');
      const dpr = window.devicePixelRatio || 1;
      const step = (H.pagePx[1] + H.pagePx[2]) / dpr;
      const wait = () => new Promise((r) => requestAnimationFrame(
        () => requestAnimationFrame(r)));
      const out = [];
      stage.scrollTop = 0;
      await wait();
      for (let k = 1; k <= 3; ++k) {
        stage.scrollTop = k * step;
        await wait();
        const st = H.editStats;
        const ab = await H.auditBase();
        out.push({ k, win: st.windowFirstPage + '+' + st.windowPages,
                   repainted: st.repaintedPages, diff: ab.diff, box: ab.box });
      }
      return JSON.stringify(out);`));
    for (const r of rows) {
      console.log(`  滚到第 ${r.k} 页：窗口 ${r.win} 重画页 ${r.repainted}` +
                  `  底图自审 diff=${r.diff} 框=${JSON.stringify(r.box)}`);
    }
  }

  // 量 I：单步拖选的开销分解（逐次移动，每次等它处理完再读账）
  //   与 F 不同：F 连发不等，量的是"积压了多少拍"；这里每次只走一步，
  //   量的是"一步要花多少、花在哪一层"。读数全部取宿主与引擎自己记的账，
  //   不含 CDP 往返 —— 同一条命令发一次与发一百次，读数应当一样。
  console.log('\n=== I. 单步拖选的开销分解 ===');
  {
    await run('await window.__heditor.loadStressDocument(200, 20); return true;');
    await sleep(600);
    await run('document.getElementById("stage").scrollTop = 0; return true;');
    await sleep(200);
    await run('return await window.__heditor.setCaret(0, 0);');
    await sleep(80);
    // 往返基线：一个什么都不做的表达式值多少毫秒。读数若与它同量级，
    //   说明量到的其实是往返，不是这一层要花的钱。
    const rtt = [];
    for (let i = 0; i < 5; ++i) {
      const t = Date.now();
      await c.eval('return 1;');
      rtt.push(Date.now() - t);
    }
    const g = JSON.parse(await run(`
      const H = window.__heditor;
      const cv = H.canvas, r = cv.getBoundingClientRect();
      const s = document.getElementById('stage').getBoundingClientRect();
      const c = H.caret;
      return JSON.stringify({ cx: r.left + c.x * (r.width / cv.width),
        cy: r.top + (c.y + c.h / 2) * (r.height / cv.height),
        midX: (s.left + s.right) / 2, top: s.top, bottom: s.bottom,
        stageH: s.bottom - s.top });`));
    console.log(`  视口高 ${g.stageH} CSS px，按下点 y=${Math.round(g.cy)}`);
    await mouse('mousePressed', g.cx, g.cy, 1);
    await sleep(80);
    const rows = [];
    for (let i = 0; i < 24; ++i) {
      const y = g.cy + 12 * (i + 1);
      await mouse('mouseMoved', g.midX + ((i % 2) ? 24 : -24), y, 1);
      await sleep(0);
      // 两套宿主的账形状不同：单线程给 settleMs / putMs（它自己上屏），
      //   Worker 给 stateMs（那一拍在 Worker 里花的时间）。都读，按名字认领。
      const r = JSON.parse(await run(`
        const H = window.__heditor;
        const bl = H.blitStats || {}, st = H.editStats, ss = H.stats;
        const sel = H.selection || {}, dg = H.drag || {};
        return JSON.stringify({ settle: bl.settleMs, win: bl.windowMs,
          frame: bl.frameMs, image: bl.imageMs, put: bl.putMs, blit: bl.totalMs,
          pages: bl.pages, stateMs: bl.stateMs, seq: bl.seq,
          paint: st.paintMs, layout: st.layoutMs, pageFp: st.pageFpMs,
          repainted: st.repaintedPages, windowPages: st.windowPages,
          present: ss.paintPresentMs, fill: ss.paintFillMs, glyph: ss.paintGlyphMs,
          rects: sel.rects ? sel.rects.length : -1, ex: dg.extends || 0 });`));
      rows.push(r);
    }
    await mouse('mouseReleased', g.midX, g.cy + 300, 0);
    await run('return await window.__heditor.dragSettled();');
    const med = (k) => q(rows.map((r) => Number(r[k]) || 0), 0.5);
    const top = (k) => Math.max(...rows.map((r) => Number(r[k]) || 0));
    console.log(`  CDP 往返基线 中位 ${q(rtt, 0.5)} ms / 最小 ${Math.min(...rtt)} ms`);
    console.log(`  一步宿主总账 settle 中位 ${med('settle').toFixed(2)} / 最大 ${top('settle').toFixed(2)} ms` +
                `  Worker 那一拍 stateMs 中位 ${med('stateMs').toFixed(2)} / 最大 ${top('stateMs').toFixed(2)} ms`);
    console.log(`    窗口对齐 ${med('win').toFixed(2)}  上屏 ${med('blit').toFixed(2)}` +
                `（取帧 ${med('frame').toFixed(2)} / ImageData ${med('image').toFixed(2)}` +
                ` / putImageData ${med('put').toFixed(2)}）  贴 ${med('pages')} 页`);
    console.log(`  引擎 present 中位 ${med('present').toFixed(2)}（铺面积 ${med('fill').toFixed(2)}` +
                ` + 字形 ${med('glyph').toFixed(2)}）  绘制合计 ${med('paint').toFixed(2)}` +
                `（排版 ${med('layout').toFixed(2)} / 页指纹 ${med('pageFp').toFixed(2)}）`);
    console.log(`  选区矩形 ${med('rects')} 条  重画页 ${med('repainted')} / 窗口 ${med('windowPages')}` +
                `  extends ${rows.length ? rows[rows.length - 1].ex : 0}`);
    console.log('  前 8 步 settle/stateMs/present/put：' + rows.slice(0, 8).map((r) =>
      `${Number(r.settle).toFixed(1)}/${Number(r.stateMs).toFixed(1)}/` +
      `${Number(r.present).toFixed(1)}/${Number(r.put).toFixed(1)}`).join('  '));

    // 端到端：一次拖选移动 = 一次命中往返 + 一条命令。两段都在关键路径上，
    //   而 drag_select 的 pump 串行化它们（一次命中没回来，下一个点只排队）。
    //   门面 ask 只在异步宿主上有 —— 单线程宿主同步命中，这一段跳过。
    const hasAsk = await ev('typeof window.__heditor.ask === "function"');
    if (hasAsk) {
      console.log('  端到端分解（命中往返 / 命令往返）：');
      const e2e = [];
      for (let i = 0; i < 10; ++i) {
        const t = JSON.parse(await run(`
          const H = window.__heditor;
          const t0 = performance.now();
          const hit = await H.ask('hit', { x: 60, y: 60 + ${i} * 6 });
          const t1 = performance.now();
          if (hit) await H.setFocusAtSide(hit.para, hit.offset, hit.side);
          const t2 = performance.now();
          return JSON.stringify({ hit: t1 - t0, cmd: t2 - t1, total: t2 - t0 });`));
        e2e.push(t);
      }
      const em = (k) => q(e2e.map((r) => Number(r[k]) || 0), 0.5);
      const et = (k) => Math.max(...e2e.map((r) => Number(r[k]) || 0));
      console.log(`    命中 中位 ${em('hit').toFixed(2)} / 最大 ${et('hit').toFixed(2)} ms` +
                  `   命令 中位 ${em('cmd').toFixed(2)} / 最大 ${et('cmd').toFixed(2)} ms` +
                  `   合计 中位 ${em('total').toFixed(2)} / 最大 ${et('total').toFixed(2)} ms`);
    }
  }

  // 量 K：真实拖动（约 120 Hz 的移动）下的追尾
  //   与 I 的区别：I 每步都等它处理完（量"一步要花多少"），K 不等（量"一个接一个
  //   来的时候画面落后多少、停手之后还要追多久"）。页面侧每帧记一次 extends，
  //   于是"最后一次移动到达"与"最后一次真的画出来"之间的差就是追尾。
  console.log('\n=== K. 真实拖动下的追尾 ===');
  {
    await run('await window.__heditor.loadStressDocument(200, 20); return true;');
    await sleep(600);
    await run('document.getElementById("stage").scrollTop = 0; return true;');
    await sleep(200);
    await run('return await window.__heditor.setCaret(0, 0);');
    await sleep(80);
    await run(`
      window.__klog = []; window.__kmm = []; window.__krun = true;
      window.addEventListener('mousemove', () => { window.__kmm.push(performance.now()); }, true);
      (function lo() {
        if (!window.__krun) return;
        const d = window.__heditor.drag || {};
        window.__klog.push([performance.now(), d.extends || 0]);
        requestAnimationFrame(lo);
      })();`);
    const g = JSON.parse(await run(`
      const H = window.__heditor;
      const cv = H.canvas, r = cv.getBoundingClientRect();
      const s = document.getElementById('stage').getBoundingClientRect();
      const c = H.caret;
      return JSON.stringify({ cx: r.left + c.x * (r.width / cv.width),
        cy: r.top + (c.y + c.h / 2) * (r.height / cv.height),
        midX: (s.left + s.right) / 2 });`));
    await mouse('mousePressed', g.cx, g.cy, 1);
    await sleep(80);
    const t0 = Date.now();
    for (let i = 0; i < 60; ++i) {
      await mouse('mouseMoved', g.midX + ((i % 2) ? 18 : -18), g.cy + 4 * i, 1);
      await sleep(6);
    }
    const sendMs = Date.now() - t0;
    const nArrived = Number(await ev('window.__kmm.length'));
    const ext0 = Number(await ev('(window.__heditor.drag || {}).extends || 0'));
    // 等它追平：轮询 extends 直到不再增长。不能在这里 await dragSettled ——
    //   它等的是"整个拖选结束"（st 归零），而此刻鼠标还按着，会一直等到抬手。
    let lastSeen = -1;
    let stableFrames = 0;
    const tq = Date.now();
    while (Date.now() - tq < 3000) {
      const n = Number(await ev('(window.__heditor.drag || {}).extends || 0'));
      if (n === lastSeen) { if (++stableFrames >= 5) break; } else { stableFrames = 0; lastSeen = n; }
      await sleep(20);
    }
    const tailMs = Date.now() - tq;
    const ext1 = Number(await ev('(window.__heditor.drag || {}).extends || 0'));
    await mouse('mouseReleased', g.midX, g.cy + 300, 0);
    await run('return await window.__heditor.dragSettled();');
    const res = JSON.parse(await run(`
      window.__krun = false;
      const mm = window.__kmm, log = window.__klog;
      const lastMove = mm.length ? mm[mm.length - 1] : 0;
      let lastInc = 0, maxEx = 0;
      for (const it of log) { if (it[1] > maxEx) { maxEx = it[1]; lastInc = it[0]; } }
      return JSON.stringify({ lastMove, lastInc, maxEx, frames: log.length,
        moves: mm.length });`));
    console.log(`  页面收到 ${nArrived} 次移动，发送耗时 ${sendMs} ms` +
                `（平均 ${(sendMs / Math.max(1, nArrived)).toFixed(1)} ms 一次）`);
    console.log(`  停下时 extends=${ext0}，追平后 ${ext1}（共 ${res.maxEx} 步）  取样 ${res.frames} 帧`);
    console.log(`  停止发送到追平 ${tailMs} ms；最后一次移动到达后` +
                `又过 ${(res.lastInc - res.lastMove).toFixed(1)} ms 才画完最后一步`);
  }

  // 量 L：第一次交互能不能拖（用户报"第一次获得焦点时拖不动，要再拖一次"）
  //   页面重载后不做任何别的交互，直接按下拖动 —— 这就是"第一次"。
  //   顺便回答一个影响判据可信度的问题：CDP 派的鼠标事件到底有没有 pointerdown
  //   （没有的话，指针捕获那条路在探针里从未被覆盖）。
  console.log('\n=== L. 第一次交互的拖选 ===');
  {
    await c.send('Page.navigate', { url: URL_ });
    await sleep(1800);
    for (let i = 0; i < 60; ++i) {
      const ok = await ev('!!(window.__heditor && window.__heditor.ready)').catch(() => false);
      if (ok) break;
      await sleep(200);
    }
    await c.eval('return window.__heditor.stopBlink();');
    await run('return await window.__heditor.setCaret(0, 0);');
    await sleep(200);
    const g = JSON.parse(await run(`
      const H = window.__heditor;
      const cv = H.canvas, r = cv.getBoundingClientRect();
      const c = H.caret;
      return JSON.stringify({ cx: r.left + c.x * (r.width / cv.width),
        cy: r.top + (c.y + c.h / 2) * (r.height / cv.height) });`));
    const attempt = async (label) => {
      await run(`
        window.__lrec = [];
        for (const t of ['pointerdown', 'mousedown', 'mousemove', 'mouseup', 'pointerup']) {
          window.addEventListener(t, (e) => window.__lrec.push(t + ':' +
            (typeof e.buttons === 'number' ? e.buttons : '?')), true);
        }
        return true;`);
      await mouse('mousePressed', g.cx, g.cy, 1);
      await sleep(80);
      const a0 = await ev('JSON.stringify(window.__heditor.drag)');
      for (let i = 1; i <= 8; ++i) {
        await mouse('mouseMoved', g.cx + 30 + i * 10, g.cy + 20 + i * 12, 1);
        await sleep(16);
      }
      await sleep(150);
      const a1 = JSON.parse(await run(`
        const H = window.__heditor;
        return JSON.stringify({ drag: H.drag, anchor: H.selection.anchor,
          focus: H.selection.focus, rects: H.selection.rects.length });`));
      await mouse('mouseReleased', g.cx + 130, g.cy + 140, 0);
      await run('return await window.__heditor.dragSettled();');
      const rec = JSON.parse(await run('return JSON.stringify(window.__lrec);'));
      const kinds = {};
      for (const r of rec) { const k = r.split(':')[0]; kinds[k] = (kinds[k] || 0) + 1; }
      console.log(`  ${label}：按下后 drag=${a0}`);
      console.log(`     拖动后 drag=${JSON.stringify(a1.drag)}` +
                  `  选区 ${a1.anchor}..${a1.focus}（${a1.rects} 条矩形）`);
      console.log(`     事件 ${JSON.stringify(kinds)}`);
    };
    await attempt('第一次');
    await attempt('第二次');
    // 第三次：先把焦点挪出编辑器（模拟"用户刚点过别处"），再拖。
    //   这一条是"第一次获得焦点时拖不动"的可复现构造：宿主在按下时会 ta.focus()，
    //   焦点从别的元素挪走会派发一次 blur；而 blur 在捕获阶段会经过 window，
    //   拖选若把 blur 当收尾信号，这一下就把刚起来的拖选收掉了。
    const moved = await run(`
      document.body.setAttribute('tabindex', '-1');
      if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
      document.body.focus();
      return document.activeElement === document.body;`);
    console.log(`  把焦点挪到 body：${moved}`);
    await attempt('焦点在 body 时');
  }

  // 量 J：每拍 state 消息里各字段多大
  //   Worker 宿主每次命令都回推一份完整状态，里面的 controls / layout / selection
  //   都是结构化克隆跨线程的对象 —— 这是"一步 2 ms"最可能的去处。
  console.log('\n=== J. 每拍 state 消息的字段大小 ===');
  {
    await run('await window.__heditor.loadStressDocument(200, 20); return true;');
    await sleep(500);
    const sizes = JSON.parse(await run(`
      const H = window.__heditor;
      const L = (v) => { try { return JSON.stringify(v).length; } catch (e) { return -1; } };
      return JSON.stringify({ selection: L(H.selection), controls: L(H.controls),
        stats: L(H.stats), editStats: L(H.editStats), viewport: L(H.viewport),
        rects: (H.selection || {}).rects ? H.selection.rects.length : -1,
        ctlItems: (H.controls || {}).items ? H.controls.items.length : -1 });`));
    for (const k of ['selection', 'controls', 'stats', 'editStats', 'viewport']) {
      console.log(`  ${k}: ${(sizes[k] / 1024).toFixed(1)} KB` +
                  (k === 'selection' ? `（${sizes.rects} 条矩形）` : '') +
                  (k === 'controls' ? `（${sizes.ctlItems} 项控件）` : ''));
    }
  }

  // 量 M：一次移动占主线程多久
  //   分解帧里的账看不出"跟手"卡在哪一段：从浏览器把 mousemove 交给页面，到这一次的
  //   选区真的画进画布，全在同一条任务里跑完。于是在窗口捕获阶段挂两条监听 —— 一条
  //   挂在宿主之前记起点、一条挂在宿主之后记终点，差值就是这条链这一次移动的占用。
  //   另外记 50 ms 以上的长任务：那才是肉眼看到的"卡一下"。
  //
  //   必须在长文档上量：短样例的每一步都太小，量出来的"跟手"是假的。
  console.log('\n=== M. 一次移动占主线程多久 ===');
  {
    const boot = async (setup, dpr) => {
      await c.send('Emulation.setDeviceMetricsOverride',
                   { width: 1500, height: 1200, deviceScaleFactor: dpr || 1, mobile: false });
      await c.send('Page.navigate', { url: URL_ });
      await sleep(1800);
      for (let i = 0; i < 60; ++i) {
        const ok = await ev('!!(window.__heditor && window.__heditor.ready)').catch(() => false);
        if (ok) break;
        await sleep(200);
      }
      await c.eval('return window.__heditor.stopBlink();');
      if (setup) await run(setup);
      // 光标摆在文档开头：起点一致，各次之间才可比。
      await run('return await window.__heditor.setCaret(0, 0);');
      await sleep(150);
      return JSON.parse(await run(`
        const H = window.__heditor;
        const cv = H.canvas, r = cv.getBoundingClientRect();
        const c = H.caret;
        return JSON.stringify({ cx: r.left + c.x * (r.width / cv.width),
          cy: r.top + (c.y + c.h / 2) * (r.height / cv.height) });`));
    };
    const oneDrag = async (label, setup, dpr) => {
      const g = await boot(setup, dpr);
      await run(`
        window.__mt = { pre: [], post: [], long: [], fr: [] };
        window.addEventListener('mousemove', () => window.__mt.pre.push(performance.now()), true);
        try {
          new PerformanceObserver((l) => {
            for (const e of l.getEntries()) window.__mt.long.push(e.duration);
          }).observe({ entryTypes: ['longtask'] });
        } catch (e) { window.__mt.noLong = true; }
        return true;`);
      await mouse('mousePressed', g.cx, g.cy, 1);
      await sleep(120);
      // 终点监听必须在按下之后才挂：begin() 每次都把宿主的移动监听摘掉重挂，
      //   挂到最新一处，所以按下前挂的监听会排到它前面，"终点"就不是终点了。
      await run(`window.addEventListener('mousemove', () => window.__mt.post.push(performance.now()), true);
        return true;`);
      // 帧节拍：每一步都在主线程里把选区写进了画布，剩下的只有合成。合成跟不上
      //   就看帧间隔 —— 它比任何 JS 计时都接近"看起来跟不跟手"。
      await run(`
        window.__mt.stopFr = false;
        (function loop() {
          window.__mt.fr.push(performance.now());
          if (!window.__mt.stopFr) requestAnimationFrame(loop);
        })();
        return true;`);
      for (let i = 0; i < 60; ++i) {
        await mouse('mouseMoved', g.cx + 40 + (i % 7) * 30, g.cy + 60 + (i % 11) * 18, 1);
        await sleep(20);
      }
      await sleep(250);
      await run('window.__mt.stopFr = true; return true;');
      const mt = JSON.parse(await run(`
        const a = window.__mt.pre, b = window.__mt.post;
        const n = Math.min(a.length, b.length);
        const d = [];
        for (let i = 0; i < n; ++i) d.push(b[i] - a[i]);
        d.sort((x, y) => x - y);
        const fr = window.__mt.fr, iv = [];
        for (let i = 1; i < fr.length; ++i) iv.push(fr[i] - fr[i - 1]);
        iv.sort((x, y) => x - y);
        let over = 0;
        for (const v of iv) if (v > 25) over++;
        const H = window.__heditor;
        return JSON.stringify({ pre: a.length, post: b.length, frames: iv.length, over,
          frMed: iv.length ? Number(iv[(iv.length / 2) | 0].toFixed(1)) : -1,
          frMax: iv.length ? Number(iv[iv.length - 1].toFixed(1)) : -1,
          med: d.length ? Number(d[(d.length / 2) | 0].toFixed(2)) : -1,
          max: d.length ? Number(d[d.length - 1].toFixed(2)) : -1,
          rects: H.selection.rects.length, extends: (H.drag || {}).extends || 0,
          long: window.__mt.long.map((v) => Number(v.toFixed(1))), noLong: !!window.__mt.noLong });`));
      await mouse('mouseReleased', g.cx + 100, g.cy + 200, 0);
      await run('return await window.__heditor.dragSettled();');
      console.log(`  ${label}：移动 ${mt.pre} 次全收到，落 ${mt.extends} 次` +
                  `  选区 ${mt.rects} 条矩形`);
      console.log(`     宿主处理一次移动占主线程：中位 ${mt.med} ms / 最大 ${mt.max} ms` +
                  `  50 ms 以上长任务 ${mt.long.length} 个` +
                  (mt.long.length ? `：${mt.long.join(', ')}` : '') +
                  (mt.noLong ? '（本浏览器不支持 longtask）' : ''));
      console.log(`     拖动期间的帧间隔：中位 ${mt.frMed} ms / 最大 ${mt.frMax} ms` +
                  `  超过 25 ms 的 ${mt.over} 帧（共 ${mt.frames} 帧）`);
    };
    await oneDrag('短样例·小选区', null, 1);
    await oneDrag('长文档·小选区', 'return await window.__heditor.run(7, 1.5);', 1);
    await oneDrag('长文档·整篇已选', `await window.__heditor.run(7, 1.5);
      return await window.__heditor.selectAll();`, 1);
    // 帧是设备像素，窗口尺寸按 DPR 折算 —— 同样的手势，DPR 1.5 的机器要搬 2.25 倍
    //   （DPR 2 是 4 倍）的像素。这一臂用来看"跟不跟手"对 DPR 有多敏感。
    await oneDrag('长文档·整篇已选·DPR 1.5', `await window.__heditor.run(7, 1.5);
      return await window.__heditor.selectAll();`, 1.5);
    await oneDrag('长文档·整篇已选·DPR 2', `await window.__heditor.run(7, 1.5);
      return await window.__heditor.selectAll();`, 2);
    await c.send('Emulation.setDeviceMetricsOverride',
                 { width: 1500, height: 1200, deviceScaleFactor: 1, mobile: false });
  }

  // 量 N：拖选的收尾信号
  //   元素级失焦不能收尾 —— 宿主在按下的同一条链里把焦点搬进输入框，那一下会把刚起的
  //   拖选收掉；整个窗口失焦必须收尾 —— 那之后 mouseup 不会再到达页面。
  //   两条一起验：只验前一条，把收尾信号整体删掉也会被判成对的。
  console.log('\n=== N. 拖选的收尾信号 ===');
  {
    await c.send('Page.navigate', { url: URL_ });
    await sleep(1800);
    for (let i = 0; i < 60; ++i) {
      const ok = await ev('!!(window.__heditor && window.__heditor.ready)').catch(() => false);
      if (ok) break;
      await sleep(200);
    }
    await c.eval('return window.__heditor.stopBlink();');
    await run('return await window.__heditor.setCaret(0, 0);');
    await sleep(150);
    const g = JSON.parse(await run(`
      const H = window.__heditor;
      const cv = H.canvas, r = cv.getBoundingClientRect();
      const c = H.caret;
      return JSON.stringify({ cx: r.left + c.x * (r.width / cv.width),
        cy: r.top + (c.y + c.h / 2) * (r.height / cv.height) });`));
    const active = () => ev('!!(window.__heditor.drag && window.__heditor.drag.active)');
    await mouse('mousePressed', g.cx, g.cy, 1);
    await sleep(80);
    const b1 = await active();
    await run("window.dispatchEvent(new Event('blur')); return true;");
    await sleep(80);
    const a1 = await active();
    await mouse('mouseReleased', g.cx, g.cy, 0);
    await sleep(60);
    await mouse('mousePressed', g.cx, g.cy, 1);
    await sleep(80);
    const b2 = await active();
    await run(`
      const d = document.createElement('input');
      document.body.appendChild(d);
      d.focus();
      d.blur();
      d.remove();
      return true;`);
    await sleep(80);
    const a2 = await active();
    await mouse('mouseReleased', g.cx, g.cy, 0);
    await sleep(60);
    console.log(`  窗口失焦：拖选中 ${b1} -> 收尾后 ${a1}（期望 true -> false）`);
    console.log(`  元素失焦：拖选中 ${b2} -> 其他元素失焦后 ${a2}（期望 true -> true）`);
  }

  c.ws.close();
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e && e.stack ? e.stack : e);
  process.exit(1);
});
