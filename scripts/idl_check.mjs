// 协议核对：宿主与引擎之间的接口面，四个地方必须说的是同一件事。
//
// 要证伪的事：wasm 导出（C++）、命令表（engine/api.ts）、调用点（editor.ts / host/dom.ts）、
//   薄壳（transport/worker_main.ts）各写各的。"改了一边忘了另一边"既不会编译失败，也不会立刻报错：
//   它表现成探针里某条断言的数字变了、某个按钮按了没反应、或者一条永远跑不到的分支。
//
// 核对六条：
//   一、协议表自己成立：命令名唯一、类别与呈现归属合法、调用的导出真的在 wasm 侧注册了。
//   二、导出全覆盖：wasm 的每一个导出，要么被某条命令调用，要么登记在不经过命令表的清单里。
//      —— 这一条就是"新增了引擎能力却忘了给门面"的探测器。
//   三、实现表与协议表一致：WRITE / READ / SILENT 的键集合必须等于表里对应类别的名字。
//      （运行时 collect() 也拦一遍；这里是让它在打开浏览器之前就红。）
//   四、engine_api 里的 mod.X 都在登记范围之内（打错一个名字不会静默变成 undefined 调用）。
//   五、调用点不越界：editor.ts / host/dom.ts / input/input.ts 写的命令名必须在表里，
//      且读/写类别要对。
//   六、边界纪律：库里的宿主与传输层不许出现 mod. 字面量 —— 只有 engine/api.ts 能摸
//      引擎对象，其余一律走命令表。
//
// 用法（不需要 Chrome、不需要构建产物）：
//   node scripts/idl_check.mjs
// 退出码 0 = 全绿，1 = 有漂移。每一条问题都会打出文件名与行号。

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = (rel) => fs.readFileSync(path.join(ROOT, rel), 'utf8');


/**
 * 去掉注释，但保留换行（行号要对得上）。
 *
 * 为什么必须去：命令表里有一段说明写着 "mod.setCaret(...); mod.showCaret(true);"，
 *   不去注释就会把说明文字当成真的调用 —— 判据会对着一段注释报"越界"。
 * 字符串里的 // 与 /* 不误伤（按引号状态跳过）；正则字面量里的 // 认不出来，
 *   但那只可能漏判，不会误判。
 */
function stripComments(src) {
  const out = [];
  let i = 0;
  let quote = null;
  while (i < src.length) {
    const ch = src[i];
    if (quote) {
      out.push(ch);
      if (ch === '\\' && i + 1 < src.length) { out.push(src[i + 1]); i += 2; continue; }
      if (ch === quote) quote = null;
      i += 1;
      continue;
    }
    if (ch === '"' || ch === "'" || ch === '`') { quote = ch; out.push(ch); i += 1; continue; }
    if (ch === '/' && src[i + 1] === '/') {
      while (i < src.length && src[i] !== '\n') { out.push(' '); i += 1; }
      continue;
    }
    if (ch === '/' && src[i + 1] === '*') {
      const end = src.indexOf('*/', i + 2);
      const stop = end < 0 ? src.length : end + 2;
      for (; i < stop; i += 1) out.push(src[i] === '\n' ? '\n' : ' ');
      continue;
    }
    out.push(ch);
    i += 1;
  }
  return out.join('');
}

const lineOf = (text, index) => text.slice(0, index).split('\n').length;

/**
 * 抓某个名字在源码里的每次出现（去注释后），回 [{line, text}]。
 */
function occurrences(code, re) {
  const hits = [];
  let m;
  const rx = new RegExp(re.source, re.flags.includes('g') ? re.flags : re.flags + 'g');
  while ((m = rx.exec(code)) !== null) hits.push({ line: lineOf(code, m.index), text: m[0] });
  return hits;
}


// IDL 从真源直接跑出来：把 api.ts 转成 CJS 再执行一次。
//
// 为什么不读构建产物：那要多一条"先构建、再核对"的顺序约定，而约定会忘。目录里那条
//   mtime 检查本身就是个例子：它在 problems 声明之前引用它，一触发就抛 ReferenceError
//   （"产物过期"变成崩溃）。api.ts 零 import，转译之后直接可以执行 —— 核对的到底是
//   不是当前源码，不再靠约定。
const apiTs = read('web/src/engine/api.ts');
const apiCode = stripComments(apiTs);
const API_TS = 'web/src/engine/api.ts';

const tsUrl = new URL('file://' + path.join(ROOT, 'web/node_modules/typescript/lib/typescript.js')
  .replace(/\\/g, '/')).href;
const ts = await import(tsUrl).then((m) => m.default ?? m);
const compiled = ts.transpileModule(apiTs, {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  fileName: 'api.ts',
}).outputText;
const apiModule = { exports: {} };
new Function('module', 'exports', compiled)(apiModule, apiModule.exports);
const IDL = apiModule.exports.IDL;
if (!IDL) throw new Error('web/src/engine/api.ts 没有导出 IDL');

const problems = [];
const note = (where, msg) => problems.push({ where, msg });


const KIND = new Set(['w', 'r', 's']);
const RENDER = new Set(['self', 'host', 'none']);
const seen = new Map();
for (const [name, kind, calls, render] of IDL.commands) {
  if (typeof name !== 'string' || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) {
    note('协议表', `命令名不合法：${name}`);
    continue;
  }
  if (seen.has(name)) note('协议表', `命令重名：${name}`);
  seen.set(name, true);
  if (!KIND.has(kind)) note('协议表', `${name}：类别 ${kind} 不是 w/r/s`);
  if (!RENDER.has(render)) note('协议表', `${name}：呈现归属 ${render} 不是 self/host/none`);
  if (!Array.isArray(calls)) { note('协议表', `${name}：第三列必须是数组`); continue; }
  if (kind !== 'w' && render === 'host') note('协议表', `${name}：只有写命令才会要求宿主补呈现`);
}


const cpp = read('src/wasm/wasm_main.cpp');
const exports = new Set([...cpp.matchAll(/emscripten::function\("([A-Za-z0-9_]+)"/g)].map((m) => m[1]));
const outside = new Map(IDL.outside.map(([n, kind, why]) => [n, { kind, why }]));

const calledBy = new Map();
for (const [name, , calls] of IDL.commands) {
  for (const c of calls) {
    if (!exports.has(c)) note('协议表', `${name}：调的 ${c} 在 wasm 侧没有注册`);
    if (!calledBy.has(c)) calledBy.set(c, []);
    calledBy.get(c).push(name);
  }
}
for (const [n, info] of outside) {
  if (!exports.has(n)) note('协议表', `不经过命令表的清单里有 ${n}，但 wasm 侧没有这个导出`);
  // "同上" 是允许的写法（紧跟着上一条同样的理由），两个字符起。
  if (!info.why || info.why.length < 2) note('协议表', `${n}：要写清为什么不经过命令表`);
  if (calledBy.has(n)) note('协议表', `${n}：既在清单里又被 ${calledBy.get(n).join('/')} 调用，二选一`);
}
// wasm_main.cpp 里 "// …" 的注释提到导出名不算数，所以这里只认上面那份注册清单。
const orphans = [...exports].filter((e) => !calledBy.has(e) && !outside.has(e));
for (const e of orphans) note('src/wasm/wasm_main.cpp', `导出 ${e} 没有任何登记：加一条命令，或写进 OUTSIDE 并说明原因`);


for (const [table, kind] of [['WRITE', 'w'], ['READ', 'r'], ['SILENT', 's']]) {
  const m = apiCode.match(new RegExp('const ' + table + " = collect\\('" + kind + "', \\{([\\s\\S]*?)\\n\\}\\);"));
  if (!m) { note(API_TS, `${table} 不再走 collect('${kind}', …) 了 —— 对账那条线断了`); continue; }
  const keys = new Set([...m[1].matchAll(/^ {2}([A-Za-z_][A-Za-z0-9_]*): /gm)].map((x) => x[1]));
  const want = new Set(IDL.byKind(kind));
  for (const k of want) if (!keys.has(k)) note(API_TS, `${table} 缺 ${k}（协议表里有）`);
  for (const k of keys) if (!want.has(k)) note(API_TS, `${table} 多 ${k}（协议表里没有）`);
}

// 派生这件事本身也要钉住：手抄的名单一旦被重新塞回来，这条会红。
if (!/const PRESENT_ALWAYS = IDL\.presentAlways\(\);/.test(apiCode)) {
  note(API_TS, 'PRESENT_ALWAYS 不再是派生的了 —— 不要把手抄名单加回来');
}


const MODULE_PROPS = new Set(['FS', 'HEAPU8', 'HEAPF32', 'HEAP32']);  // 模块属性，不是导出
const known = new Set([...calledBy.keys(), ...outside.keys(), ...MODULE_PROPS]);
for (const hit of occurrences(apiCode, /\bmod\.([A-Za-z_][A-Za-z0-9_]*)/)) {
  const name = hit.text.slice(4);
  if (!exports.has(name) && !MODULE_PROPS.has(name)) {
    note(API_TS, `第 ${hit.line} 行：mod.${name} 不是 wasm 导出`);
  } else if (!known.has(name)) {
    note(API_TS, `第 ${hit.line} 行：mod.${name} 没在协议表里登记`);
  }
}


const kindOf = new Map(IDL.commands.map((c) => [c[0], c[1]]));
function checkCalls(file, re, wantKinds, label) {
  const code = stripComments(read(file));
  for (const hit of occurrences(code, re)) {
    const name = hit.text.match(/'([A-Za-z0-9_]+)'/)[1];
    if (!kindOf.has(name)) { note(file, `第 ${hit.line} 行：${label} ${name} 不在协议表里`); continue; }
    if (!wantKinds.includes(kindOf.get(name))) {
      note(file, `第 ${hit.line} 行：${label} ${name} 的类别是 ${kindOf.get(name)}`);
    }
  }
}
checkCalls('web/src/editor.ts', /link\.write\('([A-Za-z0-9_]+)'/, ['w', 's'], '写命令');
checkCalls('web/src/editor.ts', /link\.read\('([A-Za-z0-9_]+)'/, ['r'], '读命令');
checkCalls('web/src/host/dom.ts', /link\.read\('([A-Za-z0-9_]+)'/, ['r'], '读命令');
checkCalls('web/src/host/dom.ts', /link\.write\('([A-Za-z0-9_]+)'/, ['w', 's'], '写命令');
checkCalls('web/src/input/input.ts', /env\.write\('([A-Za-z0-9_]+)'/, ['w', 's'], '写命令');


for (const file of ['web/src/editor.ts', 'web/src/transport/worker_main.ts',
                    'web/src/host/dom.ts', 'web/src/input/input.ts']) {
  for (const hit of occurrences(stripComments(read(file)), /\bmod\./)) {
    note(file, `第 ${hit.line} 行：出现了 mod. —— 这一层只能走 link.read / link.write / api` +
               `.engine()`);
  }
}

// 薄壳可以伸手拿的那两条：连页的两个配置（页间隙 + 连页标志）改的是排版形状，
//   而且必须与"换页度量 + 重排 + 作废槽表"成套发生，所以留在 setViewMode 里。
//   别的一律走命令表 —— 这个白名单是"又多了一个绕过命令表的入口"的探测器。
// 薄壳伸手拿引擎只允许"贴图那一档"：读窗口/页尺寸、拿页像素、拿还欠着的脏矩形。
//   贴图必须在薄壳里做（它要 OffscreenCanvas 的上下文），但"画什么"仍然是引擎的结论 ——
//   于是这条边界只能是"允许拿哪几个导出"，不是"允许拿哪几条命令"。
//   识别办法：找出被赋成 api.engine() 的别名，再看别名上取的每个成员；成员必须是真导出
//   （这样 catch (e) 里的 e.stack 不会被误判），且必须在下面这份白名单里。
const SHELL_MAY_REACH = new Set([
  'windowFirstPage', 'windowPageCount',           // 这一窗是哪几页
  'pagePxWidth', 'pagePxHeight',                  // 页画布的背衬尺寸
  'pageRgba',                                     // 一页的导出像素（内容 + 瞬态）
  'pagesPendingBlitJson', 'pendingRectsJson',     // 欠着的脏页与脏矩形
]);
const SHELL_FILES = ['web/src/transport/worker_main.ts'];
const shellHits = new Map();
for (const file of SHELL_FILES) {
  const code = stripComments(read(file));
  // 两种拿法都要认：先别名（const e = api.engine()）再从别名上取，以及直接点着取。
  const aliases = new Set();
  for (const m of code.matchAll(/([A-Za-z_$][A-Za-z0-9_$]*)\s*=\s*api\??\.engine\(\)/g)) {
    aliases.add(m[1]);
  }
  const take = (idx, name) => {
    if (!exports.has(name)) return;               // e.stack / e.canvas 之类，不是导出
    shellHits.set(file + ':' + (lineOf(code, idx) + 1) + ':' + name, name);
  };
  for (const a of aliases) {
    const rx = new RegExp('(?<![A-Za-z0-9_$.])' + a.replace(/\$/g, '\\$') +
                          '\\.([A-Za-z_][A-Za-z0-9_]*)', 'g');
    for (const m of code.matchAll(rx)) take(m.index, m[1]);
  }
  for (const m of code.matchAll(/api\??\.engine\(\)\.([A-Za-z_][A-Za-z0-9_]*)/g)) {
    take(m.index, m[1]);
  }
}
for (const key of shellHits.keys()) {
  const [file, line, name] = key.split(':');
  if (!SHELL_MAY_REACH.has(name)) {
    note(file, `第 ${line} 行：薄壳拿了 ${name} —— 贴图之外不该碰引擎`);
  }
}


const byKindCount = (k) => IDL.byKind(k).length;
const present = IDL.presentAlways();
console.log(`协议表：${IDL.commands.length} 条（写 ${byKindCount('w')} / 读 ${byKindCount('r')} / ` +
            `静默 ${byKindCount('s')}）`);
console.log(`wasm 导出：${exports.size} 个 —— 命令调用 ${calledBy.size} 个，` +
            `不经过命令表 ${outside.size} 个`);
console.log(`要宿主补呈现：${present.size} 条（${[...present].join(' ')}）`);
for (const kind of ['glue', 'host', 'state', 'diag']) {
  const list = IDL.outside.filter((o) => o[1] === kind).map((o) => o[0]);
  if (list.length) console.log(`  不经过命令表 · ${kind}：${list.join(' ')}`);
}

if (problems.length) {
  console.log(`\n共 ${problems.length} 处不一致：`);
  for (const p of problems) console.log(`  [${p.where}] ${p.msg}`);
  process.exit(1);
}
console.log('\n四份文件说的是同一件事。');
