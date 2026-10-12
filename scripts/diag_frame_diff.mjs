// 诊断：[2b] 那条"逐个敲 == 一次性插入"的帧差到底差在哪儿。
//
// 两条路各自定格成确定相位后逐字节比帧；差出来的像素按"行内 x 分布"聚一聚，
// 并同时报出两条路上的光标矩形与文档长度 —— 差在光标还是差在文字，一眼能分。
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { CDP, sleep } from './cdp.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};
const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://127.0.0.1:8781/web/index.html');

const ZOOM = 1.5;
const AT = 8;
const RUN = 'ssssss' + 'd'.repeat(15) + 'f'.repeat(14) + 'g'.repeat(25);

const KEY = (k) => `
  window.__heditor.textarea.dispatchEvent(
    new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true, cancelable: true }));
  return true;`;

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

  const resetTo = async () => conn.eval(`
    window.__heditor.reset(6, ${ZOOM});
    window.__heditor.setCaret(0, ${AT});
    return true;`);
  const caret = async () => JSON.parse(await conn.eval(`
    const c = window.__heditor.caret;
    return JSON.stringify({ o: c.offset, s: c.side, x: c.x, y: c.y, w: c.w, h: c.h,
                            vis: c.visible });`));

  await resetTo();
  for (let i = 0; i < RUN.length; ++i) {
    await conn.eval(KEY(RUN[i]));
    await sleep(12);
  }
  await conn.eval('window.__heditor.stopBlink(); window.__heditor.present(); return true;');
  await conn.eval('window.__frameA = window.__heditor.rawFrame(); return true;');
  const caretA = await caret();
  const textA = await conn.eval('return window.__heditor.text;');
  console.log(`A（逐个敲）：文档 ${Array.from(textA).length} 码点  光标 ${caretA.o}/${caretA.s}` +
              ` 矩形 x=${caretA.x} y=${caretA.y} ${caretA.w}x${caretA.h} 可见=${caretA.vis}`);

  await resetTo();
  await conn.eval(`return window.__heditor.type(${JSON.stringify(RUN)});`);
  await conn.eval('window.__heditor.stopBlink(); window.__heditor.present(); return true;');
  const caretB = await caret();
  const textB = await conn.eval('return window.__heditor.text;');
  console.log(`B（一次性）：文档 ${Array.from(textB).length} 码点  光标 ${caretB.o}/${caretB.s}` +
              ` 矩形 x=${caretB.x} y=${caretB.y} ${caretB.w}x${caretB.h} 可见=${caretB.vis}`);

  const d = JSON.parse(await conn.eval(`
    const A = window.__frameA, B = window.__heditor.rawFrame();
    const w = window.__heditor.frameW(), h = window.__heditor.frameH();
    const n = Math.min(A.length, B.length);
    let diff = 0;
    const rows = new Map();
    const cols = new Map();
    let aInk = 0, bInk = 0;
    for (let i = 0; i + 3 < n; i += 4) {
      const p = i >> 2, x = p % w, y = (p - x) / w;
      if (A[i + 3] > 8) ++aInk;
      if (B[i + 3] > 8) ++bInk;
      if (A[i] !== B[i] || A[i + 1] !== B[i + 1] || A[i + 2] !== B[i + 2]) {
        ++diff;
        rows.set(y, (rows.get(y) || 0) + 1);
        cols.set(x, (cols.get(x) || 0) + 1);
      }
    }
    const top = (m) => Array.from(m.entries()).sort((p, q) => q[1] - p[1]).slice(0, 8);
    return JSON.stringify({ diff, w, h, lenA: A.length, lenB: B.length, aInk, bInk,
                            rows: top(rows), cols: top(cols) });`));

  console.log(`\n帧 ${d.w}×${d.h}  字节 A=${d.lenA} B=${d.lenB}  差 ${d.diff} 像素` +
              `  墨迹量 A=${d.aInk} B=${d.bInk}`);
  console.log(`  差异最多的行(y,像素数)：${JSON.stringify(d.rows)}`);
  console.log(`  差异最多的列(x,像素数)：${JSON.stringify(d.cols)}`);
  conn.ws.close();
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e && e.stack ? e.stack : e);
  process.exit(1);
});
