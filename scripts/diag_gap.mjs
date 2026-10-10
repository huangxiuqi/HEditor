// 临时诊断：用真实鼠标点击横扫控件所在的整行，找"点不到"的死区。
//
// 判据：每一次点击都应让光标停在"命中测试给出的那个偏移"上，而且画出来的光标
//   位置就在点击点的附近。凡是"点下去光标没动"或"落点与命中偏移不一致"的 x，
//   就是用户说的"点不进去"。
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
  await conn.waitFor('return !!(window.__ced && window.__ced.ready)', 'ready');

  const geo = async () => conn.eval(`
    const cv = window.__ced.canvas, r = cv.getBoundingClientRect();
    const cs = window.__ced.controls.items;
    let lo = 1e9, hi = -1e9, y = 0;
    for (const it of cs) for (const q of (it.rects || [])) {
      lo = Math.min(lo, q.x); hi = Math.max(hi, q.x + q.w); y = q.y + q.h / 2;
    }
    return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
             zoom: Number(document.getElementById('zoom').value), lo, hi, y };`);

  const clickPt = async (g, xPt, yPt) => {
    const p = { x: g.left + xPt * g.zoom * g.sx, y: g.top + yPt * g.zoom * g.sy,
                button: 'left', clickCount: 1, buttons: 1 };
    await conn.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...p });
    await conn.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...p });
    await sleep(12);
  };

  const sweep = async (variant, tag) => {
    await conn.eval(`return window.__ced.loadFormSample(${variant});`);
    await conn.eval('return window.__ced.stopBlink();');
    const g = await geo();
    console.log(`\n=== ${tag}（variant ${variant}）x∈[${g.lo.toFixed(1)}, ${g.hi.toFixed(1)}] y=${g.y.toFixed(1)} ===`);
    let prev = '';
    for (let x = g.lo - 6; x <= g.hi + 6; x += 0.5) {
      const hit = await conn.eval(
        `return JSON.stringify(window.__ced.hit(${x.toFixed(3)}, ${g.y.toFixed(3)}));`);
      const h = JSON.parse(hit);
      await clickPt(g, x, g.y);
      const st = await conn.eval(`
        const c = window.__ced.caret;
        return JSON.stringify({ off: c.offset, x: c.x, own: window.__ced.controls.caret });`);
      const s = JSON.parse(st);
      const key = `${h === null ? 'null' : h.offset}|${h === null ? '?' : h.field}|${s.off}|${s.own}`;
      if (key !== prev) {
        const bad = h && (s.off !== h.offset) ? '   <<< 落点被改写' : '';
        console.log(`   x=${x.toFixed(1)}  命中=${h ? `${h.offset}/${h.field}` : 'null'}` +
                    `  光标=${s.off} 画在 x=${(s.x / g.zoom).toFixed(1)}pt 面板归属=${s.own}${bad}`);
        prev = key;
      }
    }
  };

  await sweep(3, '三层嵌套（都可编辑）');
  await sweep(2, '三连相邻（都可编辑）');

  // 三连：把后两个字段删空，看区间树
  console.log('\n=== 三连：删空后两个字段 ===');
  await conn.eval('return window.__ced.loadFormSample(2);');
  await conn.eval('return window.__ced.stopBlink();');
  for (const i of [2, 1]) {
    for (let k = 0; k < 6; ++k) {
      const done = await conn.eval(`
        const it = window.__ced.controls.items[${i}];
        if (it.end[1] <= it.start[1]) return true;
        window.__ced.setCaretAt(0, it.end[1], ${i});
        window.__ced.setCaretOnly(0, it.end[1]);
        return false;`);
      if (done) break;
      await conn.eval(key('Backspace'));
    }
    const cs = await conn.eval('return JSON.stringify(window.__ced.controls.items' +
                               '.map((it,i)=>({i, r:[it.start[1],it.end[1]], d:it.depth, p:it.parent})));');
    console.log(`   删空 #${i} 之后：${cs}`);
  }
  conn.ws.close();
  process.exit(0);
}

const key = (k) => `
  window.__ced.textarea.dispatchEvent(
    new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true, cancelable: true }));
  return true;`;

main().catch((e) => {
  console.error('诊断失败：', e && e.stack ? e.stack : e);
  process.exit(1);
});
