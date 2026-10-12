// 诊断：字与"它后面那枚开括号"之间那一段，有没有插入点能落进去。
//
// 判据两条，都落在可证伪的数字上：
//   一，横扫整行时，画出来的光标位置必须跟着点击位置走（偏差不超过一个字的宽度）；
//   二，字段起点那个偏移上，"括号之前"与"括号之后"必须是两个不同的光标位置。
//
// 用法（先用 verify_web.ps1 起服务与 Chrome，或用 -KeepAlive 留着）：
//   node scripts/diag_nested_gap.mjs --port 9222 --url http://127.0.0.1:8781/web/index.html
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

  const load = async (v) => {
    await conn.eval(`return window.__heditor.loadFormSample(${v});`);
    await conn.eval('return window.__heditor.stopBlink();');
  };

  const geo = async () => conn.eval(`
    const cv = window.__heditor.canvas, r = cv.getBoundingClientRect();
    const cs = window.__heditor.controls.items;
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

  await load(3);
  const g = await geo();
  const slots = await conn.eval(
    'return JSON.stringify((window.__heditor.bracketSlots || []).filter(s => s.para === 0));');
  console.log(`\n=== variant 3 三层嵌套：占位槽（帧磅）===`);
  for (const s of JSON.parse(slots)) {
    console.log(`   cp=${String(s.cp).padStart(2)} ${s.side === 1 ? '[' : ']'} ` +
                `归属=${s.owner}  x=${s.x.toFixed(2)}`);
  }

  // 每个字段起点偏移上，两种归属画出来的光标位置 —— 必须是两个不同的 x。
  console.log(`\n=== 同一偏移上的"括号之前 / 括号之后" ===`);
  for (const [cp, hint] of [[5, -1], [5, 0], [6, -1], [6, 1], [7, -1], [7, 2]]) {
    const r = await conn.eval(`
      window.__heditor.setCaretAt(0, ${cp}, ${hint});
      const c = window.__heditor.caret;
      return JSON.stringify({ off: c.offset, f: window.__heditor.controls.caret,
                              x: c.x / ${g.zoom} });`);
    const v = JSON.parse(r);
    console.log(`   cp=${cp} 归属=${hint}  →  光标 ${v.off}（面板 ${v.f}）画在 x=${v.x.toFixed(2)}`);
  }

  // 横扫：光标画在哪，与点击点差多少。
  console.log(`\n=== 横扫 x∈[${g.lo.toFixed(1)}, ${g.hi.toFixed(1)}]（0.25pt 一步）===`);
  let prev = '';
  let worst = 0;
  let worstAt = 0;
  for (let x = g.lo - 8; x <= g.hi + 8; x += 0.25) {
    const h = JSON.parse(await conn.eval(
      `return JSON.stringify(window.__heditor.hit(${x.toFixed(3)}, ${g.y.toFixed(3)}));`));
    await clickPt(g, x, g.y);
    const s = JSON.parse(await conn.eval(`
      const c = window.__heditor.caret;
      return JSON.stringify({ off: c.offset, x: c.x / ${g.zoom},
                              f: window.__heditor.controls.caret });`));
    const d = Math.abs(s.x - x);
    if (d > worst) { worst = d; worstAt = x; }
    const key = `${h.offset}|${h.field}|${s.off}|${s.f}|${s.x.toFixed(1)}`;
    if (key !== prev) {
      console.log(`   x=${x.toFixed(2)}  命中=${h.offset}/${h.field}` +
                  `  光标=${s.off} 画在 x=${s.x.toFixed(2)}（差 ${d.toFixed(2)}）`);
      prev = key;
    }
  }
  console.log(`\n   最大偏差 ${worst.toFixed(2)}pt（在 x=${worstAt.toFixed(2)}）；` +
              `一个字宽约 14pt，偏差超过一格就是"光标没画在点的地方"`);

  // 单位标定 + "点开括号内侧"那一格到底落在哪：把探针那条算式原样算一遍。
  console.log(`\n=== variant 0：点 #1 开括号内侧（标定 caret.x 的单位）===`);
  await load(0);
  const g0 = await geo();
  const slots9 = JSON.parse(await conn.eval(
    'return JSON.stringify((window.__heditor.bracketSlots || []).filter(s => s.para === 0 && s.cp === 9));'));
  for (const s of slots9) {
    console.log(`   槽 cp=9 ${s.side === 1 ? '[' : ']'} 归属=${s.owner} x=${s.x.toFixed(2)}`);
  }
  const slotX = slots9.find((s) => s.side === 1).x;
  for (const hint of [-1, 0, 1, -2]) {
    const r = JSON.parse(await conn.eval(`
      window.__heditor.setCaretAt(0, 9, ${hint});
      const c = window.__heditor.caret;
      return JSON.stringify({ x: c.x, off: c.offset, f: window.__heditor.controls.caret });`));
    console.log(`   setCaretAt(9, ${hint}) → 光标 ${r.off}（面板 ${r.f}）caret.x=${r.x}` +
                `（除以 zoom ${g0.zoom} = ${(r.x / g0.zoom).toFixed(2)}）`);
  }
  const innerRaw = (await conn.eval(`
    window.__heditor.setCaretAt(0, 9, 1);
    return window.__heditor.caret.x;`));
  for (const [tag, clickX] of [
    ['按探针原式（未除 zoom）', (slotX + innerRaw) * 0.5 + 0.3],
    ['除以 zoom 之后', (slotX + innerRaw / g0.zoom) * 0.5 + 0.3],
  ]) {
    const h = JSON.parse(await conn.eval(
      `return JSON.stringify(window.__heditor.hit(${clickX.toFixed(3)}, ${g0.y.toFixed(3)}));`));
    await clickPt(g0, clickX, g0.y);
    const s = JSON.parse(await conn.eval(`
      const c = window.__heditor.caret;
      return JSON.stringify({ off: c.offset, f: window.__heditor.controls.caret });`));
    console.log(`   ${tag}：点 x=${clickX.toFixed(2)} → 命中 ${h.offset}/${h.field}，` +
                `落点 ${s.off} 归属 ${s.f}`);
  }
  conn.ws.close();
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e && e.stack ? e.stack : e);
  process.exit(1);
});
