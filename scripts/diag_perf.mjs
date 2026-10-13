// 诊断：长文档编辑卡在哪一层 —— 引擎（排版 / 光栅 / 帧组装）与宿主（跨界 / 上屏）分开记。
//
// 为什么要分开：引擎的 paintMs 到"帧缓冲组装完"为止，之后还有取视图、构造 ImageData、
//   putImageData 三段，面板此前没有这三段的数字。而画布是整篇大小的单张位图，
//   putImageData 花多久取决于浏览器要不要把整张画布重新合成，不取决于脏矩形多小 ——
//   这两笔账混在一起，就只能得到一句"上屏慢"，无法判断该改哪一边。
//
// 第二件事：画布面积对照实验。同样贴一小块，分别贴进"整篇大小的画布"与"一页大小的
//   画布"，两者之差就是"单一巨型画布"这一设计本身的代价。
//
// 用法（先起服务与带调试端口的 Chrome）：
//   node scripts/diag_perf.mjs --port 9222 --url http://127.0.0.1:8781/web/index.html
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
const REPS = Number(opt('reps', '8'));

// 页内：连续敲 REPS 个字，每个字记一次"引擎三行 + 宿主三行"，取最小值与均值。
//   最小值才是"这一层真正要花的钱"，均值里掺着调度抖动；两个都印出来是为了看出抖动有多大。
const TYPING = `
return (function () {
  const H = window.__heditor;
  const rows = [];
  for (let i = 0; i < ${REPS}; ++i) {
    const t0 = performance.now();
    H.type('字');
    const t1 = performance.now();
    const st = H.editStats;
    const bl = H.blitStats;
    const ss = H.stats;
    rows.push({
      op: t1 - t0,
      layout: st.layoutMs, pageFp: st.pageFpMs, paint: st.paintMs, engine: st.totalMs,
      fill: ss.paintFillMs, glyph: ss.paintGlyphMs, present: ss.paintPresentMs,
      hits: ss.glyphCacheHits, misses: ss.glyphCacheMisses,
      relaid: st.relaidParagraphs, reused: st.reusedParagraphs,
      totalParas: st.totalParagraphs, full: st.layoutWasFull,
      dirtyPages: st.dirtyPageCount, repaintedPages: st.repaintedPages,
      pixels: st.repaintedPixels, framePx: st.framePixels,
      blitFrame: bl.frameMs, blitImage: bl.imageMs, blitPut: bl.putMs, blitTotal: bl.totalMs,
      blitFull: bl.full, boxW: bl.boxW, boxH: bl.boxH,
    });
  }
  return JSON.stringify(rows);
})()`;

// 页内：画布面积对照实验。
//   往两种画布贴同样大小的一块，各贴 20 次取最小值。
//   如果"整篇画布"明显更贵，那贵的那部分就是画布面积，与引擎无关。
const CANVAS_TEST = `
return (function () {
  const H = window.__heditor;
  const W = H.frameW(), Hh = H.frameH();
  const pageH = H.pagePx ? H.pagePx[1] : H.frameH();
  const bw = 200, bh = 40;
  const data = new Uint8ClampedArray(bw * bh * 4).fill(200);
  const img = new ImageData(data, bw, bh);
  const mk = (w, h) => {
    const c = document.createElement('canvas');
    c.width = w; c.height = h;
    return c.getContext('2d', { alpha: false });
  };
  const big = mk(W, Hh);
  const small = mk(W, pageH);
  const run = (g, w, h) => {
    let best = 1e9;
    for (let i = 0; i < 20; ++i) {
      const t = performance.now();
      g.putImageData(img, 0, 0, (i * 7) % Math.max(1, w - bw), (i * 13) % Math.max(1, h - bh), bw, bh);
      const d = performance.now() - t;
      if (d < best) best = d;
    }
    return best;
  };
  return JSON.stringify({
    frameW: W, frameH: Hh, pageH,
    bigMs: run(big, W, Hh), smallMs: run(small, W, pageH),
  });
})()`;

const min = (rows, k) => Math.min(...rows.map((r) => r[k]));
const avg = (rows, k) => rows.reduce((a, r) => a + r[k], 0) / rows.length;

// 页内：走宿主真实路径的一次"敲一个字"，返回这一轮的账。
//   与 TYPING 的区别是只做一次，用于量非稳态的那些位置（首段、末段、撤销）。
const ONE_TYPE = `
return (function () {
  const H = window.__heditor;
  const t0 = performance.now();
  H.type('字');
  const op = performance.now() - t0;
  const st = H.editStats;
  const bl = H.blitStats;
  const ss = H.stats;
  return JSON.stringify({
    op, layout: st.layoutMs, pageFp: st.pageFpMs, paint: st.paintMs, engine: st.totalMs,
    fill: ss.paintFillMs, glyph: ss.paintGlyphMs, present: ss.paintPresentMs,
    hits: ss.glyphCacheHits, misses: ss.glyphCacheMisses, cacheEntries: ss.glyphCacheEntries,
    relaid: st.relaidParagraphs, reused: st.reusedParagraphs, totalParas: st.totalParagraphs,
    full: st.layoutWasFull, shifted: st.suffixShifted,
    dirtyPages: st.dirtyPageCount, repaintedPages: st.repaintedPages,
    pixels: st.repaintedPixels, framePx: st.framePixels, totalPages: st.totalPages,
    blitPut: bl.putMs, blitTotal: bl.totalMs, blitFull: bl.full,
  });
})()`;

// 页内：连敲 REPS 个字，同时用 requestAnimationFrame 记帧间隔，并把每一拍的
// 操作耗时一起记下来。
//   帧间隔是"手感"：rAF 回调的间隔就是屏幕真正刷新的节拍，主线程算得再快、
//   只要合成跟不上，帧间隔一样会被拉长。两者一起看才分得清"是算得慢"还是"刷不上"。
const FPS_BURST = `
return (function () {
  const H = window.__heditor;
  const gaps = [];
  const ops = [];
  const hosts = [];
  let last = 0;
  let n = 0;
  let typing = true;
  const tick = (ts) => {
    if (last) gaps.push(ts - last);
    last = ts;
    if (n < ${REPS}) {
      ++n;
      const t0 = performance.now();
      H.type('字');
      ops.push(performance.now() - t0);
      hosts.push(H.blitStats.settleMs || 0);
      requestAnimationFrame(tick);
    } else {
      typing = false;
    }
  };
  requestAnimationFrame(tick);
  return new Promise((resolve) => {
    const iv = setInterval(() => {
      if (!typing && gaps.length >= ${REPS}) {
        clearInterval(iv);
        const q = (a, p) => {
          const s = a.slice().sort((x, y) => x - y);
          return s[Math.min(s.length - 1, Math.floor(s.length * p))];
        };
        resolve(JSON.stringify({
          frames: gaps.length,
          gapP50: q(gaps, 0.5), gapP90: q(gaps, 0.9), gapMax: q(gaps, 1),
          gapAvg: gaps.reduce((a, b) => a + b, 0) / gaps.length,
          opP50: q(ops, 0.5), opP90: q(ops, 0.9), opMax: q(ops, 1),
          hostP50: q(hosts, 0.5),
          slowFrames: gaps.filter((g) => g > 20).length,
        }));
      }
    }, 20);
  });
})()`;

function line(tag, rows) {
  const r = rows[rows.length - 1];
  console.log(`  ${tag.padEnd(14)}` +
              ` 引擎 ${min(rows, 'engine').toFixed(2)} ms` +
              `（layout ${min(rows, 'layout').toFixed(2)} / pageFp ${min(rows, 'pageFp').toFixed(2)}` +
              ` / paint ${min(rows, 'paint').toFixed(2)}` +
              ` = 铺面积 ${min(rows, 'fill').toFixed(2)} + 字形 ${min(rows, 'glyph').toFixed(2)}` +
              ` + 帧搬运 ${min(rows, 'present').toFixed(2)}）` +
              `  上屏 ${min(rows, 'blitTotal').toFixed(2)} ms` +
              `  操作合计 ${min(rows, 'op').toFixed(2)} ms（均值 ${avg(rows, 'op').toFixed(2)}）`);
  console.log(`  ${''.padEnd(14)}` +
              ` 账：重排段 ${r.relaid}/${r.totalParas}` +
              `  脏页 ${r.dirtyPages}  重画 ${r.repaintedPages} 页 / ${min(rows, 'pixels')} px` +
              `（占全帧 ${(100 * min(rows, 'pixels') / r.framePx).toFixed(3)} %）` +
              `  布局${r.full ? '全量' : '增量'}` +
              `  字形缓存 ${min(rows, 'hits')} 命中 / ${min(rows, 'misses')} 未命中`);
}

async function main() {
  const target = await CDP.findPageTarget(PORT);
  const c = await CDP.connect(target.webSocketDebuggerUrl);
  await c.send('Page.enable');
  await c.send('Runtime.enable');
  await c.disableHttpCache();
  await c.send('Emulation.setDeviceMetricsOverride',
               { width: 1500, height: 1200, deviceScaleFactor: 1, mobile: false });
  await c.send('Page.navigate', { url: URL_ });
  await c.waitFor('return !!(window.__heditor && window.__heditor.ready)', 'ready', 60000);
  await c.eval('return window.__heditor.stopBlink();');

  for (const paras of [200, 1000, 3000]) {
    await c.eval(`return window.__heditor.loadStressDocument(${paras}, 20);`);
    await c.eval('return window.__heditor.stopBlink();');
    await sleep(200);
    const s = await c.eval(`
      const st = window.__heditor.editStats;
      return JSON.stringify({ pages: st.totalPages, fw: window.__heditor.frameW(),
                              fh: window.__heditor.frameH(), doc: st.docCodepoints,
                              layout: st.layoutMs, paint: st.paintMs });`);
    const o = JSON.parse(s);
    console.log(`\n=== 压力 ${paras} 段 × 20 字 ===`);
    console.log(`  装载：${o.pages} 页  帧 ${o.fw} × ${o.fh} px` +
                `（${(o.fw * o.fh / 1e6).toFixed(2)} Mpx）  码点 ${o.doc}  ` +
                `全量排版 ${o.layout.toFixed(1)} ms  全量绘制 ${o.paint.toFixed(1)} ms`);

    // 把光标放到文档中段（避开"末尾追加"与"首段无前缀可复用"两个特例）
    await c.eval(`return window.__heditor.setCaret(${Math.floor(paras / 2)}, 5);`);
    await c.eval('return window.__heditor.stopBlink();');
    await sleep(120);
    line('中段敲字', JSON.parse(await c.eval(TYPING)));

    // 文档末尾敲字：后缀为空、无需平移，是增量最好走的一档
    await c.eval(`return window.__heditor.setCaret(${paras - 1}, 20);`);
    await c.eval('return window.__heditor.stopBlink();');
    await sleep(120);
    line('末段敲字', JSON.parse(await c.eval(TYPING)));
  }

  console.log('\n=== 画布面积对照（同样贴 200×40 一块，20 次取最小）===');
  const cv = JSON.parse(await c.eval(CANVAS_TEST));
  console.log(`  整篇画布 ${cv.frameW} × ${cv.frameH} = ${(cv.frameW * cv.frameH / 1e6).toFixed(2)} Mpx` +
              `  →  ${cv.bigMs.toFixed(3)} ms/次`);
  console.log(`  单页画布 ${cv.frameW} × ${cv.pageH} = ${(cv.frameW * cv.pageH / 1e6).toFixed(2)} Mpx` +
              `  →  ${cv.smallMs.toFixed(3)} ms/次`);
  console.log(`  倍数 ${(cv.bigMs / Math.max(1e-6, cv.smallMs)).toFixed(1)} 倍`);

  // 演示场景：6 = 可编辑单段，7 = 长文档 200 段（截图里那一份）
  for (const scen of [7, 6]) {
    const load = JSON.parse(await c.eval(`
      const t0 = performance.now();
      const st = window.__heditor.run(${scen}, 1.5);
      const ms = performance.now() - t0;
      return JSON.stringify({ ms, pages: st.totalPages, relaid: st.relaidParagraphs,
                              total: st.totalParagraphs, lines: st.totalLines,
                              layout: st.layoutMs, paint: st.paintMs,
                              pixels: st.repaintedPixels, framePx: st.framePixels,
                              fw: window.__heditor.frameW(), fh: window.__heditor.frameH() });`));
    console.log(`\n=== 演示场景 ${scen}（zoom 1.5）装载 = ${load.ms.toFixed(0)} ms ===`);
    console.log(`  ${load.pages} 页 / ${load.total} 段 / ${load.lines} 行` +
                `  帧 ${load.fw} × ${load.fh} = ${(load.framePx / 1e6).toFixed(2)} Mpx`);
    console.log(`  装载记账：重排 ${load.relaid}/${load.total} 段  排版 ${load.layout.toFixed(1)} ms` +
                `  绘制 ${load.paint.toFixed(1)} ms  重画 ${load.pixels} px` +
                `（占全帧 ${(100 * load.pixels / load.framePx).toFixed(3)} %）`);

    await c.eval('return window.__heditor.stopBlink();');
    const spots = [
      ['首段(无前缀可复用)', 0, 5],
      ['中段', Math.floor(load.total / 2), 5],
      ['末段', load.total - 1, 5],
    ];
    for (const [tag, para, off] of spots) {
      await c.eval(`return window.__heditor.setCaret(${para}, ${off});`);
      await c.eval('return window.__heditor.stopBlink();');
      await sleep(120);
      const r = JSON.parse(await c.eval(ONE_TYPE));
      console.log(`  ${tag.padEnd(20)} 操作 ${r.op.toFixed(1)} ms` +
                  `（引擎 layout ${r.layout.toFixed(1)} / pageFp ${r.pageFp.toFixed(1)}` +
                  ` / paint ${r.paint.toFixed(1)} = 铺面积 ${r.fill.toFixed(1)}` +
                  ` + 字形 ${r.glyph.toFixed(1)} + 帧搬运 ${r.present.toFixed(1)}，上屏 ${r.blitTotal.toFixed(1)}）` +
                  `  重排 ${r.relaid}/${r.totalParas}${r.full ? ' 全量' : ' 增量'}` +
                  `${r.shifted ? ' 平移' : ''}` +
                  `  脏页 ${r.dirtyPages}  重画 ${r.pixels} px` +
                  `（占全帧 ${(100 * r.pixels / r.framePx).toFixed(3)} %）` +
                  `  缓存 ${r.hits}/${r.misses}`);
    }

    // 屏幕真正刷新的节拍：连敲 REPS 个字，记 rAF 间隔
    await c.eval(`return window.__heditor.setCaret(${Math.floor(load.total / 2)}, 5);`);
    await c.eval('return window.__heditor.stopBlink();');
    await sleep(120);
    const fps = JSON.parse(await c.eval(FPS_BURST));
    console.log(`  连敲 ${fps.frames} 字      帧间隔 p50 ${fps.gapP50.toFixed(1)}` +
                ` / p90 ${fps.gapP90.toFixed(1)} / max ${fps.gapMax.toFixed(1)} ms` +
                `（>20ms 的慢帧 ${fps.slowFrames}）` +
                `  单拍操作 p50 ${fps.opP50.toFixed(1)} / p90 ${fps.opP90.toFixed(1)}` +
                ` / max ${fps.opMax.toFixed(1)} ms` +
                `（宿主收尾 p50 ${fps.hostP50.toFixed(1)}）`);

    // 光标闪烁那一拍：底图一个字都没变，只贴光标
    await c.eval(`return window.__heditor.setCaret(${Math.floor(load.total / 2)}, 5);`);
    await c.eval('return window.__heditor.stopBlink();');
    await sleep(120);
    const blink = JSON.parse(await c.eval(`
      const H = window.__heditor;
      const rows = [];
      for (let i = 0; i < 6; ++i) {
        const t0 = performance.now();
        H.present();
        const d = performance.now() - t0;
        rows.push({ d, pixels: H.editStats.repaintedPixels, put: H.blitStats.putMs,
                    blit: H.blitStats.totalMs, full: H.blitStats.full });
      }
      return JSON.stringify(rows);`));
    console.log(`  光标闪烁一拍        ${min(blink, 'd').toFixed(2)} ms` +
                `（上屏 ${min(blink, 'blit').toFixed(2)}）  重画 ${blink[0].pixels} px` +
                `  整帧? ${blink[0].full}`);
  }

  // 合成代价：同一串敲字，画布可见与不可见各跑一遍。
  //   两次的引擎侧与 putImageData 完全相同，差的只有"浏览器要不要把这张画布合成上屏"。
  //   这个差就是画布面积本身要付的钱 —— 它不在任何引擎计时里，也不在 putImageData 里。
  console.log('\n=== 合成代价：同一串敲字，画布可见 vs 不可见 ===');
  for (const scen of [7, 6]) {
    await c.eval(`return window.__heditor.run(${scen}, 1.5);`);
    await c.eval('return window.__heditor.stopBlink();');
    await sleep(200);
    const info = JSON.parse(await c.eval(
      'return JSON.stringify({ paras: window.__heditor.editStats.totalParagraphs,' +
      ' fw: window.__heditor.frameW(), fh: window.__heditor.frameH() });'));
    await c.eval(`return window.__heditor.setCaret(${Math.floor(info.paras / 2)}, 5);`);
    await c.eval('return window.__heditor.stopBlink();');
    await sleep(120);
    const vis = JSON.parse(await c.eval(FPS_BURST));
    await c.eval("window.__heditor.canvas.style.visibility = 'hidden'; return true;");
    await sleep(60);
    const hid = JSON.parse(await c.eval(FPS_BURST));
    await c.eval("window.__heditor.canvas.style.visibility = ''; return true;");
    console.log(`  场景 ${scen}（帧 ${info.fw} × ${info.fh}` +
                ` = ${(info.fw * info.fh / 1e6).toFixed(2)} Mpx）` +
                `  可见 p50 ${vis.gapP50.toFixed(1)} ms  不可见 p50 ${hid.gapP50.toFixed(1)} ms` +
                `  差 ${(vis.gapP50 - hid.gapP50).toFixed(1)} ms` +
                `（可见慢帧 ${vis.slowFrames}/${vis.frames}，不可见 ${hid.slowFrames}/${hid.frames}）`);
  }

  // 合成代价 vs 画布面积：固定"1 页窗口"，扫 zoom 改画布大小，各量一次
  //   可见/不可见。两个点定不了曲线，而这条曲线的形状决定"窗口留几页"。
  console.log('\n=== 合成代价 vs 画布面积（1 页窗口，扫 zoom）===');
  for (const z of [1.0, 1.5, 2.0, 2.5, 3.0]) {
    await c.eval(`return window.__heditor.run(7, ${z});`);
    await c.eval('return window.__heditor.stopBlink();');
    await sleep(150);
    // 光标放到靠前的段，保证窗口停在开头那一页
    await c.eval('return window.__heditor.setCaret(20, 5);');
    await c.eval('return window.__heditor.stopBlink();');
    await sleep(120);
    const info = JSON.parse(await c.eval(
      'return JSON.stringify({ fw: window.__heditor.frameW(), fh: window.__heditor.frameH(),' +
      ' wp: window.__heditor.editStats.windowPages });'));
    const vis = JSON.parse(await c.eval(FPS_BURST));
    await c.eval("window.__heditor.canvas.style.visibility = 'hidden'; return true;");
    await sleep(60);
    const hid = JSON.parse(await c.eval(FPS_BURST));
    await c.eval("window.__heditor.canvas.style.visibility = ''; return true;");
    console.log(`  zoom ${z.toFixed(1)}  帧 ${info.fw} × ${info.fh}` +
                ` = ${(info.fw * info.fh / 1e6).toFixed(2)} Mpx（窗口 ${info.wp} 页）` +
                `  可见 p50 ${vis.gapP50.toFixed(1)} / 不可见 p50 ${hid.gapP50.toFixed(1)}` +
                `  差 ${(vis.gapP50 - hid.gapP50).toFixed(1)} ms` +
                `  操作 p50 ${vis.opP50.toFixed(1)} ms`);
  }

  c.ws.close();
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e && e.stack ? e.stack : e);
  process.exit(1);
});
