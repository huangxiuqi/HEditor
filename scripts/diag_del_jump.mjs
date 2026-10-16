// 诊断：删除之后光标落在哪儿 —— 复现两条"跳一格"现象（嵌套删「天」、三连删「悸」）。
//
// 一，三层嵌套样例删掉"天"之后，光标是不是跑到了"三"后面（少走一格）；
// 二，三连可编辑字段先删"三天"再删"悸"，光标是不是跑进了第三个控件。
//
// 每一步都用真实键盘事件走，然后打印 (偏移, 侧别)、光标 x、字段区间与括号槽 ——
// 判据要落在数字上：一次删除只该让光标退一个字，不该跨过括号。
//
// 用法：
//   node scripts/diag_del_jump.mjs --port 9222 --url http://127.0.0.1:8781/web/index.html
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
    const st = window.__heditor.editStats;
    return JSON.stringify({ off: c.offset, side: c.side, x: c.x, f: window.__heditor.controls.caret,
                            items: window.__heditor.controls.items.map(i => [i.start[1], i.end[1]]),
                            text: window.__heditor.text });`));
  const slots = async () => JSON.parse(await conn.eval(
    'return JSON.stringify((window.__heditor.bracketSlots || []).filter(s => s.para === 0));'));
  const fmtSlots = (ss) => ss.map((s) => `${s.cp}${s.side === 1 ? '[' : ']'}`).join(' ');

  const line = (tag, s, z) =>
    console.log(`  ${tag.padEnd(16)} 光标 ${s.off}/${s.side}` +
                `  x=${(s.x / z).toFixed(2)}  面板=${s.f}  区间=${JSON.stringify(s.items)}`);

  // 一：三层嵌套删"天"
  {
    console.log('\n=== 一、三层嵌套（variant 3）：外层[5,11) 中层[6,10) 内层[7,9) ===');
    await load(3);
    const z = await zoom();
    const ss = await slots();
    console.log(`  括号槽：${fmtSlots(ss)}`);
    console.log(`  文本：${(await state()).text}`);
    // 光标停在"天"后面（＝外层字段末尾的正文里），按退格删掉"天"
    await conn.eval('return window.__heditor.setCaret(0, 11);');
    line('删之前', await state(), z);
    await conn.eval(KEY('Backspace'));
    await sleep(30);
    line('删「天」之后', await state(), z);
    const after = await slots();
    console.log(`  括号槽（删后）：${fmtSlots(after)}`);
    // 再退一步：应该是「三」，不该跨过括号
    await conn.eval(KEY('Backspace'));
    await sleep(30);
    line('再退一步', await state(), z);
    console.log(`  括号槽（再退）：${fmtSlots(await slots())}`);
  }

  // 二：三连可编辑字段，先删"三天"再删"悸"
  {
    console.log('\n=== 二、三连可编辑（variant 2）：[5,7) [7,9) [9,11) ===');
    await load(2);
    const z = await zoom();
    console.log(`  括号槽：${fmtSlots(await slots())}`);
    console.log(`  文本：${(await state()).text}`);
    await conn.eval('return window.__heditor.setCaret(0, 11);');
    line('删之前', await state(), z);
    for (let i = 1; i <= 2; ++i) {
      await conn.eval(KEY('Backspace'));
      await sleep(30);
      line(`删「三天」第${i}下`, await state(), z);
    }
    console.log(`  括号槽：${fmtSlots(await slots())}`);
    // 接着再退一步：该删掉「悸」（前一个字符），光标应停在「复」与「悸」之间
    await conn.eval(KEY('Backspace'));
    await sleep(30);
    line('再退一步（删悸）', await state(), z);
    console.log(`  括号槽：${fmtSlots(await slots())}`);
    // 另一种走法：把光标摆到「悸」后面那一侧再退格
    await load(2);
    await conn.eval('return window.__heditor.setCaret(0, 11);');
    for (let i = 1; i <= 2; ++i) { await conn.eval(KEY('Backspace')); await sleep(30); }
    await conn.eval('return window.__heditor.setCaretAtSide(0, 9, 1);');
    line('摆到悸后面', await state(), z);
    await conn.eval(KEY('Backspace'));
    await sleep(30);
    line('删「悸」之后', await state(), z);
    console.log(`  括号槽：${fmtSlots(await slots())}`);
  }

  conn.ws.close();
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e && e.stack ? e.stack : e);
  process.exit(1);
});
