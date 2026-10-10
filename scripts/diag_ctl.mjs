// 诊断：控件边界的可达停位与点击归属，覆盖三连相邻（variant 2）与三层嵌套（variant 3）。
//
// 每个场景打三张表：真实鼠标横扫一行得到的 x 区间到命中与落点的对应、方向键逐格走出的
// 偏移序列、删空内容之后的区间树。判据是可达停位与控件端点一致，不一致的 x 区间就是
// 用户说的“点不进去”。
import { appendFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { CDP, sleep } from './cdp.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
mkdirSync(join(ROOT, 'out'), { recursive: true });
const LOG = join(ROOT, 'out', 'diag_ctl.txt');
writeFileSync(LOG, '');

/**
 * 同时写控制台与日志文件。包一层是因为验证脚本在原生进程写 stderr 时会中断管道，
 * 只靠它的转发会丢掉已经产生的诊断输出。
 */
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

/**
 * 读一条完整状态：光标偏移与渲染像素、归属字段、全部控件区间与树关系。
 */
const stateExpr = `
  const c = window.__ced.caret, cc = window.__ced.controls;
  return JSON.stringify({
    off: c.offset, cx: Math.round(c.x), vis: c.visible,
    own: cc.caret,
    items: cc.items.map(i => [i.start[1], i.end[1], i.depth, i.parent, i.ro ? 1 : 0,
                              (i.rects || []).length]),
  });`;

/**
 * 把一串采样压成“值相同就合并”的区间段，便于一眼看出死区。
 */
function ranges(samples) {
  const out = [];
  for (const [x, k] of samples) {
    const last = out[out.length - 1];
    if (last && last.k === k) last.b = x;
    else out.push({ k, a: x, b: x });
  }
  return out;
}

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

  const dump = async (tag) => {
    const s = await state();
    const items = s.items.map(([a, b, d, p, ro, nr], i) =>
      `#${i}[${a},${b})d${d}p${p}${ro ? 'R' : ''}:${nr}矩形`).join('  ');
    say(`  ${tag.padEnd(26)} 光标=${s.off} 归属=${s.own} 渲染x=${s.cx}` +
                `   控件 ${items}`);
    return s;
  };

  /**
   * 真实鼠标横扫：x 是帧磅坐标，与控件矩形同一把尺子。
   * 每次点击之间不重置，因为落点只取决于点击位置。
   */
  const sweep = async (variant) => {
    await conn.eval(`return window.__ced.loadFormSample(${variant});`);
    await conn.eval('return window.__ced.stopBlink();');
    const geo = await conn.eval(`
      const cv = window.__ced.canvas, r = cv.getBoundingClientRect();
      const it = window.__ced.controls.items;
      let lo = 1e9, hi = -1e9, y = 0;
      for (const q of it) for (const qq of (q.rects || [])) {
        lo = Math.min(lo, qq.x); hi = Math.max(hi, qq.x + qq.w); y = qq.y + qq.h / 2;
      }
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               zoom: window.__ced.zoomLevel, lo, hi, y };`);
    say(`\n--- variant ${variant}：x ∈ [${geo.lo.toFixed(1)}, ${geo.hi.toFixed(1)}] ` +
                `y=${geo.y.toFixed(1)} zoom=${geo.zoom}`);
    const samples = [];
    const step = 0.5;
    for (let x = geo.lo - 8; x <= geo.hi + 8; x += step) {
      const hit = JSON.parse(await conn.eval(
        `return JSON.stringify(window.__ced.hit(${x.toFixed(3)}, ${geo.y.toFixed(3)}));`));
      const p = { x: geo.left + x * geo.zoom * geo.sx, y: geo.top + geo.y * geo.zoom * geo.sy,
                  button: 'left', clickCount: 1, buttons: 1 };
      await conn.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...p });
      await conn.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...p });
      await sleep(4);
      const st = await state();
      samples.push([Number(x.toFixed(1)),
                    `${hit ? `${hit.offset}/${hit.field}` : 'null'}→${st.off}/${st.own}` +
                    `@${st.cx}`]);
    }
    for (const r of ranges(samples)) {
      say(`   x=[${r.a},${r.b}]  ${r.k}`);
    }
    return geo;
  };

  /** 方向键逐格走：记录偏移序列，看有没有跨过控件边界。 */
  const walk = async (variant, from, dir, n) => {
    await conn.eval(`return window.__ced.loadFormSample(${variant});`);
    await conn.eval('return window.__ced.stopBlink();');
    await conn.eval(`return window.__ced.setCaretOnly(0, ${from});`);
    const seq = [from];
    for (let i = 0; i < n; ++i) {
      await conn.eval(keyExpr(dir > 0 ? 'ArrowRight' : 'ArrowLeft'));
      seq.push((await state()).off);
    }
    say(`   方向键${dir > 0 ? '右' : '左'} ${n} 次（起点 ${from}）：${seq.join(' ')}`);
  };

  for (const variant of [2, 3]) {
    say(`\n================ variant ${variant} ================`);
    await conn.eval(`return window.__ced.loadFormSample(${variant});`);
    await conn.eval('return window.__ced.stopBlink();');
    await dump('载入');
    const slots = await conn.eval(`return JSON.stringify(window.__ced.bracketSlots);`);
    say('  括号占位：' + slots);
    await sweep(variant);
    await walk(variant, 4, +1, 10);
    await walk(variant, 12, -1, 10);
  }

  // 三层嵌套的删除链：把最内层「心悸」删掉，再看光标与区间。
  say('\n================ 复现：三层嵌套删掉「心悸」 ================');
  await conn.eval('return window.__ced.loadFormSample(3);');
  await conn.eval('return window.__ced.stopBlink();');
  await conn.eval('return window.__ced.setCaretOnly(0, 9);');
  await dump('光标置于 9（内层末尾）');
  await conn.eval(keyExpr('Backspace'));
  await dump('退格 1 次');
  await conn.eval(keyExpr('Backspace'));
  await dump('退格 2 次（内层删空）');
  await conn.eval(keyExpr('ArrowRight'));
  await dump('右方向键 1 次');
  await conn.eval(keyExpr('ArrowRight'));
  await dump('右方向键 2 次');
  await conn.eval(keyExpr('ArrowLeft'));
  await dump('左方向键 1 次');

  say('\n================ 复现：三层嵌套把三个字段都删空 ================');
  await conn.eval('return window.__ced.loadFormSample(3);');
  await conn.eval('return window.__ced.stopBlink();');
  for (let i = 2; i >= 0; --i) {
    for (let k = 0; k < 6; ++k) {
      const done = await conn.eval(`
        const it = window.__ced.controls.items[${i}];
        if (it.end[1] <= it.start[1]) return true;
        window.__ced.setCaretAt(0, it.end[1], ${i});
        window.__ced.setCaretOnly(0, it.end[1]);
        return false;`);
      if (done) break;
      await conn.eval(keyExpr('Backspace'));
    }
    await dump(`删空 #${i} 之后`);
  }
  await conn.eval(keyExpr('ArrowRight'));
  await dump('删空后右方向键');
  await conn.eval(`${keyExpr('ArrowLeft')}`);
  await dump('删空后左方向键');
  await conn.eval(keyExpr('痛'));
  await dump('删空后在空字段位置打字');

  await conn.screenshot(join(ROOT, 'out', 'web', 'diag-ctl-nested-final.png'));
  await conn.send('Browser.close').catch(() => {});
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e.message);
  console.error(e.stack || '(无栈)');
  process.exit(2);
});
