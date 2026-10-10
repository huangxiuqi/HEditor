// 极小的 Chrome DevTools Protocol 客户端（无第三方依赖，Node 22 自带 fetch/WebSocket）。
//
// 抽出来的理由：验证台现在有两个探针（web_probe.mjs 看渲染，edit_probe.mjs 看编辑），
// 都需要"连上调试端口 → 导航 → 在页面里取值 → 抓异常"。这段不该有两份。
// （web_probe.mjs 里还留着自己那份旧拷贝，功能相同；下次改它时一并迁过来。）
import { mkdirSync, writeFileSync } from 'node:fs';

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class CDP {
  constructor(ws) {
    this.ws = ws;
    this.seq = 0;
    this.pending = new Map();
    this.consoleErrors = [];
    ws.addEventListener('message', (ev) => this._onMessage(JSON.parse(ev.data)));
  }

  static async connect(wsUrl) {
    const ws = new WebSocket(wsUrl);
    await new Promise((res, rej) => {
      ws.addEventListener('open', res, { once: true });
      ws.addEventListener('error', () => rej(new Error('WebSocket 连接失败：' + wsUrl)),
                           { once: true });
    });
    return new CDP(ws);
  }

  _onMessage(msg) {
    if (msg.id && this.pending.has(msg.id)) {
      const { resolve, reject } = this.pending.get(msg.id);
      this.pending.delete(msg.id);
      if (msg.error) reject(new Error(msg.method + ' → ' + JSON.stringify(msg.error)));
      else resolve(msg.result);
      return;
    }
    if (msg.method === 'Runtime.consoleAPICalled' && msg.params.type === 'error') {
      this.consoleErrors.push(
        (msg.params.args || []).map((a) => a.value ?? a.description ?? '').join(' '));
    }
    if (msg.method === 'Runtime.exceptionThrown') {
      this.consoleErrors.push(
        'uncaught: ' + (msg.params.exceptionDetails?.exception?.description ||
                        msg.params.exceptionDetails?.text || ''));
    }
    if (msg.method === 'Log.entryAdded' && msg.params.entry.level === 'error') {
      // 把 url 带上。资源类错误（404 / MIME 不对）光有 text 是一句
      //   "Failed to load resource: the server responded with a status of 404"
      //   —— 完全看不出是哪个资源，等于没记。
      const e = msg.params.entry;
      const where = e.url ? ` → ${e.url}` : '';
      this.consoleErrors.push('log: ' + e.text + where);
    }
  }

  send(method, params = {}) {
    const id = ++this.seq;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ id, method, params }));
      setTimeout(() => {
        if (this.pending.delete(id)) reject(new Error('超时：' + method));
      }, 240000);
    });
  }

  // 在页面里求值。表达式体（不是函数体），awaitPromise 打开。
  // 关掉 HTTP 磁盘缓存。导航之前必须调。
  //
  // 为什么必须显式关，而不是"反正 headless 没缓存"：
  //   Chrome 的 profile 是跨次运行复用的（scripts/verify_web.ps1 用固定的
  //   out/chrome-profile，而且从不清它），本地静态服务又会发 Last-Modified ——
  //   于是改了前端脚本、探针却拿到上一次的缓存副本。
  //   后果：改了前端脚本后探针仍拿旧副本，而 wasm 因体积变化会被重新下载，
  //   页面于是处于"新引擎 + 旧脚本"的半新半旧状态 —— 比全旧更难看出问题。
  //   探针验证的是磁盘上这一份代码，不是上一次的。
  async disableHttpCache() {
    await this.send('Network.enable');
    await this.send('Network.setCacheDisabled', { cacheDisabled: true });
  }

  async eval(expr) {
    const r = await this.send('Runtime.evaluate', {
      expression: `(function(){ ${expr} })()`,
      awaitPromise: true,
      returnByValue: true,
    });
    if (r.exceptionDetails) {
      throw new Error('页面内异常：' + (r.exceptionDetails.exception?.description ||
                                       r.exceptionDetails.text));
    }
    return r.result.value;
  }

  // 找页面目标：只用 fetch，不需要先建立 WebSocket，所以做成静态的
  static async findPageTarget(port, tries = 60) {
    for (let i = 0; i < tries; i++) {
      try {
        const res = await fetch(`http://127.0.0.1:${port}/json/list`);
        const list = await res.json();
        const page = list.find((t) => t.type === 'page');
        if (page) return page;
      } catch { /* 守护还没起来 */ }
      await sleep(500);
    }
    throw new Error(`连不上 Chrome 调试端口 ${port}`);
  }

  async waitFor(expr, label, timeoutMs = 180000) {
    const t0 = Date.now();
    let last = null;
    while (Date.now() - t0 < timeoutMs) {
      try {
        last = await this.eval(expr);
        if (last) return Date.now() - t0;
      } catch {
        last = null;   // 导航中执行上下文会失效，重试即可
      }
      await sleep(400);
    }
    throw new Error(`等待超时：${label}（最后取值 ${JSON.stringify(last)}）`);
  }

  async screenshot(path) {
    const shot = await this.send('Page.captureScreenshot',
                                 { format: 'png', captureBeyondViewport: true });
    writeFileSync(path, Buffer.from(shot.data, 'base64'));
  }
}

export function ensureDir(p) { mkdirSync(p, { recursive: true }); }

// 断言收集器：把每条断言的结果同时打印到控制台并留档，最后统一判定退出码。
//
// 注意命名：当前小节存在 cur，不是 section。
//   写成 this.section = 标题 会把原型上的 section() 方法遮蔽掉 ——
//   于是 chk.section('...') 报 "not a function"。同名字段与方法不能并存。
export class Checks {
  constructor(section) {
    this.rows = [];
    this.cur = section || '';
  }
  note(text) { console.log('     ' + text); }
  section(title) {
    this.cur = title;
    console.log(`\n${title}`);
  }
  check(name, cond, detail) {
    const ok = !!cond;
    this.rows.push({ section: this.cur, name, ok, detail: detail ?? '' });
    console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}${detail ? '   ' + detail : ''}`);
    return ok;
  }
  near(name, actual, expected, tol, unit = '') {
    const d = Math.abs(actual - expected);
    return this.check(name, d <= tol, `${actual} ≈ ${expected}（偏差 ${d}${unit}）`);
  }
  get total() { return this.rows.length; }
  get failures() { return this.rows.filter((r) => !r.ok).length; }
}
