// 分层闸：core 内部的 include 只能沿一条方向走。
//
// 要证伪的事：目录名看着是分层的，实际谁都能 include 谁。CMake 只有一个 heditor_core，
//   include 根是 src/，写成 core/xxx 之后任何层都看得见任何层 —— 层与层的关系全靠人记。
// 逐层拆分目标做不到这一条：链接期只兜得住符号，纯头文件依赖（类型、inline）照样编得过。
//
// 用法（不需要构建）：
//   node scripts/check_layers.mjs
// 退出码 0 = 全绿，1 = 有越层。每条都打文件、行号与那条边。

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SRC = path.join(ROOT, 'src');

// 允许的边：键是"从哪一层"，值是"可以 include 哪些层"。同层内部永远允许。
// 方向只有一条：越靠后的层知道的越多，前面的一律不认识后面的。
const ALLOWED = {
  utils: [],
  text: ['utils'],
  doc: ['utils'],
  layout: ['doc', 'text', 'utils'],
  compile: ['doc', 'layout', 'utils'],
  render: ['text', 'utils'],
  engine: ['compile', 'doc', 'layout', 'render', 'text', 'utils'],
};

// 不在 core 下的目录（宿主入口）：它们属于最外层，可以 include 任何一层。
const HOST_DIRS = new Set(['app', 'wasm']);

const INCLUDE_RE = /#include\s+"core\/([a-z_]+)\//g;

const problems = [];

/**
 * 列出一棵树下的所有 C/C++ 源文件。
 * @param {string} dir 起始目录
 * @returns {string[]} 绝对路径
 */
function sources(dir) {
  const out = [];
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (/\.(h|cpp)$/.test(e.name)) out.push(p);
    }
  };
  walk(dir);
  return out;
}

/**
 * 文件属于哪一层。
 *
 * src/core/xxx/... 返回 xxx；src/app、src/wasm 返回 null（宿主，不受层表约束）；
 * 其余（例如 src/core/*.h 这种直接躺在 core 下的）也返回 null —— 那一层没有名字，
 * 出现这种文件本身该被看见，所以按"不在表里"报出来。
 * @param {string} file 绝对路径
 * @returns {string|null} 层名
 */
function layerOf(file) {
  const rel = path.relative(SRC, file).split(path.sep);
  if (rel[0] !== 'core') return null;
  if (rel.length < 3) return '';
  return rel[1];
}

for (const file of sources(SRC)) {
  const from = layerOf(file);
  if (from === null) continue;
  if (!Object.prototype.hasOwnProperty.call(ALLOWED, from)) {
    problems.push({ where: path.relative(ROOT, file), msg: `层 ${from || '(core 根)'} 没在允许边表里` });
    continue;
  }
  const text = fs.readFileSync(file, 'utf8');
  const lines = text.split('\n');
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i];
    if (!line.includes('#include')) continue;
    for (const m of line.matchAll(INCLUDE_RE)) {
      const to = m[1];
      if (to === from) continue;
      if (ALLOWED[from].includes(to)) continue;
      problems.push({
        where: path.relative(ROOT, file) + ':' + (i + 1),
        msg: `${from} -> ${to}（${from} 只允许 ${ALLOWED[from].join(' / ') || '同层'}）`,
      });
    }
  }
}

if (problems.length === 0) {
  const layers = Object.keys(ALLOWED).join(' / ');
  console.log(`分层闸：${layers}；宿主目录 ${[...HOST_DIRS].join(' / ')} 不设限`);
  process.exit(0);
}
console.log(`分层闸：${problems.length} 处越层`);
for (const p of problems) console.log(`  [${p.where}] ${p.msg}`);
process.exit(1);
