// 诊断：在空字段自己的位置上回车（分段），那个字段还在不在。
//
// 复现的是"零宽字段在断点被丢"这条 —— 试迁移时它让判据红，而旧路径（setBlocks）是绿的，
// 因为旧路径有一笔手工账（adjustControlsOnSplit）把字段推到新段。
//
// 每一步打印 (偏移, 侧别)、括号槽、控件区间与 id、文本、段落 —— 结论要落在数字上。
//
// 用法：
//   node scripts/diag_split_empty_field.mjs --port 9222 --url http://127.0.0.1:8781/web/index.html
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
    const it = window.__heditor.controls.items;
    return JSON.stringify({ off: c.offset, side: c.side, para: c.para,
                            f: window.__heditor.controls.caret,
                            n: it.length,
                            ids: it.map(i => i.id),
                            items: it.map(i => [i.start[0], i.start[1], i.end[0], i.end[1]]),
                            text: window.__heditor.text });`));
  const slots = async () => JSON.parse(await conn.eval(
    'return JSON.stringify(window.__heditor.bracketSlots || []);'));
  const fmtSlots = (ss) => ss.map((s) => `${s.cp}${s.side === 1 ? '[' : ']'}`).join(' ');
  const zoom = async () => conn.eval('return Number(document.getElementById("zoom").value);');
  // 几何：光标 x 是帧像素，括号槽 x 是磅 —— 换算到同一坐标系再比，
  //   才能回答"光标画在哪一枚括号的哪一侧"。
  const geom = async () => {
    const z = await zoom();
    const cx = await conn.eval('return window.__heditor.caret.x;');
    const ss = await slots();
    return { caret_pt: cx / z, slots: ss.map((s) => ({ cp: s.cp, x: s.x, owner: s.owner, para: s.para })) };
  };
  const line = (tag, s) =>
    console.log(`  ${tag.padEnd(26)} 光标 ${s.para}:${s.off}/${s.side}  面板=${s.f}` +
                `  字段数=${s.n} ${JSON.stringify(s.ids)}`);
  const lineGeom = (tag, g) => {
    console.log(`  ${tag.padEnd(26)} 光标 x=${g.caret_pt.toFixed(2)}pt  ` +
                `槽：${g.slots.map((s) => `p${s.para}/${s.x.toFixed(2)}#${s.owner}`).join(' ')}`);
  };

  console.log('=== 三连可编辑（variant 2）：part1[5,7) part2[7,9) part3[9,11) ===');
  await load(2);
  line('初始', await st());
  console.log(`  括号槽：${fmtSlots(await slots())}`);

  // 把 #2、#1 删空（与探针同一条路：每删一次用"删除前的端点"重新定位）
  for (const ci of [2, 1]) {
    for (let guard = 0; guard < 8; ++guard) {
      const cur = (await st()).items;
      if (cur[ci][1] === cur[ci][3]) break;
      await conn.eval(`return window.__heditor.setCaretAt(0, ${cur[ci][3]}, ${ci});`);
      await conn.eval(KEY('Backspace'));
      await sleep(30);
    }
  }
  const emptied = await st();
  line('两个字段删空之后', emptied);
  console.log(`  括号槽：${fmtSlots(await slots())}`);
  console.log(`  区间：${JSON.stringify(emptied.items)}`);
  console.log(`  文本：${emptied.text}`);

  // cp 7 上压着 5 枚边界（part1 的 ]、part2 的 [ ]、part3 的 [ ]），所以：
  //   侧别 1 = part1 的 ] 之后 = part2 的 [ 之前 → 在 part2 【外面】
  //   侧别 2 = part2 的 [ 之后 = part2 的 ] 之前 → 在 part2 【里面】（空字段的内容位置）
  //   侧别 3 = part2 的 ] 之后 = part3 的 [ 之前 → 在 part3 【外面】（= 点 part2 右括号的右半边）
  for (const [side, tag] of [[1, 'part2 外面（侧别 1）'], [2, 'part2 里面（侧别 2）'],
                             [3, 'part2 之后（侧别 3）']]) {
    await load(2);
    for (const ci of [2, 1]) {
      for (let guard = 0; guard < 8; ++guard) {
        const cur = (await st()).items;
        if (cur[ci][1] === cur[ci][3]) break;
        await conn.eval(`return window.__heditor.setCaretAt(0, ${cur[ci][3]}, ${ci});`);
        await conn.eval(KEY('Backspace'));
        await sleep(30);
      }
    }
    await conn.eval(`return window.__heditor.setCaretAtSide(0, 7, ${side});`);
    await sleep(30);
    const before = await st();
    line(`摆到 ${tag}`, before);
    console.log(`  括号槽：${fmtSlots(await slots())}`);

    await conn.eval(KEY('Enter'));
    await sleep(50);
    const after = await st();
    line(`回车之后（${tag}）`, after);
    console.log(`  括号槽：${fmtSlots(await slots())}`);
    lineGeom('几何', await geom());
    console.log(`  区间：${JSON.stringify(after.items)}`);
    console.log(`  文本：${after.text}`);
    console.log('');
  }
  console.log(`  期望：part1 与 part2 都还在（part2 归新段，即 start[0] === 1）`);

  conn.ws.close();
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e && e.stack ? e.stack : e);
  process.exit(1);
});
