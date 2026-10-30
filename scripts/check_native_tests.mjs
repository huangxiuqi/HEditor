// native 判据的边界闸：只许算，不许看画、不许模拟交互、不许出图。
//
// 要证伪的事：native 判据说"通过"了，浏览器上一跑就不对。原因不是断言写错，是它验的
//   那一层根本不在用户走的那条路上 —— 从引擎的 Bitmap 到屏幕上那块画布还隔着 wasm 边界
//   拷贝、Worker 往返、putImageData 与视口换算、主画布合成四层；交互又远一层，native
//   直接收算好的磅坐标，绕过的正是最容易错的"clientX 到磅"那一步。
//
// 用法（不需要构建）：
//   node scripts/check_native_tests.mjs
// 退出码 0 = 没有新增越界，1 = 有。存量按节号豁免，清一节从 BASELINE 删一个号。

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const DIR = path.join(ROOT, 'tests');

// 禁止的三类调用。命中即是越界：它们的结论都跨不出 native。
//
// 只禁"读回"与"模拟"：present / repaint 这类驱动调用不算 —— 它们只是让引擎画一次，
//   不构成假证据。真正的问题在读回那一刻（像素断言）与假装用户那一刻（事件模拟）。
const BANNED = [
  { re: /\.frame\(\)|frameBytes\(\)|\brawFrame\b|\bauditFrame\b|\bframeDigest\b|\bpagePixels\b|\bframeDiffBetween\b/,
    why: '读帧像素' },
  { re: /\bpaintPage\b|\bpageBitmap\b|\brasterPage\b/, why: '光栅入口' },
  { re: /\bdispatchKey\b|\bKeyEvent\b|\bPointerEvent\b|\.mouse\b|\btextarea\b|\.heditor-/,
    why: '交互模拟' },
];

// 存量的豁免：按节号。现在清空了 —— 一条都不许再加。
//   加豁免要改这个常量，评审时看得见；这正是"存量只能减不能增"的机械保证。
const BASELINE = new Set([]);

const files = fs.readdirSync(DIR).filter((f) => f.endsWith('.cpp')).sort();
const problems = [];
const waived = new Map();

for (const name of files) {
  const text = fs.readFileSync(path.join(DIR, name), 'utf8');
  const lines = text.split('\n');
  // 节边界：if (runSection(N))。没有分节的 TU（doc/shaping/feature）整份算一节 0。
  const marks = [];
  lines.forEach((l, i) => {
    const m = l.match(/if \(runSection\((\d+)\)\)/);
    if (m) marks.push({ line: i, id: Number(m[1]) });
  });
  const sectionOf = (line) => {
    let id = 0;
    for (const mk of marks) {
      if (mk.line <= line) id = mk.id;
      else break;
    }
    return id;
  };

  lines.forEach((l, i) => {
    // 注释里提到这些名字是解释，不是调用。
    const code = l.replace(/\/\/.*$/, '').replace(/^\s*\*.*$/, '');
    for (const b of BANNED) {
      if (!b.re.test(code)) continue;
      const sec = sectionOf(i);
      const where = path.relative(ROOT, path.join(DIR, name)) + ':' + (i + 1);
      const msg = `${where} 第 ${sec} 节 ${b.why}`;
      const key = name + ':' + sec;
      if (BASELINE.has(key)) waived.set(key, (waived.get(key) || 0) + 1);
      else problems.push(msg);
      break;
    }
  });
}

// 已经清干净但仍挂在豁免表里：一并查出来，避免这份表只增不减。
const stale = [...BASELINE].filter((k) => !waived.has(k));
const total = [...waived.values()].reduce((a, b) => a + b, 0);

if (problems.length === 0) {
  const tail = total ? `；存量待清 ${total} 处 / ${waived.size} 节` : '';
  console.log(`native 判据边界：只算不呈现、不模拟交互${tail}`);
  if (stale.length) {
    console.error('这些豁免已经清干净了，从 BASELINE 删掉：' + stale.join('、'));
    process.exit(1);
  }
  process.exit(0);
}

console.error(`native 判据越界 ${problems.length} 处（读帧像素只能在 web 侧）：`);
for (const p of problems) console.error('  ' + p);
if (total) console.error(`另有存量豁免 ${total} 处 / ${waived.size} 节，见 BASELINE。`);
process.exit(1);
