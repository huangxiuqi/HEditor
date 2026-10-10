// 诊断续：控件的“边界多于位置”这件事具体怎么表现出来。
//
// 三张表：每个码点偏移对应的光标渲染位置（相邻偏移的位置重合就是“方向键移不进去”）、
// 每枚括号槽附近的真实点击落点、删空之后的可达性。另存若干截图供与用户截图对照。
import { appendFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { CDP, sleep } from './cdp.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
mkdirSync(join(ROOT, 'out'), { recursive: true });
const LOG = join(ROOT, 'out', 'diag_ctl2.txt');
writeFileSync(LOG, '');

/** 同时写控制台与日志文件，避免验证脚本转发丢失输出。 */
const say = (s = '') => {
  process.stdout.write(s + '\n');
  appendFileSync(LOG, s + '\n');
};

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};
const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://localhost:8781/web/index.html');

const keyExpr = (k) => `
  window.__ced.textarea.dispatchEvent(
    new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true, cancelable: true }));
  return true;`;

const stateExpr = `
  const c = window.__ced.caret, cc = window.__ced.controls;
  return JSON.stringify({
    off: c.offset, cx: Math.round(c.x), own: cc.caret,
    items: cc.items.map(i => [i.start[1], i.end[1], i.depth, i.parent, i.ro ? 1 : 0,
                              (i.rects || []).length]),
  });`;

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
  await sleep(300);

  const state = async () => JSON.parse(await conn.eval(stateExpr));
  const items = (s) => s.items.map(([a, b, d, p, ro, nr], i) =>
    `#${i}[${a},${b})d${d}p${p}${ro ? 'R' : ''}:${nr}矩形`).join('  ');
  const slotsOf = async () => (await conn.eval('return JSON.stringify(window.__ced.bracketSlots);'));

  const load = async (v) => {
    await conn.eval(`return window.__ced.loadFormSample(${v});`);
    await conn.eval('return window.__ced.stopBlink();');
  };
  const shot = (n) => conn.screenshot(join(ROOT, 'out', 'web', n));

  // 每个偏移的光标渲染位置：相邻偏移落在同一个 x 上，方向键就会“看起来没动”。
  const table = async (variant, maxOff) => {
    await load(variant);
    say(`variant ${variant} 逐偏移：`);
    for (let off = 0; off <= maxOff; ++off) {
      await conn.eval(`return window.__ced.setCaretOnly(0, ${off});`);
      const s = await state();
      const inner = await conn.eval(`
        const cc = window.__ced.controls, off = ${off};
        for (let i = cc.items.length - 1; i >= 0; --i) {
          const it = cc.items[i];
          if (off >= it.start[1] && off <= it.end[1]) return i;
        }
        return -1;`);
      say(`  off=${String(off).padStart(2)} 渲染x=${String(s.cx).padStart(3)} ` +
          `光标归属=${s.own} 最内层=${inner}`);
    }
  };

  await table(3, 17);
  await table(2, 17);

  // 真实点击每枚括号槽的中心与两侧各 2pt，看落点是否与命中一致。
  say('\n每枚括号槽附近的真实点击（variant 3）：');
  await load(3);
  const geo = await conn.eval(`
    const cv = window.__ced.canvas, r = cv.getBoundingClientRect();
    const q = window.__ced.controls.items[0].rects[0];
    return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
             zoom: window.__ced.zoomLevel, y: q.y + q.h / 2 };`);
  const slots = JSON.parse(await slotsOf());
  for (const sl of slots) {
    const out = [];
    for (const dx of [-3, -1.5, 0, 1.5, 3]) {
      const x = sl.x + dx;
      const hit = JSON.parse(await conn.eval(
        `return JSON.stringify(window.__ced.hit(${x.toFixed(2)}, ${geo.y.toFixed(2)}));`));
      const p = { x: geo.left + x * geo.zoom * geo.sx,
                  y: geo.top + geo.y * geo.zoom * geo.sy,
                  button: 'left', clickCount: 1, buttons: 1 };
      await conn.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...p });
      await conn.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...p });
      await sleep(8);
      const s = await state();
      out.push(`x=${x.toFixed(1)} 命中${hit ? `${hit.offset}/${hit.field}` : 'null'}` +
               `→${s.off}/${s.own}@${s.cx}`);
    }
    say(`  槽 cp=${sl.cp} ${sl.side === 1 ? '开' : '闭'} owner=${sl.owner} 底x=${sl.x.toFixed(2)}` +
        `  ${out.join('  ')}`);
  }

  // 用户现象 3：只删掉最内层的「心悸」，看光标停在哪儿、再按右键去哪儿。
  say('\n现象 3：删掉「心悸」之后的光标（variant 3）：');
  await load(3);
  await conn.eval('return window.__ced.setCaretOnly(0, 9);');
  for (let i = 0; i < 2; ++i) await conn.eval(keyExpr('Backspace'));
  let s = await state();
  say(`  删完后 光标=${s.off} 归属=${s.own} 渲染x=${s.cx}   ${items(s)}`);
  say(`  括号占位=${await slotsOf()}`);
  await shot('diag2-bug3-after-del.png');
  for (let i = 0; i < 3; ++i) {
    await conn.eval(keyExpr('ArrowRight'));
    s = await state();
    say(`  右方向键 ${i + 1} 次 → 光标=${s.off} 归属=${s.own} 渲染x=${s.cx}`);
  }

  // 用户现象 2：把最内层的「心悸」与中层的「三天」都删掉。
  say('\n现象 2：删掉「心悸」「三天」之后的区间树（variant 3）：');
  await load(3);
  await conn.eval('return window.__ced.setCaretOnly(0, 9);');
  for (let i = 0; i < 2; ++i) await conn.eval(keyExpr('Backspace'));
  s = await state();
  say(`  删「心悸」后 ${items(s)}`);
  for (let i = 0; i < 2; ++i) await conn.eval(keyExpr('Backspace'));
  s = await state();
  say(`  再删「三天」后 光标=${s.off} 归属=${s.own} 渲染x=${s.cx}   ${items(s)}`);
  say(`  括号占位=${await slotsOf()}`);
  await shot('diag2-bug2-after-del.png');

  // 用户现象 4：后两个控件删空后，键盘还能不能进到那个位置。
  say('\n现象 4：后两个控件删空后（variant 3，只剩最外层有内容）：');
  await conn.eval('return window.__ced.setCaretOnly(0, 5);');
  const seq = [5];
  for (let i = 0; i < 8; ++i) {
    await conn.eval(keyExpr('ArrowRight'));
    seq.push((await state()).off);
  }
  say(`  从 5 连按右键 8 次：${seq.join(' ')}`);
  await conn.eval('return window.__ced.setCaretOnly(0, 12);');
  const seq2 = [12];
  for (let i = 0; i < 8; ++i) {
    await conn.eval(keyExpr('ArrowLeft'));
    seq2.push((await state()).off);
  }
  say(`  从 12 连按左键 8 次：${seq2.join(' ')}`);
  // 空字段位置能否用鼠标点到：横扫那一列附近
  const y2 = geo.y;
  const probes = [];
  for (let x = 150; x <= 230; x += 1) {
    const p = { x: geo.left + x * geo.zoom * geo.sx, y: geo.top + y2 * geo.zoom * geo.sy,
                button: 'left', clickCount: 1, buttons: 1 };
    await conn.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...p });
    await conn.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...p });
    await sleep(6);
    const st = await state();
    probes.push(`${x}:${st.off}/${st.own}`);
  }
  say(`  横扫 x=150..230 的落点：${probes.join(' ')}`);

  await conn.send('Browser.close').catch(() => {});
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e.message);
  console.error(e.stack || '(无栈)');
  process.exit(2);
});
