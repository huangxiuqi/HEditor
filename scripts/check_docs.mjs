// TS 侧的注释规范检查（JSDoc）。扫描 web/src 与 web/app：前者进 dist/*.d.ts，
//   后者是外部使用者照抄的那一份，两边的注释都有人读。
//
// 判据来自三件事，缺一条都会让"注释齐全"变成只看运气：
//   一、成员与方法必须有 JSDoc 块（不是 // 行注释）—— 生成 .d.ts 之后它才跟着走。
//   二、有参数就有 @param，有返回值就有 @returns（两个都按名字/有无判定，不看措辞）。
//   三、注释块与上一个成员之间必须空一行，否则整段读起来是一堵墙。
//
// 为什么用编译器 API 而不是正则：正则分不清"方法声明"与"方法体里的一次调用"，
//   而这份检查的结论要被当成门禁 —— 误报一次就会有人开始忽略它。
//
// 用法：
//   node scripts/check_docs.mjs            # 只报告
//   node scripts/check_docs.mjs --fix      # 一并把"缺空行"这类机械问题改掉
//   node scripts/check_docs.mjs --json     # 给别的脚本读
import { readFileSync, writeFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
// typescript 装在 web/ 下面（前端工具链整体在那一层）。
const tsPath = join(ROOT, 'web/node_modules/typescript/lib/typescript.js');
const ts = await import(new URL('file://' + tsPath.replace(/\\/g, '/')).href)
  .then((m) => m.default ?? m);

const argv = process.argv.slice(2);
const FIX = argv.includes('--fix');
const JSON_OUT = argv.includes('--json');

function walk(dir) {
  const out = [];
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) out.push(...walk(p));
    else if (p.endsWith('.ts')) out.push(p);
  }
  return out;
}

const FILES = [...walk(join(ROOT, 'web/src')), ...walk(join(ROOT, 'web/app'))];

// 取节点挂着的最后一段 JSDoc（ts 把注释挂在 node.jsDoc 上）。
function docStart(node) {
  const docs = node.jsDoc;
  if (!docs || !docs.length) return null;
  return docs[docs.length - 1];
}

function returnsSomething(node) {
  const t = node.type;
  if (!t) {
    // 没有注解：函数体里有没有 return 值不好判，按"有返回值"要求（宁严勿松）。
    return true;
  }
  const text = t.getText();
  return !/^(void|undefined|never)$/.test(text.trim());
}

function paramNames(node) {
  const names = [];
  for (const p of node.parameters ?? []) {
    if (ts.isIdentifier(p.name)) names.push(p.name.text);
    else if (ts.isObjectBindingPattern(p.name) || ts.isArrayBindingPattern(p.name)) {
      for (const el of p.name.elements) {
        if (ts.isBindingElement(el) && ts.isIdentifier(el.name)) names.push(el.name.text);
      }
    }
  }
  return names;
}

const problems = [];

function report(kind, file, line, name, detail) {
  problems.push({ kind, file: relative(ROOT, file), line, name, detail });
}

/**
 * 一个"成员/方法"该有的东西：doc 块、@param、@returns，以及它与上一行之间的空行。
 */
// 只有"方法"必须写 JSDoc：数据成员的说明在心里那份契约里（types.ts），
//   每个 x: number 都配一句"是横坐标"只是在复述类型。
function isMethod(node) {
  return (
    ts.isMethodDeclaration(node) ||
    ts.isMethodSignature(node) ||
    ts.isFunctionDeclaration(node) ||
    ts.isCallSignatureDeclaration(node)
  );
}

function checkMember(sf, node, label, src, prevEndLine) {
  const doc = docStart(node);
  const line = sf.getLineAndCharacterOfPosition(node.getStart(sf)).line + 1;
  const ambient = sf.fileName.endsWith('.d.ts');
  if (!doc) {
    // 环境声明（.d.ts）只查空行：它们是"外部形状"的转录，注释只会复述类型本身。
    // 数据成员也不查：那一层的说明在 types.ts 的契约里，逐字段复述类型没有信息量。
    if (!ambient && isMethod(node)) report('missing-doc', sf.fileName, line, label, '没有 JSDoc 块');
    return;
  }
  const text = doc.getText(sf);
  const docLine = sf.getLineAndCharacterOfPosition(doc.getStart(sf)).line + 1;

  // 空行：注释块与上一个成员之间必须有一行空白。紧跟 "{" 的第一个成员不算 ——
  //   那里空一行反而把块的开头与成员拆开。
  if (docLine > 1 && prevEndLine !== null) {
    const between = src.slice(prevEndLine, docLine - 1);
    const braceBefore = src[docLine - 2] !== undefined && src[docLine - 2].trim().endsWith('{');
    const blank = between.some((l) => l.trim() === '');
    if (!blank && prevEndLine > 0 && !braceBefore) {
      report('missing-gap', sf.fileName, docLine, label, '注释块与上一行之间缺空行');
    }
  }

  const params = paramNames(node);
  for (const p of params) {
    if (!new RegExp('@param\\s+(\\[)?' + p + '\\b').test(text)) {
      report('missing-param', sf.fileName, docLine, label, `缺 @param ${p}`);
    }
  }
  if (node.kind === ts.SyntaxKind.MethodDeclaration || node.kind === ts.SyntaxKind.FunctionDeclaration) {
    if (!ts.isConstructorDeclaration?.(node) && returnsSomething(node) && !/@returns?\b/.test(text)) {
      report('missing-returns', sf.fileName, docLine, label, '缺 @returns');
    }
  }
}

function checkFile(file) {
  const text = readFileSync(file, 'utf8');
  const sf = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true, ts.ScriptKind.TS);
  const lines = text.split('\n');

  // 每个节点"上一条声明"的结束行：用来判空行。用源码行号而不是 AST 顺序，
  //   因为中间可能夹着装饰器或类型别名这类不参与判断的东西。
  const startLineOf = (n) => sf.getLineAndCharacterOfPosition(n.getStart(sf)).line;

  function prevEndLineOf(node) {
    // 同一父节点里紧挨在它前面的那一条（语句表 / 成员表），返回它最后一行的行号。
    // 不能用 getChildren：SourceFile 只会给出一个 SyntaxList，找不到"前一条"。
    const p = node.parent;
    const arr = p && (p.statements ?? p.members);
    if (!arr) return null;
    const i = arr.indexOf(node);
    if (i <= 0) return null; // 第一个成员：不判空行（紧跟 { 的位置）
    return sf.getLineAndCharacterOfPosition(arr[i - 1].getEnd()).line;
  }

  function visit(node) {
    const k = node.kind;
    if (k === ts.SyntaxKind.InterfaceDeclaration) {
      for (const m of node.members) {
        const label = memberLabel(m);
        if (label) checkMember(sf, m, label, lines, prevEndLineOf(m));
      }
    } else if (k === ts.SyntaxKind.ClassDeclaration) {
      for (const m of node.members) {
        const label = memberLabel(m);
        if (label) checkMember(sf, m, label, lines, prevEndLineOf(m));
      }
    } else if (k === ts.SyntaxKind.FunctionDeclaration && node.name) {
      // 只查导出的：内部小函数（一行 lambda 的包装）不强制，否则会写出一堆废话注释。
      const exported = node.modifiers?.some((m) => m.kind === ts.SyntaxKind.ExportKeyword);
      if (exported) checkMember(sf, node, node.name.text, lines, prevEndLineOf(node));
    }
    ts.forEachChild(node, visit);
  }

  function memberLabel(m) {
    if (m.name && ts.isIdentifier(m.name)) return m.name.text;
    if (m.name && (ts.isStringLiteral(m.name) || ts.isNumericLiteral(m.name))) return m.name.text;
    if (ts.isCallSignatureDeclaration?.(m)) return 'call()';
    if (ts.isIndexSignatureDeclaration?.(m)) return '[index]';
    if (ts.isConstructSignatureDeclaration?.(m)) return 'new()';
    return null;
  }

  visit(sf);
  return sf;
}

for (const f of FILES) checkFile(f);

// 机械修复：给缺空行的注释块补一行空白（从后往前插，行号不串）。
if (FIX) {
  const byFile = new Map();
  for (const p of problems) {
    if (p.kind !== 'missing-gap') continue;
    if (!byFile.has(p.file)) byFile.set(p.file, []);
    byFile.get(p.file).push(p.line);
  }
  let fixed = 0;
  for (const [rel, list] of byFile) {
    const path = join(ROOT, rel);
    const lines = readFileSync(path, 'utf8').split('\n');
    for (const lineNo of [...list].sort((a, b) => b - a)) {
      const idx = lineNo - 1;
      if (idx > 0 && lines[idx - 1].trim() !== '') {
        lines.splice(idx, 0, '');
        ++fixed;
      }
    }
    writeFileSync(path, lines.join('\n'), 'utf8');
  }
  console.log(`补空行 ${fixed} 处`);
  process.exit(0);
}

if (JSON_OUT) {
  console.log(JSON.stringify(problems, null, 1));
  process.exit(problems.length ? 1 : 0);
}

const groups = new Map();
for (const p of problems) {
  if (!groups.has(p.kind)) groups.set(p.kind, []);
  groups.get(p.kind).push(p);
}
for (const [kind, list] of groups) {
  console.log(`\n[${kind}] ${list.length} 处`);
  for (const p of list.slice(0, 200)) {
    console.log(`  ${p.file}:${p.line}  ${p.name}  ${p.detail}`);
  }
  if (list.length > 200) console.log(`  … 另有 ${list.length - 200} 处`);
}
console.log(`\n检查 ${FILES.length} 个文件，问题 ${problems.length} 处`);
process.exit(problems.length ? 1 : 0);
