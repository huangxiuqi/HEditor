// 保存 / 打开的验收探针。
//
// 判据的口径与 edit_test 的往返用例一致：存盘 → 改一笔 → 打开 → 正文与帧指纹回到存盘那一刻。
//   指纹含瞬态（光标、选区），所以两次取样前都把光标钉在同一个位置。
//
// 这一份跑在示例页上（构建产物或 Vite 开发服务器都行），两条传输各跑一遍。
//
// 用法：node scripts/saveload_probe.mjs --port 9222
//       node scripts/saveload_probe.mjs --transport worker
import { CDP, Checks } from './cdp.mjs';

const argv = process.argv.slice(2);
const opt = (n, d) => {
  const i = argv.indexOf('--' + n);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : d;
};
const PORT = Number(opt('port', '9222'));
const URL_BASE = opt('url', 'http://127.0.0.1:8781/web/dist-app/index.html');
// 传输可以从 --transport 给，也可以直接带在 --url 上（探针脚本只转发 --url）。
const u = new URL(URL_BASE);
const TRANSPORT = u.searchParams.get('transport') ?? opt('transport', 'inline');
u.searchParams.set('transport', TRANSPORT);
u.searchParams.set('debug', '1');
const URL_ = u.href;

const chk = new Checks('保存 / 打开');
const body = (b) => 'return (async () => {\n' + b + '\n})();';

const target = await CDP.findPageTarget(PORT);
const c = await CDP.connect(target.webSocketDebuggerUrl);
await c.send('Page.enable');
await c.send('Runtime.enable');
await c.disableHttpCache();
await c.send('Emulation.setDeviceMetricsOverride',
             { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });

console.log('→ 打开', URL_);
await c.send('Page.navigate', { url: URL_ });
await c.waitFor('return !!window.__demoReady', 'window.__demoReady', 60000);
const ev = (b) => c.eval(body(b));

// 取指纹之前先停表、把光标钉住：闪烁相位与光标位置都会进指纹。
const SNAP = `
  window.__demo.diag.stopBlink();
  await window.__demo.setCaret({ para: 0, offset: 0 });
  window.__demo.diag.present();
  await window.__demo.flush();
  return {
    digest: await window.__demo.diag.frameDigest(),
    text: window.__demo.text(),
    pages: window.__demo.geometry().pages,
  };`;

chk.section('[S1] 存出来的是一份文档');
const s1 = await ev(`
  const e = window.__demo, d = e.diag;
  await d.reset(6, 1.5);
  await e.type('保存');
  await e.type('测试');
  ${SNAP}`);
const saved = await ev(`
  const e = window.__demo;
  return await e.save();`);
chk.check('保存返回非空 JSON 对象',
          typeof saved === 'string' && saved.length > 100 && saved.trimStart().startsWith('{'),
          `${saved.length} 字节`);
chk.check('文字按 UTF-8 写进文件（不是码点数组）',
          saved.includes('"t":"') && !saved.includes('"t":['),
          saved.includes('"t":[') ? '仍是数组' : '字符串');
chk.check('字体表里有真名字（PostScript 名 + 文件名）',
          /"ps":"[^"]+"/.test(saved) && /"file":"[^"]+\.ttf"/.test(saved),
          (saved.match(/"fonts":\{[\s\S]{0,160}/) || ['没有 fonts'])[0].slice(0, 120));

chk.section('[S2] 存盘 → 改一笔 → 打开：正文与帧都回到存盘那一刻');
const mutated = await ev(`
  const e = window.__demo;
  await e.type('污染');
  ${SNAP}`);
chk.check('改过之后正文确实变了（前置条件，否则下面那条恒真）',
          mutated.text !== s1.text, `${s1.text.length} → ${mutated.text.length} 字`);
const reopened = await ev(`
  const e = window.__demo;
  await e.open(${JSON.stringify(saved)});
  ${SNAP}`);
chk.check('打开之后正文与存盘那一刻逐字相同', reopened.text === s1.text,
          `${reopened.text.length} / ${s1.text.length} 字`);
chk.check('打开之后帧指纹与存盘那一刻相同（含光标与选区）',
          reopened.digest === s1.digest, `${s1.digest} → ${reopened.digest}`);
chk.check('打开之后页数也回到存盘那一刻',
          reopened.pages === s1.pages, `${s1.pages} → ${reopened.pages}`);

chk.section('[S3] 打开之后编辑器仍然可用');
const after = await ev(`
  const e = window.__demo;
  const before = e.text().length;
  const ok = await e.type('继续');
  const typed = e.text().length;
  await e.undo();
  return { before, typed, ok, undone: e.text().length };`);
chk.check('打开之后还能打字', after.ok === true && after.typed === after.before + 2,
          `${after.before} → ${after.typed}`);
chk.check('打开之后的编辑可以撤销', after.undone === after.before,
          `${after.typed} → ${after.undone}`);

chk.section('[S4] 坏文件：拒绝并且不动当前文档');
const bad = await ev(`
  const e = window.__demo;
  const before = e.text();
  let threw = false;
  try { await e.open('{"v":1,"blocks":[{"nope":true}]}'); } catch (err) { threw = true; }
  return { threw, same: e.text() === before };`);
chk.check('坏 JSON 打开失败并抛错', bad.threw === true);
chk.check('坏文件没有污染当前文档（失败即不生效）', bad.same === true);

console.log('');
console.log(`共 ${chk.total} 项断言，失败 ${chk.failures} 项（${TRANSPORT}）`);
process.exit(chk.failures ? 1 : 0);
