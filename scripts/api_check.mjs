// 公开面的静态闸：demo 只许用契约里有的成员，且不许碰内部件。
//
// 不需要浏览器、不需要构建产物 —— 与 idl_check.mjs 同一档：打开页面之前就红。
//
// 四条规则：
//   一、demo 里不许出现内部标识符（内部工厂、命令表、wasm 堆、胶水）。
//   二、editor.X 的 X 必须在 web/src/types.ts 的 Editor 接口里。
//   三、HEditor.X 的 X 必须是库真正导出的（只有 createEditor）。
//   四、demo 里不许自己接键盘/指针/合成事件 —— 输入层在库里。
//
// 用法：node scripts/api_check.mjs
import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const CONTRACT = 'web/src/types.ts';
const MEMBERS_LIST = 'web/src/public_members.gen.ts';
// 示例页要扫的文件：两个页面 + web/app 下的全部 TS（组件拆成多份之后，列名字一定会漏）。
const DEMO_FILES = ['web/index.html', 'web/multi.html', ...appFiles('web/app')];

/**
 * 一个目录下的全部 TS 文件（递归）。
 * @param dir 相对仓库根的目录
 * @returns 相对路径列表
 */
function appFiles(dir) {
  const out = [];
  for (const name of readdirSync(join(ROOT, dir))) {
    const rel = dir + '/' + name;
    if (statSync(join(ROOT, rel)).isDirectory()) out.push(...appFiles(rel));
    else if (rel.endsWith('.ts')) out.push(rel);
  }
  return out;
}

const problems = [];
const note = (where, msg) => problems.push(`${where}: ${msg}`);

function read(rel) {
  return readFileSync(join(ROOT, rel), 'utf8');
}

/ 去掉注释与字符串，避免在注释里讲"不要用 __heditor"反而被判红。 */
function code(text) {
  let out = '';
  let i = 0;
  const n = text.length;
  while (i < n) {
    const ch = text[i];
    if (ch === '<' && text.startsWith('<!--', i)) {
      const j = text.indexOf('-->', i + 4);
      i = j < 0 ? n : j + 3;
      continue;
    }
    if (ch === '/' && text[i + 1] === '/') {
      while (i < n && text[i] !== '\n') ++i;
      continue;
    }
    if (ch === '/' && text[i + 1] === '*') {
      const j = text.indexOf('*/', i + 2);
      i = j < 0 ? n : j + 2;
      continue;
    }
    if (ch === '"' || ch === "'" || ch === '`') {
      const q = ch;
      ++i;
      while (i < n) {
        if (text[i] === '\\') { i += 2; continue; }
        if (text[i] === q) { ++i; break; }
        ++i;
      }
      out += '""';
      continue;
    }
    out += ch;
    ++i;
  }
  return out;
}

function contractMembers() {
  const text = read(CONTRACT);
  const start = text.indexOf('export interface Editor {');
  const end = text.indexOf('\n}', start);
  const body = text.slice(start, end);
  const names = new Set();
  for (const line of body.split('\n')) {
    // 泛型方法写成 on<K extends ...>(，所以分隔符也要认 '<'
    const m = /^  (?:readonly )?([A-Za-z_][A-Za-z0-9_]*)[(:<]/.exec(line);
    if (m) names.add(m[1]);
  }
  return names;
}

function lineOf(text, index) {
  return text.slice(0, index).split('\n').length;
}

const members = contractMembers();

const FORBIDDEN = [
  '__heditor',
  '__api',
  'CreateHEditor',
  'HEAPU8',
  'HEAPU32',
  'importScripts',
  'mod.',
  '.engine(',
  'api.read',
  'api.write',
];
for (const rel of DEMO_FILES) {
  const text = read(rel);
  const bare = code(text);
  for (const bad of FORBIDDEN) {
    let at = bare.indexOf(bad);
    while (at >= 0) {
      note(rel, `第 ${lineOf(bare, at)} 行出现内部标识符 ${bad}`);
      at = bare.indexOf(bad, at + 1);
    }
  }
}

for (const rel of DEMO_FILES) {
  const bare = code(read(rel));
  for (const m of bare.matchAll(/\beditor\.([A-Za-z_][A-Za-z0-9_]*)/g)) {
    if (!members.has(m[1])) {
      note(rel, `第 ${lineOf(bare, m.index)} 行调了 editor.${m[1]} —— 契约里没有这个成员`);
    }
  }
}

const EXPORTED = new Set(['createEditor']);
for (const rel of DEMO_FILES) {
  const bare = code(read(rel));
  for (const m of bare.matchAll(/\bHEditor\.([A-Za-z_][A-Za-z0-9_]*)/g)) {
    if (!EXPORTED.has(m[1])) {
      note(rel, `第 ${lineOf(bare, m.index)} 行调了 HEditor.${m[1]} —— 库没有导出它`);
    }
  }
}

const EVENTS = ['keydown', 'keyup', 'keypress', 'pointerdown', 'pointermove', 'pointerup',
                'compositionstart', 'compositionupdate', 'compositionend', 'wheel'];
for (const rel of DEMO_FILES) {
  const bare = code(read(rel));
  for (const ev of EVENTS) {
    const rx = new RegExp(`addEventListener\\(\\s*['"]${ev}['"]`, 'g');
    for (const m of bare.matchAll(rx)) {
      note(rel, `第 ${lineOf(bare, m.index)} 行自己接了 ${ev} —— 输入层在库里`);
    }
  }
}

if (existsSync(join(ROOT, MEMBERS_LIST))) {
  const gen = read(MEMBERS_LIST);
  const listed = new Set([...gen.matchAll(/^  '([A-Za-z_][A-Za-z0-9_]*)',$/gm)].map((m) => m[1]));
  const want = new Set([...members].filter((n) => n !== 'diag'));
  for (const n of want) if (!listed.has(n)) note(MEMBERS_LIST, `少了 ${n}（契约里有）`);
  for (const n of listed) if (!want.has(n)) note(MEMBERS_LIST, `多了 ${n}（契约里没有）`);
} else {
  note(MEMBERS_LIST, '文件不存在 —— 先跑 python scripts/gen_public_members.py');
}

// 三、发布声明只暴露契约。
//   写给使用者的那份 .d.ts 由 tsc 生成，判据是"生成结果里只有入口与类型"——
//   实现类的成员一旦漏进来，TS 会把它们当成公开面（与 JS 侧 111 个成员同一种病）。
const DTS = 'web/dist/index.d.ts';
if (!existsSync(join(ROOT, DTS))) {
  note(DTS, '不存在 —— 先跑 npm run build:types');
} else {
  const dts = read(DTS);
  if (!/export \{ createEditor \}/.test(dts)) note(DTS, '入口不再是 createEditor');
  if (!/export type \* from '\.\/types'/.test(dts)) note(DTS, '不再重导出契约类型');
  // 声明文件里出现 class 说明有个实现被导出了（对象字面量实现不会产出 class）。
  if (/\bclass\b/.test(code(dts))) note(DTS, '声明里出现了 class —— 实现被导出了');
}

if (problems.length) {
  console.log(`公开面检查：${problems.length} 条问题`);
  for (const p of problems) console.log('  ' + p);
  process.exit(1);
}
console.log(`公开面检查通过：demo ${DEMO_FILES.length} 个文件 / 契约 ${members.size} 个成员 / `
            + `${readdirSync(join(ROOT, 'web/src')).length} 个源文件`);
