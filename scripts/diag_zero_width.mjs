// 诊断：三连字段删空之后打字，字进了哪个字段。
//
// 复现探针里那条失败：把 #2、#1 用真实退格删成零宽，此时光标应停在 #1 上，
// 敲一个字应当进 #1（期望 [[5,7],[7,8],[8,8]]）。实际进了 #0（[[5,8],[8,8],[8,8]]）。
//
// 每一步打印 (偏移, 侧别)、括号槽、控件区间、文本 —— 结论要落在数字上。
//
// 用法：
//   node scripts/diag_zero_width.mjs --port 9222 --url http://127.0.0.1:8781/web/index.html
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

  const load = async (v) => {
    await conn.eval(`return window.__heditor.loadFormSample(${v});`);
    await conn.eval('return window.__heditor.stopBlink();');
  };
  const st = async () => JSON.parse(await conn.eval(`
    const c = window.__heditor.caret;
    return JSON.stringify({ off: c.offset, side: c.side,
                            f: window.__heditor.controls.caret,
                            items: window.__heditor.controls.items.map(i => [i.start[1], i.end[1]]),
                            text: window.__heditor.text });`));
  const slots = async () => JSON.parse(await conn.eval(
    'return JSON.stringify((window.__heditor.bracketSlots || []).filter(s => s.para === 0));'));
  const fmtSlots = (ss) => ss.map((s) => `${s.cp}${s.side === 1 ? '[' : ']'}`).join(' ');
  const line = (tag, s) =>
    console.log(`  ${tag.padEnd(24)} 光标 ${s.off}/${s.side}  面板=${s.f}` +
                `  区间=${JSON.stringify(s.items)}`);

  console.log('=== 三连可编辑（variant 2）：#0[5,7) #1[7,9) #2[9,11) ===');
  await load(2);
  line('初始', await st());
  console.log(`  括号槽：${fmtSlots(await slots())}`);

  // 与探针同一条路径：每删一次都用"删除前的端点"重新定位光标
  for (const ci of [2, 1]) {
    for (let guard = 0; guard < 8; ++guard) {
      const cur = (await st()).items;
      if (cur[ci][0] === cur[ci][1]) break;
      await conn.eval(`return window.__heditor.setCaretAt(0, ${cur[ci][1]}, ${ci});`);
      line(`定位到 #${ci} 端点 ${cur[ci][1]}`, await st());
      await conn.eval(KEY('Backspace'));
      await sleep(30);
      line(`删 #${ci} 一下`, await st());
    }
  }
  const before = await st();
  console.log(`  括号槽：${fmtSlots(await slots())}`);
  console.log(`  文本：${before.text}`);
  console.log(`  面板字段=${before.f}`);

  // 与探针同一条路：整份重建一次控件（#2 标只读），光标应当被搬回 #1 上。
  await conn.eval(`
    const it2 = window.__heditor.controls.items;
    return window.__heditor.setFormControls(it2.map((x, i) => ({
      start: x.start, end: x.end, kind: 0, id: x.id, ro: i === 2 })));`);
  await sleep(30);
  const rebuilt = await st();
  line('重建控件之后', rebuilt);
  console.log(`  括号槽：${fmtSlots(await slots())}`);

  await conn.eval('return window.__heditor.type("痛");');
  await sleep(30);
  const after = await st();
  line('敲「痛」之后', after);
  console.log(`  括号槽：${fmtSlots(await slots())}`);
  console.log(`  文本：${after.text}`);
  console.log(`  期望：区间 [[5,7],[7,8],[8,8]]（字进 #1）`);

  conn.ws.close();
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e && e.stack ? e.stack : e);
  process.exit(1);
});
