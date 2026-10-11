// 诊断续二：逐个偏移把光标画出来，看相邻偏移的光标在屏幕上是否真的分开。
//
// 用户报“反和复之间的括号那点点不进去、方向键也移不进去”，判据就是“两个相邻偏移
// 的光标像素位置是否不同”。截图按偏移裁剪放大，直接看光标落在哪一对方括号的内外。
import { appendFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { CDP, sleep } from './cdp.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
mkdirSync(join(ROOT, 'out'), { recursive: true });
const LOG = join(ROOT, 'out', 'diag_ctl3.txt');
writeFileSync(LOG, '');

/**
 * 同时写控制台与日志文件，避免验证脚本转发丢失输出。
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
  await sleep(300);

  const load = async (v) => {
    await conn.eval(`return window.__heditor.loadFormSample(${v});`);
    await conn.eval('return window.__heditor.stopBlink();');
  };

  /**
   * 按区域裁剪放大截图，便于看清括号与光标的相对位置。
   */
  const zoomShot = async (name, cx, cy, w, h) => {
    const shot = await conn.send('Page.captureScreenshot', {
      format: 'png',
      clip: { x: Math.round(cx), y: Math.round(cy), width: Math.round(w), height: Math.round(h),
              scale: 3 },
    });
    writeFileSync(join(ROOT, 'out', 'web', name), Buffer.from(shot.data, 'base64'));
  };

  const lineBox = async () => conn.eval(`
    const cv = window.__heditor.canvas, r = cv.getBoundingClientRect();
    const q = window.__heditor.controls.items[0].rects[0];
    return { cx: r.left + q.x * window.__heditor.zoomLevel, cy: r.top + (q.y - 4) * window.__heditor.zoomLevel,
             w: (q.w + 40) * window.__heditor.zoomLevel, h: (q.h + 8) * window.__heditor.zoomLevel,
             zoom: window.__heditor.zoomLevel, left: r.left, top: r.top };`);

  for (const variant of [3, 2]) {
    await load(variant);
    const lb = await lineBox();
    say(`variant ${variant}：逐偏移的光标位置（线性轴，单位帧磅）`);
    for (let off = 5; off <= 11; ++off) {
      const s = JSON.parse(await conn.eval(`
        window.__heditor.setCaret(0, ${off});
        const c = window.__heditor.caret, cc = window.__heditor.controls;
        return JSON.stringify({ off: c.offset, x: c.x, own: cc.caret, items: cc.count });`));
      const slots = JSON.parse(await conn.eval(
        'return JSON.stringify(window.__heditor.bracketSlots);'))
        .map((q) => `${q.cp}${q.side === 1 ? '[' : ']'}${q.x.toFixed(1)}`).join(' ');
      say(`  off=${off} 光标x=${s.x.toFixed(1)}pt 光标归属=${s.own}   槽 ${slots}`);
      await zoomShot(`diag3-nested-off${off}.png`, lb.cx - 20, lb.cy, lb.w, lb.h);
    }
  }

  // 三连相邻：删掉中间字段之后，光标与括号的关系。
  await load(2);
  const lb2 = await lineBox();
  await conn.eval('return window.__heditor.setCaretOnly(0, 9);');
  for (let i = 0; i < 2; ++i) await conn.eval(keyExpr('Backspace'));
  const st = JSON.parse(await conn.eval(`
    const c = window.__heditor.caret, cc = window.__heditor.controls;
    return JSON.stringify({ off: c.offset, x: c.x, own: cc.caret,
      items: cc.items.map(i => [i.start[1], i.end[1], i.depth, i.parent,
                               (i.rects || []).length]) });`));
  say(`\n三连：删掉中间字段（心悸）后 光标=${st.off} 渲染x=${st.x.toFixed(1)} 归属=${st.own}`);
  say(`  控件 ${JSON.stringify(st.items)}`);
  say(`  槽 ${JSON.parse(await conn.eval('return JSON.stringify(window.__heditor.bracketSlots);'))
        .map((q) => `${q.cp}${q.side === 1 ? '[' : ']'}${q.x.toFixed(1)}`).join(' ')}`);
  await zoomShot('diag3-adj-del-middle.png', lb2.cx - 20, lb2.cy, lb2.w, lb2.h);

  // 三连：把后两个字段都删空。
  await load(2);
  await conn.eval('return window.__heditor.setCaretOnly(0, 9);');
  for (let i = 0; i < 2; ++i) await conn.eval(keyExpr('Backspace'));
  await conn.eval('return window.__heditor.setCaretOnly(0, 9);');
  for (let i = 0; i < 2; ++i) await conn.eval(keyExpr('Backspace'));
  const st2 = JSON.parse(await conn.eval(`
    const c = window.__heditor.caret, cc = window.__heditor.controls;
    return JSON.stringify({ off: c.offset, x: c.x, own: cc.caret,
      items: cc.items.map(i => [i.start[1], i.end[1], i.depth, i.parent,
                               (i.rects || []).length]) });`));
  const doc = await conn.eval('return window.__heditor.allText;');
  say(`\n三连：后两个字段都删空后 文本=${JSON.stringify(doc)} 光标=${st2.off} 归属=${st2.own}`);
  say(`  控件 ${JSON.stringify(st2.items)}`);
  say(`  槽 ${JSON.parse(await conn.eval('return JSON.stringify(window.__heditor.bracketSlots);'))
        .map((q) => `${q.cp}${q.side === 1 ? '[' : ']'}${q.x.toFixed(1)}`).join(' ')}`);
  await conn.eval('return window.__heditor.setCaret(0, 7);');
  const st3 = JSON.parse(await conn.eval(`
    const c = window.__heditor.caret, cc = window.__heditor.controls;
    return JSON.stringify({ off: c.offset, x: c.x, own: cc.caret });`));
  say(`  把光标放到 7：渲染x=${st3.x.toFixed(1)} 归属=${st3.own}`);
  await zoomShot('diag3-adj-del-two.png', lb2.cx - 20, lb2.cy, lb2.w, lb2.h);

  // 三层嵌套：一次删掉「心悸」「三天」（区间 [7,11)），看区间树与括号发出顺序。
  await load(3);
  const lb3 = await lineBox();
  await conn.eval('return window.__heditor.setCaretOnly(0, 11);');
  for (let i = 0; i < 4; ++i) await conn.eval(keyExpr('Backspace'));
  const st4 = JSON.parse(await conn.eval(`
    const c = window.__heditor.caret, cc = window.__heditor.controls;
    return JSON.stringify({ off: c.offset, x: c.x, own: cc.caret, items: cc.items.map(i =>
      ({ r: [i.start[1], i.end[1]], d: i.depth, p: i.parent, rects: (i.rects || []).length })) });`));
  say(`\n三层嵌套：删掉「心悸三天」后 文本=${JSON.stringify(
    await conn.eval('return window.__heditor.allText;'))} 光标=${st4.off} 归属=${st4.own}`);
  say(`  控件 ${JSON.stringify(st4.items)}`);
  say(`  槽 ${JSON.parse(await conn.eval('return JSON.stringify(window.__heditor.bracketSlots);'))
        .map((q) => `${q.cp}${q.side === 1 ? '[' : ']'}${q.x.toFixed(1)}`).join(' ')}`);
  await zoomShot('diag3-nested-del-four.png', lb3.cx - 40, lb3.cy, lb3.w + 40, lb3.h);

  await conn.send('Browser.close').catch(() => {});
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e.message);
  console.error(e.stack || '(无栈)');
  process.exit(2);
});
