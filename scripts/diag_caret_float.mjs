// 诊断：光标位置为什么会"飘" —— 复现两条现象。
//
// 一，三层嵌套样例把控件内文字全删空，光标定位到最后一枚右括号之前，输入一个字符：
//    字符与光标是不是都跑到了第一枚左括号之后。
// 二，三连可编辑样例把后两个控件删空，光标定位到第二个控件，回车：
//    光标是不是跑进了第三个控件。
//
// 每一步打印 (偏移, 侧别)、光标 x、控件区间、括号槽与文本 —— 结论要落在数字上。
//
// 用法：
//   node scripts/diag_caret_float.mjs --port 9222 --url http://127.0.0.1:8781/web/index.html
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
  // 换算因子读门面（密度：设备像素 / 磅）。#zoom 那一格是倍数，差一个 4 / 3。
  const zoom = async () => conn.eval('return window.__heditor.zoomLevel;');
  const state = async () => JSON.parse(await conn.eval(`
    const c = window.__heditor.caret;
    return JSON.stringify({ para: c.para, off: c.offset, side: c.side, x: c.x,
                            f: window.__heditor.controls.caret,
                            items: window.__heditor.controls.items.map(i => [i.start[1], i.end[1]]),
                            text: window.__heditor.text });`));
  const slots = async () => JSON.parse(await conn.eval(
    'return JSON.stringify((window.__heditor.bracketSlots || []).filter(s => s.para === 0));'));
  const fmtSlots = (ss) => ss.map((s) => `${s.cp}${s.side === 1 ? '[' : ']'}`).join(' ');

  const line = (tag, s, z) =>
    console.log(`  ${tag.padEnd(20)} 光标 ${s.para}:${s.off}/${s.side}` +
                `  x=${(s.x / z).toFixed(2)}  面板=${s.f}  区间=${JSON.stringify(s.items)}`);

  // 一：三层嵌套删空 → 定位到最后一枚右括号之前 → 输入字符
  {
    console.log('\n=== 一、三层嵌套（variant 3）：外[5,11) 中[6,10) 内[7,9) ===');
    await load(3);
    const z = await zoom();
    console.log(`  初始括号槽：${fmtSlots(await slots())}`);
    console.log(`  文本：${(await state()).text}`);
    // 把控件内文字全删掉：光标摆到外层末尾，连按退格
    await conn.eval('return window.__heditor.setCaret(0, 11);');
    line('起点（外层末尾）', await state(), z);
    for (let i = 1; i <= 6; ++i) { await conn.eval(KEY('Backspace')); await sleep(30); }
    line('删空后', await state(), z);
    console.log(`  括号槽：${fmtSlots(await slots())}`);
    console.log(`  文本：${(await state()).text}`);
    // 定位到最后一枚右括号之前（6 枚括号 → 侧别 5）
    await conn.eval('return window.__heditor.setCaretAtSide(0, 5, 5);');
    line('定位到 ]外 之前', await state(), z);
    await conn.eval('return window.__heditor.type("X");');
    await sleep(30);
    line('输入 X 后', await state(), z);
    console.log(`  括号槽：${fmtSlots(await slots())}`);
    console.log(`  文本：${(await state()).text}`);
    await conn.eval('return window.__heditor.type("Y");');
    await sleep(30);
    line('再输入 Y 后', await state(), z);
    console.log(`  括号槽：${fmtSlots(await slots())}`);
    console.log(`  文本：${(await state()).text}`);
  }

  // 二：三连可编辑，后两个删空 → 定位到第二个控件 → 回车
  {
    console.log('\n=== 二、三连可编辑（variant 2）：[5,7) [7,9) [9,11) ===');
    await load(2);
    const z = await zoom();
    console.log(`  初始括号槽：${fmtSlots(await slots())}`);
    console.log(`  文本：${(await state()).text}`);
    await conn.eval('return window.__heditor.setCaret(0, 11);');
    for (let i = 1; i <= 4; ++i) { await conn.eval(KEY('Backspace')); await sleep(30); }
    line('后两个删空后', await state(), z);
    console.log(`  括号槽：${fmtSlots(await slots())}`);
    console.log(`  文本：${(await state()).text}`);
    await conn.eval('return window.__heditor.setCaretAtSide(0, 7, 2);');
    line('定位到第二个控件', await state(), z);
    await conn.eval(KEY('Enter'));
    await sleep(30);
    line('回车后', await state(), z);
    console.log(`  括号槽：${fmtSlots(await slots())}`);
    console.log(`  文本：${(await state()).text}`);
    console.log(`  面板字段=${(await state()).f}`);
    console.log(`  控件明细=${await conn.eval(
      'return JSON.stringify(window.__heditor.controls.items);')}`);
  }

  conn.ws.close();
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e && e.stack ? e.stack : e);
  process.exit(1);
});
