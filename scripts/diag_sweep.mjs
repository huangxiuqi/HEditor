// 临时诊断：把两条路径的 "x → 落点偏移" 表并排打出来。
//
//   A = 引擎命中测试（__heditor.hit(x) 直接给帧磅）
//   B = 真实鼠标点击（CDP Input.dispatchMouseEvent，走页面的坐标换算）
//   C = 页面的合成点击（__heditor.clickAt，与 B 同一套换算，只是不经过 CDP）
//
// 三条同时打，就能分清差异出在"坐标换算"还是"命中 → 落点"。
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { CDP, sleep } from './cdp.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
const argv = process.argv.slice(2);
const opt = (n, d) => {
  const i = argv.indexOf('--' + n);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : d;
};
const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://localhost:8781/web/index.html');

async function main() {
  const target = await CDP.findPageTarget(PORT);
  const conn = await CDP.connect(target.webSocketDebuggerUrl);
  await conn.send('Page.enable');
  await conn.send('Runtime.enable');
  await conn.disableHttpCache();
  await conn.send('Emulation.setDeviceMetricsOverride',
                  { width: 1500, height: 1200, deviceScaleFactor: 1, mobile: false });
  await conn.send('Page.navigate', { url: URL_ });
  await conn.waitFor('return !!(window.__heditor && window.__heditor.ready)', 'ready');

  await conn.eval('return window.__heditor.loadFormSample(2);');
  await conn.eval('return window.__heditor.stopBlink();');

  const env = await conn.eval(`
    const cv = window.__heditor.canvas, r = cv.getBoundingClientRect();
    return { left: r.left, top: r.top, w: r.width, h: r.height, cw: cv.width, ch: cv.height,
             zoomUI: Number(document.getElementById('zoom').value),
             zoomLive: window.__heditor.zoomLevel };`);
  console.log('环境：', JSON.stringify(env));

  const y = await conn.eval(`
    const q = window.__heditor.controls.items[0].rects[0];
    return q.y + q.h / 2;`);
  const lo = await conn.eval(
    'return Math.min(...window.__heditor.controls.items.map(i => i.rects[0].x));');
  const hi = await conn.eval(
    'return Math.max(...window.__heditor.controls.items.map(i => i.rects[0].x + i.rects[0].w));');
  console.log(`y=${y.toFixed(2)}  lo=${lo.toFixed(2)}  hi=${hi.toFixed(2)}  zoom=${env.zoomUI}`);

  const z = env.zoomUI;
  const rows = [];
  const reload = async () => {
    await conn.eval('return window.__heditor.loadFormSample(2);');
    await conn.eval('return window.__heditor.stopBlink();');
  };

  // A：命中测试
  const tableA = {};
  for (let x = lo - 4; x <= hi + 4; x += 0.5) {
    const h = await conn.eval(`return window.__heditor.hit(${x.toFixed(2)}, ${y.toFixed(2)});`);
    if (!h) continue;
    const k = `${h.offset}/${h.field}`;
    if (!tableA[k]) tableA[k] = [x, x];
    tableA[k][1] = x;
  }

  // B：真实鼠标
  await reload();
  const tableB = {};
  for (let x = lo - 4; x <= hi + 4; x += 0.5) {
    const p = { x: env.left + x * z * (env.w / env.cw), y: env.top + y * z * (env.h / env.ch),
                button: 'left', clickCount: 1, buttons: 1 };
    await conn.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...p });
    await conn.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...p });
    await sleep(6);
    const st = await conn.eval(
      'return { o: window.__heditor.caret.offset, f: window.__heditor.controls.caret, x: window.__heditor.caret.x };');
    const k = `${st.o}/${st.f}`;
    if (!tableB[k]) tableB[k] = [x, x];
    tableB[k][1] = x;
    if (!rows.length || rows[rows.length - 1].k !== k) rows.push({ k, x, cx: st.x });
  }

  // C：页面合成点击（同一套换算，不经 CDP）
  await reload();
  const tableC = {};
  for (let x = lo - 4; x <= hi + 4; x += 0.5) {
    await conn.eval(`return window.__heditor.clickAt(${(x * z).toFixed(3)}, ${(y * z).toFixed(3)});`);
    const st = await conn.eval(
      'return { o: window.__heditor.caret.offset, f: window.__heditor.controls.caret };');
    const k = `${st.o}/${st.f}`;
    if (!tableC[k]) tableC[k] = [x, x];
    tableC[k][1] = x;
  }

  const fmt = (t) => Object.keys(t).sort().map((k) => {
    const [a, b] = t[k];
    return `${k}:[${a.toFixed(1)},${b.toFixed(1)}]`;
  }).join('  ');

  console.log('\n=== A 命中测试 hit(x) ===');
  console.log(fmt(tableA));
  console.log('\n=== B 真实鼠标点击 ===');
  console.log(fmt(tableB));
  console.log('\n=== C 页面合成点击 clickAt ===');
  console.log(fmt(tableC));
  console.log('\n=== B 的落点序列（x → 偏移/归属，光标x）===');
  for (const r of rows) console.log(`  x=${r.x.toFixed(1)}  →  ${r.k}  光标x=${r.cx.toFixed(1)}`);

  await conn.send('Browser.close').catch(() => {});
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e.message);
  console.error(e.stack || '(无栈)');
  process.exit(2);
});
