// 多实例探针：同页两个实例（一个同线程、一个 Worker）必须互不干扰。
//
// 页面上跑的是 web/multi.html（Vite 示例的第二个入口）：两栏并排，各有自己的容器
//   与引擎。判据三层：
//   独立性：往 A 敲字不改 B 的文本与页画布。
//   隔离性：A 上的一次失败只进 A 的错误历史，B 的那一份仍然是空的。
//   可拆性：destroy(A) 之后 A 不再报告任何内容，而 B 照常能编辑。
//
// 用法：bash out/_run.sh scripts/multi_probe.mjs "$S/multi.html?debug=1" out/_multi.log
import { CDP, Checks } from './cdp.mjs';

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};

const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://127.0.0.1:8781/web/dist-app/multi.html?debug=1');

const chk = new Checks('多实例');
const body = (b) => 'return (async () => {\n' + b + '\n})();';

const target = await CDP.findPageTarget(PORT);
const c = await CDP.connect(target.webSocketDebuggerUrl);
await c.send('Page.enable');
await c.send('Runtime.enable');
await c.send('Log.enable');
await c.disableHttpCache();
await c.send('Emulation.setDeviceMetricsOverride',
             { width: 1500, height: 1000, deviceScaleFactor: 1, mobile: false });

console.log('→ 打开', URL_);
await c.send('Page.navigate', { url: URL_ });
await c.waitFor('return !!window.__demoReady', 'window.__demoReady', 90000);
const ev = (b) => c.eval(body(b));

chk.section('[M1] 两个实例各自起来了');
const boot = await ev(`
  const a = window.__demoA, b = window.__demoB;
  return {
    both: !!a && !!b,
    diff: a !== b,
    ta: a.text(), tb: b.text(),
    pa: a.geometry().pages, pb: b.geometry().pages,
    // 两栏各有自己的画布容器：同一页上必须存在两组 #pages。
    stages: document.querySelectorAll('#stage').length,
    canvases: document.querySelectorAll('#pages canvas').length,
  };`);
chk.check('两个实例都拿到了', boot.both === true && boot.diff === true);
chk.check('各自装着自己的文本', boot.ta.startsWith('实例 A') && boot.tb.startsWith('实例 B'),
          `${boot.ta.slice(0, 6)} / ${boot.tb.slice(0, 6)}`);
chk.check('两套骨架同时存在（两组 #stage、至少两张页画布）',
          boot.stages === 2 && boot.canvases >= 2, `${boot.stages} 组 / ${boot.canvases} 张`);

chk.section('[M2] 独立：往 A 敲字不动 B');
const indep = await ev(`
  const a = window.__demoA, b = window.__demoB;
  const before = { a: a.text(), b: b.text() };
  await a.setCaret({ para: 0, offset: 6 });
  await a.type('X');
  await a.flush();
  return { before, after: { a: a.text(), b: b.text() } };`);
chk.check('A 的文本变了', indep.after.a === indep.before.a.slice(0, 6) + 'X' + indep.before.a.slice(6),
          `${indep.before.a.length} → ${indep.after.a.length} 字`);
chk.check('B 的文本一个字都没动', indep.after.b === indep.before.b);

chk.section('[M3] 隔离：A 只读不影响 B');
const iso = await ev(`
  const a = window.__demoA, b = window.__demoB;
  const before = { a: a.text(), b: b.text() };
  await a.configure({ mode: 'readOnly' });
  const cfg = { a: a.config().mode, b: b.config().mode };
  const typedA = await a.type('Z');
  const typedB = await b.type('W');
  await b.flush();
  return { cfg, typedA, typedB,
           grew: { a: a.text().length - before.a.length, b: b.text().length - before.b.length } };`);
chk.check('A 切到只读，B 仍是文档模式',
          iso.cfg.a === 'readOnly' && iso.cfg.b === 'document',
          `${iso.cfg.a} / ${iso.cfg.b}`);
chk.check('只读那一栏不接受编辑，另一栏照常',
          iso.typedA === false && iso.typedB === true &&
            iso.grew.a === 0 && iso.grew.b === 1,
          `A 改动 ${iso.grew.a} 字 / B 改动 ${iso.grew.b} 字`);

chk.section('[M4] 可拆：destroy(A) 之后 B 照常');
const dtor = await ev(`
  const a = window.__demoA, b = window.__demoB;
  const bBefore = b.text();
  await a.destroy();
  let rejected = false;
  try { a.text(); } catch (e) { rejected = true; }
  // B 在 A 被拆掉之后仍然能编辑：多实例共享的只有字体字节与 wasm 内存那一层。
  await b.setCaret({ para: 0, offset: 6 });
  const ok = await b.type('Y');
  await b.flush();
  return { rejected, ok, grew: b.text().length === bBefore.length + 1,
           emptied: document.getElementById('ed-a').childElementCount === 0 };`);
chk.check('A 被拆之后调用一律抛', dtor.rejected === true);
chk.check('A 的容器被清空', dtor.emptied === true);
chk.check('B 仍然能编辑', dtor.ok === true && dtor.grew === true);

const consoleErrs = c.consoleErrors.filter((t) => !/favicon/i.test(t));
chk.check('没有 console 错误', consoleErrs.length === 0, consoleErrs.slice(0, 2).join(' | '));

console.log('');
console.log(`共 ${chk.total} 项断言，失败 ${chk.failures} 项`);
process.exit(chk.failures ? 1 : 0);
