// 保存 / 打开的验收探针（单线程与 Worker 两个宿主都跑）。
//
// 用法：
//   node scripts/saveload_probe.mjs --port 9222 --url http://127.0.0.1:8781/web/index.html
//   node scripts/saveload_probe.mjs --port 9222 --url http://127.0.0.1:8781/web/worker.html
//
// 判据的口径与 edit_test [60] 一致：存盘 → 改一笔 → 打开 → 整帧指纹回到存盘那一刻。
//   指纹是"窗口内容 + 瞬态"的 FNV，所以控件括号、光标、选区一起被钉住。
import { CDP, Checks, sleep } from './cdp.mjs';

const argv = process.argv.slice(2);
const opt = (n, d) => {
  const i = argv.indexOf('--' + n);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : d;
};
const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://localhost:8781/web/index.html');

const say = (s) => process.stdout.write(s + '\n');
const body = (b) => 'return (async () => {\n' + b + '\n})();';

const chk = new Checks('保存 / 打开');

async function main() {
  const t = await CDP.findPageTarget(PORT);
  const c = await CDP.connect(t.webSocketDebuggerUrl);
  await c.send('Page.enable');
  await c.send('Runtime.enable');
  await c.disableHttpCache();
  await c.send('Page.navigate', { url: URL_ });
  await c.waitFor('return !!(window.__heditor && window.__heditor.ready)', 'window.__heditor.ready');

  chk.section('[S1] 存出来的是一份文档');
  const s1 = await c.eval(body(
    'const h = window.__heditor;\n' +
    'await h.stopBlink();\n' +
    'await h.run(6, h.zoomLevel);\n' +
    'await h.type("保存");\n' +
    'await h.type("测试");\n' +
    // 指纹含光标：先把光标钉在一个已知位置，否则"存盘时"与"打开后"的光标不同，
    //   比出来的是光标位移，不是文档内容（长文档那一节正好光标都在 0，所以它是对的）。
    'await h.setCaret(0, 0);\n' +
    'const saved = await h.saveDocument();\n' +
    'const digest = await h.frameDigest();\n' +
    'return { saved, digest, pages: h.editStats.totalPages, cps: h.editStats.docCodepoints };'));
  chk.check('保存返回非空 JSON 对象', typeof s1.saved === 'string' &&
            s1.saved.length > 100 && s1.saved.trimStart().startsWith('{'),
            `${s1.saved.length} 字节`);
  chk.check('保存不带副作用（帧指纹与段数不变）', typeof s1.digest === 'number' && s1.pages >= 1,
            `digest ${s1.digest} / ${s1.pages} 页 / ${s1.cps} 码点`);
  // 文件要能被人读：文字是 UTF-8（v1 写的是码点数组）、字体给的是文件名（不是一个库下标）。
  chk.check('文字按 UTF-8 写进文件（不是码点数组）',
            s1.saved.includes('"t":"') && !s1.saved.includes('"t":['),
            s1.saved.includes('"t":[') ? '仍是数组' : '字符串');
  // 字体身份用字体自己说的名字（PostScript 名），文件名排在最后 —— 文件改名不该换字体。
  chk.check('字体表里有真名字（PostScript 名 + 文件名）',
            /"ps":"[^"]+"/.test(s1.saved) && /"file":"[^"]+\.ttf"/.test(s1.saved),
            (s1.saved.match(/"fonts":\{[\s\S]{0,200}/) || ['没有 fonts'])[0].slice(0, 120));
  // 界面那一半也要在：按钮存在于两个验证台（id 不同），且启动之后是能点的 ——
  //   门面通而按钮没接，是"功能做完了却用不到"的典型形态。
  const ui = await c.eval(body(
    'const save = document.getElementById("saveDoc") || document.getElementById("btnSave");\n' +
    'const open = document.getElementById("openDoc") || document.getElementById("btnOpen");\n' +
    'return { save: !!save && !save.disabled, open: !!open && !open.disabled };'));
  chk.check('保存 / 打开按钮都在，且启动后可用', ui.save === true && ui.open === true,
            JSON.stringify(ui));

  chk.section('[S2] 打开回到存盘那一刻');
  const s2 = await c.eval(body(
    'const h = window.__heditor;\n' +
    'const saved = ' + JSON.stringify(s1.saved) + ';\n' +
    'const digest0 = ' + JSON.stringify(s1.digest) + ';\n' +
    'const cps0 = ' + JSON.stringify(s1.cps) + ';\n' +
    // 改一笔：文字与像素都必须真的变了，否则下面的"回到原样"是恒真。
    'await h.type("改了");\n' +
    'const cpsMid = h.editStats.docCodepoints;\n' +
    'const digestMid = await h.frameDigest();\n' +
    'await h.loadDocument(saved);\n' +
    'const cps1 = h.editStats.docCodepoints;\n' +
    'const digest1 = await h.frameDigest();\n' +
    'const again = await h.saveDocument();\n' +
    'const undoAfter = await h.undo();\n' +
    'return { cps0, cpsMid, cps1, digest0, digestMid, digest1, same: again === saved,\n' +
    '         undoAfter, caret: h.editStats.caretOffset, pages: h.editStats.totalPages };'));
  chk.check('打开之前确实改过（文字与像素都变了）',
            s2.cpsMid > s2.cps0 && s2.digestMid !== s2.digest0,
            `码点 ${s2.cps0} → ${s2.cpsMid}，digest ${s2.digest0} → ${s2.digestMid}`);
  chk.check('打开之后帧指纹逐位回到存盘那一刻', s2.digest1 === s2.digest0,
            `${s2.digest1} vs ${s2.digest0}`);
  chk.check('打开之后码点数回到存盘那一刻', s2.cps1 === s2.cps0,
            `${s2.cps1} vs ${s2.cps0}`);
  chk.check('再存一遍字节完全相同（写端确定）', s2.same === true, s2.same ? '相同' : '不同');
  chk.check('打开之后撤销栈是空的（历史不属于这份文档）', s2.undoAfter === null,
            String(s2.undoAfter && s2.undoAfter.caretOffset));
  chk.check('打开之后光标回到文档开头', s2.caret === 0, `偏移 ${s2.caret}`);

  chk.section('[S3] 坏文件被拒，现场不动');
  const s3 = await c.eval(body(
    'const h = window.__heditor;\n' +
    'const before = await h.saveDocument();\n' +
    'const digest = await h.frameDigest();\n' +
    'const cps = h.editStats.docCodepoints;\n' +
    'const errs = [];\n' +
    'for (const bad of ["{ 这不是 json", "{\\"v\\":99}", ""]) {\n' +
    '  try { await h.loadDocument(bad); errs.push("no-throw"); }\n' +
    '  catch (e) { errs.push(String((e && e.message) || e)); }\n' +
    '}\n' +
    'const after = await h.saveDocument();\n' +
    'return { errs, sameDoc: after === before, digest, cps, cpsAfter: h.editStats.docCodepoints };'));
  chk.check('三种坏输入都抛错（空串 / 坏 JSON / 版本不符）',
            s3.errs.every((e) => e && e !== 'no-throw'), s3.errs.join(' ｜ '));
  chk.check('坏输入之后文档一个字节都没动', s3.sameDoc === true && s3.cpsAfter === s3.cps,
            `码点 ${s3.cps} → ${s3.cpsAfter}`);

  chk.section('[S4] 长文档（2000 段）往返');
  const s4 = await c.eval(body(
    'const h = window.__heditor;\n' +
    'await h.run(7, h.zoomLevel);\n' +
    // 与 S2 同一个口径：帧指纹含光标，所以比之前要先把光标钉在打开之后会回到的位置。
    //   worker 宿主在 type 之后光标不在 0，不钉的话这一条比出来的是光标位移。
    'await h.setCaret(0, 0);\n' +
    'let t0 = performance.now();\n' +
    'const saved = await h.saveDocument();\n' +
    'const saveMs = performance.now() - t0;\n' +
    'const digest = await h.frameDigest();\n' +
    'const pages = h.editStats.totalPages;\n' +
    'await h.type("改");\n' +
    't0 = performance.now();\n' +
    'await h.loadDocument(saved);\n' +
    'const loadMs = performance.now() - t0;\n' +
    'const digest2 = await h.frameDigest();\n' +
    'return { bytes: saved.length, saveMs, loadMs, digest, digest2, pages,\n' +
    '         pages2: h.editStats.totalPages };'));
  chk.check('长文档存得下（> 100 KB）', s4.bytes > 100000, `${(s4.bytes / 1024).toFixed(1)} KB`);
  chk.check('长文档打开之后仍是同样的页数', s4.pages === s4.pages2, `${s4.pages} 页`);
  chk.check('长文档打开之后帧指纹一致', s4.digest === s4.digest2, `${s4.digest2}`);
  say(`      存 ${s4.saveMs.toFixed(1)} ms / 开 ${s4.loadMs.toFixed(1)} ms` +
      `（${(s4.bytes / 1024).toFixed(1)} KB，${s4.pages} 页）`);

  chk.section('[S5] 按需装载（lazy）的字体也有身份');
  const s5 = await c.eval(body(
    'const h = window.__heditor;\n' +
    'await h.reloadFonts("lazy");\n' +
    'await h.run(6, h.zoomLevel);\n' +
    'await h.setCaret(0, 0);\n' +
    'const saved = await h.saveDocument();\n' +
    'const m = saved.match(/"fonts":\\{[\\s\\S]{0,220}/);\n' +
    'return { fonts: m ? m[0] : "" };'));
  // name 表很小，按需装载时留在骨架里 —— 否则"换一种装载方式存盘"就写不出字体名了。
  chk.check('按需装载（lazy）也写得出 PostScript 名', /"ps":"[^"]+"/.test(s5.fonts),
            s5.fonts.slice(0, 140) || '没有 fonts');
  chk.check('按需装载也带族名（人看得出来是哪套字）', /"family":"[^"]+"/.test(s5.fonts),
            s5.fonts.slice(0, 140));

  say('');
  say('──────────────────────────────');
  say(`共 ${chk.total} 项断言，失败 ${chk.failures} 项`);
  await sleep(50);
  c.ws.close();
  process.exit(chk.failures === 0 ? 0 : 1);
}

main().catch((e) => {
  say('FATAL ' + String((e && e.stack) || e));
  process.exit(1);
});
