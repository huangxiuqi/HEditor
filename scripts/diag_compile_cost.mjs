// 诊断：一次编辑的耗时到底花在哪一层（编译 / 流索引 / 排版 / 上屏）。
//
// 要回答的问题：编辑一个字，整树重编译与流索引重建各占多少 —— 也就是"增量编译"
// 值不值得做、该做在哪一层。判据是比值，不是绝对值：
//   auditCompile().curMs 是本轮实际编译耗时，fullMs 是刚才一次整树全量编译 —— 两者同源。
//
// 用法：
//   node scripts/diag_compile_cost.mjs --port 9222 --url http://127.0.0.1:8781/web/index.html
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

  const st = async () => JSON.parse(await conn.eval(`
    const ac = window.__heditor.auditCompile();
    const es = window.__heditor.editStats;
    return JSON.stringify({
      diff: ac ? ac.diff : -1,
      curMs: ac ? ac.curMs : -1,
      fullMs: ac ? ac.fullMs : -1,
      paras: es ? es.totalParagraphs : -1,
      compile: es ? es.rebuildCompileMs : -1,
      flow: es ? es.rebuildFlowMs : -1,
      flowpos: es ? es.rebuildFlowPosMs : -1,
      copy: es ? es.rebuildCopyMs : -1,
      controls: es ? es.rebuildControlsMs : -1,
      layoutMs: es ? es.layoutMs : -1,
      totalMs: es ? es.totalMs : -1,
      relaidLines: es ? es.relaidLines : -1,
      totalLines: es ? es.totalLines : -1,
    });`));

  const show = (tag, s) =>
    console.log(`  ${tag.padEnd(22)} 段=${String(s.paras).padStart(4)}` +
                `  编译 ${s.compile.toFixed(3)}  流索引 ${s.flow.toFixed(3)}` +
                `  段区间 ${s.flowpos.toFixed(3)}  块流副本 ${s.copy.toFixed(3)}` +
                `  控件 ${s.controls.toFixed(3)}  排版 ${s.layoutMs.toFixed(3)}` +
                `  总计 ${s.totalMs.toFixed(3)}  不符=${s.diff}`);

  for (const v of [0, 1]) {
    console.log(`=== 场景 ${v} ===`);
    await conn.eval(`return window.__heditor.loadFormSample(${v});`);
    await conn.eval('return window.__heditor.stopBlink();');
    await sleep(50);
    show('装载之后', await st());

    // 在段落中段打字（远离文档首尾，避免落在"末尾追加"这种特例上）
    await conn.eval('return window.__heditor.setCaret(0, 5);');
    await conn.eval('return window.__heditor.type("字");');
    await sleep(30);
    show('打一个字之后', await st());

    for (let i = 0; i < 5; ++i) {
      await conn.eval('return window.__heditor.type("字");');
    }
    await sleep(30);
    show('再打五个字之后', await st());
    console.log('');
  }

  // 规模梯度 + 多次采样：单次毫秒级数字的噪声比信号大（同一操作在 0.000 与 0.100 之间跳），
  //   所以取 5 次的最小值 —— 最小值才是"这一层真正要花的钱"，平均值里掺着调度抖动。
  for (const paras of [200, 1000, 3000]) {
    console.log(`=== 压力 ${paras} 段 × 20 字（编辑 5 次取最小）===`);
    await conn.eval(`return window.__heditor.loadStressDocument(${paras}, 20);`);
    await conn.eval('return window.__heditor.stopBlink();');
    await sleep(120);
    const load = await st();
    console.log(`  装载：编译 ${load.compile.toFixed(3)}  流索引 ${load.flow.toFixed(3)}` +
                `  段区间 ${load.flowpos.toFixed(3)}  块流副本 ${load.copy.toFixed(3)}` +
                `  控件 ${load.controls.toFixed(3)}  排版 ${load.layoutMs.toFixed(3)}` +
                `  总计 ${load.totalMs.toFixed(3)}`);

    await conn.eval(`return window.__heditor.setCaret(${Math.floor(paras / 2)}, 5);`);
    const samples = [];
    for (let i = 0; i < 5; ++i) {
      await conn.eval('return window.__heditor.type("字");');
      await sleep(20);
      samples.push(await st());
    }
    const mn = (k) => Math.min(...samples.map((s) => s[k]));
    const av = (k) => samples.reduce((a, s) => a + s[k], 0) / samples.length;
    console.log(`  编辑 min：编译 ${mn('compile').toFixed(3)}  流索引 ${mn('flow').toFixed(3)}` +
                `  段区间 ${mn('flowpos').toFixed(3)}  块流副本 ${mn('copy').toFixed(3)}` +
                `  控件 ${mn('controls').toFixed(3)}`);
    console.log(`        排版 ${mn('layoutMs').toFixed(3)}  总计 ${mn('totalMs').toFixed(3)}` +
                `（avg ${av('totalMs').toFixed(3)}）`);
    console.log(`  自审：diff=${samples[0].diff}  本轮编译 cur=${samples[0].curMs}` +
                `  全量编译 full=${samples[0].fullMs}`);
    console.log('');
  }

  conn.ws.close();
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e && e.stack ? e.stack : e);
  process.exit(1);
});
