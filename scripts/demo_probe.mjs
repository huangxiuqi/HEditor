// demo 页的运行时闸：证明"公开面真的只有公开面"。
//
// 静态检查（api_check.mjs）看的是源码里出现了什么；这一份看的是运行期拿得到什么 ——
//   两者缺一不可：源码里没写 window.__heditor，不代表页面运行时没有那个全局。
//
// 六组断言：
//   一、内部全局都没了（__heditor / createEditor / CreateHEditor）
//   二、实例上只有契约成员，诊断成员抛
//   三、公开方法真的能用（setText / type / undo / save / configure）
//   四、change 事件真的发（且带惰性 json）
//   五、configure 的夹取与未知键
//   六、销毁之后所有调用都拒绝
//
// 用法：scripts\verify_web.ps1 -Probe scripts\demo_probe.mjs -Page demo.html
import { CDP, Checks, sleep } from './cdp.mjs';

// 顶层 await 抛出来的异常默认只在 stderr 留一行，而 verify_web.ps1 会把 stderr 包成
//   NativeCommandError、把消息吃掉 —— 那就只剩"某处失败"。这两行把它挪到 stdout，
//   日志里才看得到真正的原因。
const die = (e) => {
  console.log('PROBE FAILED: ' + ((e && (e.stack || e.message)) || String(e)));
  process.exit(1);
};
process.on('uncaughtException', die);
process.on('unhandledRejection', die);

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};
const PORT = Number(opt('port', '9222'));
const TRANSPORT = opt('transport', 'inline');
const URL_BASE = opt('url', 'http://127.0.0.1:8781/web/dist-app/index.html');
// 传输由查询参数选（demo.js 认 ?transport=worker）。带在最后，别覆盖页面自己的查询串。
const URL_ = URL_BASE + (URL_BASE.includes('?') ? '&' : '?') + 'transport=' + TRANSPORT;

const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
const page = list.find((t) => t.type === 'page');
if (!page) throw new Error('没有可用的页面 target');
const cdp = await CDP.connect(page.webSocketDebuggerUrl);
await cdp.send('Page.enable');
await cdp.send('Runtime.enable');
await cdp.send('Log.enable');
await cdp.disableHttpCache();
await cdp.send('Emulation.setDeviceMetricsOverride',
               { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });

const chk = new Checks('公开面');
console.log('→ 打开', URL_);
await cdp.send('Page.navigate', { url: URL_ });
const bootMs = await cdp.waitFor('return !!window.__demoReady', 'window.__demoReady', 40000);
console.log(`→ 页面就绪，耗时 ${(bootMs / 1000).toFixed(1)} s`);

const ev = (body) => cdp.eval('return (async () => {\n' + body + '\n})();');

chk.section('一、内部全局都没了');
{
  const r = await ev(`
    const out = {};
    out.heditor = typeof window.__heditor;
    out.createEditor = typeof window.createEditor;
    out.createWorkerEditor = typeof window.createWorkerEditor;
    out.wasmFactory = typeof window.CreateHEditor;
    // 注意：不能写 typeof window.createEditor —— typeof 也会求值，getter 会当场抛。
    // typeof 也会求值：这里能安全写，是因为内部工厂已经不在了（C 步撤掉了脚本注入）。
    out.readThrows = false;
    try { window.createEditor; } catch (e) { out.readThrows = e instanceof TypeError; }
    return out;`);
  chk.check('window.__heditor 不存在', r.heditor === 'undefined', r.heditor);
  chk.check('window.createWorkerEditor 不存在', r.createWorkerEditor === 'undefined', r.createWorkerEditor);
  chk.check('window.createEditor 不存在（内部工厂不再挂全局）',
            r.createEditor === 'undefined' || r.readThrows === true,
            r.readThrows ? '读它就抛（旧宿主的 getter）' : 'undefined');
}

chk.section('二、实例上只有契约成员');
{
  const r = await ev(`
    const e = window.__demo;
    const keys = Object.keys(e);
    let diagThrows = false;
    try { e.auditFrame; } catch (err) { diagThrows = err instanceof TypeError; }
    let unknownThrows = false;
    let msg = '';
    try { e.pleaseDoSomething; } catch (err) { unknownThrows = err instanceof TypeError; msg = err.message; }
    return { keys, diagThrows, unknownThrows, msg };`);
  chk.check('实例成员非空', r.keys.length > 0, r.keys.join(','));
  chk.check('没有 auditFrame / markCheck 这类诊断成员', !r.keys.includes('auditFrame'));
  chk.check('读 diag 抛 TypeError（debug 未开）', r.diagThrows === true);
  chk.check('读未知成员抛 TypeError', r.unknownThrows === true, r.msg);
}

chk.section('三、公开方法');
{
  const r = await ev(`
    const e = window.__demo;
    const before = e.paragraphs().length;
    await e.setCaret({ para: 0, offset: 0 });
    const typed = await e.type('AB');
    const text = e.text();
    const saved = await e.save();
    await e.undo();
    return { before, typed, hasAB: text.includes('AB'), savedLen: saved.length,
             afterUndo: e.text() };`);
  chk.check('存在多段', r.before > 0, String(r.before));
  chk.check('type 返回 true', r.typed === true);
  chk.check('文本里出现了刚敲的字', r.hasAB === true);
  chk.check('save 返回非空 JSON', r.savedLen > 100, `${r.savedLen} 字节`);
  chk.check('undo 生效（AB 被撤回）', !r.afterUndo.includes('AB'));
}

chk.section('四、change 事件');
{
  const r = await ev(`
    const e = window.__demo;
    const seen = [];
    const off = e.on('change', async (c) => seen.push({ source: c.source, version: c.version,
                                                        textLen: c.text.length,
                                                        json: (await c.json()).length }));
    await e.type('X');
    off();
    await e.type('Y');
    return { seen };`);
  chk.check('on(change) 收到了一次', r.seen.length === 1, JSON.stringify(r.seen));
  chk.check('载荷里带 json（调方法才序列化）', r.seen[0].json > 100, String(r.seen[0].json));
  chk.check('version 是递增的数', Number.isFinite(r.seen[0].version), String(r.seen[0].version));
}

chk.section('五、configure');
{
  const r = await ev(`
    const e = window.__demo;
    const got = await e.configure({ zoom: 1.25 });
    const clamped = await e.configure({ zoom: 99 });
    let badKey = '';
    try { await e.configure({ zom: 1 }); } catch (err) { badKey = err.message; }
    let readOnlyWrite = null;
    await e.configure({ mode: 'readOnly' });
    readOnlyWrite = await e.type('nope');
    const modeShown = e.config().mode;
    await e.configure({ mode: 'document' });
    return { zoom: got.zoom, clamped: clamped.zoom, readOnlyWrite, modeShown,
             cfgZoom: e.config().zoom, badKey };`);
  chk.check('configure 返回生效后的值', r.zoom === 1.25, String(r.zoom));
  chk.check('越界被夹取到 2.25', r.clamped === 2.25, String(r.clamped));
  chk.check('config() 与返回值一致', r.cfgZoom === 2.25, String(r.cfgZoom));
  chk.check('拼错的键同步抛', r.badKey.includes('未知配置项'), r.badKey);
  chk.check('只读时编辑返回 false', r.readOnlyWrite === false, String(r.readOnlyWrite));
  chk.check('mode 读回三档里的只读', r.modeShown === 'readOnly', String(r.modeShown));
}

chk.section('六、销毁');
{
  const r = await ev(`
    const e = window.__demo;
    await e.destroy();
    let threw = '';
    try { await e.type('z'); } catch (err) { threw = err.message; }
    return { threw, empty: document.getElementById('ed').children.length };`);
  chk.check('销毁后调用被拒', r.threw.includes('已销毁'), r.threw);
  chk.check('容器被清空', r.empty === 0, String(r.empty));
}

const fails = chk.rows.filter((x) => !x.ok).length;
console.log(`\n共 ${chk.total} 项断言，失败 ${fails} 项`);
await sleep(50);
// 批量验证时由 verify_web.ps1 统一收浏览器；这里各自关一次的话，
//   一条命令跑多个探针时第二个就连不上 9222 了。
if (!process.env.HEDITOR_KEEP_BROWSER) await cdp.send('Browser.close').catch(() => {});
process.exit(fails ? 1 : 0);
