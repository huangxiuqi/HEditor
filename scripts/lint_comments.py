# -*- coding: utf-8 -*-
"""检查注释是否符合 AGENTS.md 的约定，并打印违规位置。

规则：
  R1   注释里出现 markdown 反引号
  R2   装饰分隔线（连续的 - = 或制表线字符）
  R3   markdown 强调（星号成对）或残留的星标/对勾/警告符号
  R4   流水账与变更历史用语（原来、上一版、第一版、踩过、用户报的 等）
  R5   第一/第二人称口语（我们、咱们、你）
  R6   行首的 markdown 列表符号（星号、减号、中点）
  R7   注释行超过 100 列
  R8   块注释超过 12 行、文件头超过 20 行（提示压缩）
  R9   C/C++ 声明位置用了 // 行注释（该处必须是块注释）
  R10  注释里用编号记号列举（圈号 ①，或行首 1) / (1) / 一、）
  R11  口语与俚语（坑、兜底、顺手、搞混 等，应写成技术结论）

R9 的判据是花括号深度：类/结构体/命名空间/文件作用域内的注释块紧贴声明时算"声明处"，
函数体内的注释不算（那是行内说明，本来就该用 //）。

用法：
  python scripts/lint_comments.py            # 检查，有违规则退出码 1
  python scripts/lint_comments.py -v         # 额外打印违规原文
"""

import io
import os
import re
import sys
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CPP = ['src/**/*.h', 'src/**/*.hpp', 'src/**/*.cpp', 'tests/*.cpp']
JS = ['scripts/*.mjs', 'web/*.js']

R4 = re.compile(
    '原来|以前|上一版|这一版|第一版|第二版|早先|曾经|当时|那天|一开始|起初|最初|'
    '(?<!自)上次|踩过|踩到|真踩|实测过|实测：|用户报的|用户看到|用户原话|用户实测|'
    '别删|别改|别调换|改回去|我们|咱们|这次|那一次|这版'
)
R5 = re.compile('你(?![们的])|咱')
R3 = re.compile(r'\*\*|★|☆|✓|✅|⚠|✗|❌|[\U0001F300-\U0001FAFF]')
R2 = re.compile(r'[─━═]{2,}|-{4,}|={4,}')
R6 = re.compile(r'^\s*(?://|\*)?\s*[-*·]\s+\S')
# 圈号（当作列表符）、行首数字/中文序号
R10 = re.compile(r'[①②③④⑤⑥⑦⑧⑨⑩]|^\s*(?:/\*+|//|\*)?\s*[（(]?\d{1,2}[).、）]\s+\S|'
                 r'^\s*(?:/\*+|//|\*)?\s*[（(][一二三四五六七八九十][）)]')
R11 = re.compile('坑|兜底|顺手|索性|干脆|居然|搞|悄悄|偷偷|一口气|拍脑袋|魔法|崩了|'
                 '作弊|魔鬼|白做|白重画|白写|硬塞|犯傻')

# 声明行：以类型/关键字开头，或以大写字母开头的类型名开头
DECL_START = re.compile(
    r'^(?:void|int|bool|float|double|char|size_t|uint\w+|int\w+|long|short|unsigned|'
    r'std::|const|constexpr|static|struct|class|enum|union|using|template|virtual|'
    r'explicit|inline|friend|operator|~?\w+::|~?[A-Z]\w*)\b'
)
BRACE_SCOPE = re.compile(r'^(struct|class|enum|union|namespace)\b')


def comment_text(line):
    """返回该行作为注释的内容；不是注释行则返回 None。"""
    s = line.strip()
    if s.startswith('//'):
        return s[2:]
    if s.startswith('*') and not s.startswith('*/'):
        return s[1:]
    if s.startswith('/*'):
        return s[3:]
    return None


def strip_code(line, state):
    """剥掉字符串字面量与注释，返回剩下的代码和新的块注释状态（0 普通 / 1 块注释中）。"""
    out = []
    i = 0
    while i < len(line):
        if state == 1:
            j = line.find('*/', i)
            if j < 0:
                return ''.join(out), 1
            state = 0
            i = j + 2
            continue
        c = line[i]
        if c == '/' and line[i + 1:i + 2] == '/':
            break
        if c == '/' and line[i + 1:i + 2] == '*':
            state = 1
            i += 2
            continue
        if c in '"\'`':
            q = c
            i += 1
            while i < len(line):
                if line[i] == '\\':
                    i += 2
                    continue
                if line[i] == q:
                    i += 1
                    break
                i += 1
            continue
        out.append(c)
        i += 1
    return ''.join(out), state


def brace_kind(lines, i, code):
    """这一行的 { 开的是函数体（'func'）还是普通作用域（'block'）。

    { 独占一行时回看最近一行代码，拿到"头"再判 —— 否则会把函数体当成普通作用域，
    导致函数体内的注释被误判成"声明处注释"。
    """
    head = code.split('{')[0].strip()
    if not head:
        k = i - 1
        while k >= 0:
            s = lines[k].strip()
            if not s or s.startswith('//') or s.startswith('*') or s.startswith('/*'):
                k -= 1
                continue
            head = s
            break
    if BRACE_SCOPE.match(head):
        return 'block'
    return 'func' if ')' in head else 'block'


def looks_like_decl(s):
    """下一行是否像一条声明（用于判断上一段注释是不是"声明处注释"）。"""
    s = s.strip()
    if not s or s.startswith(('#', '}', '//', '/*', '*')):
        return False
    if s in ('public:', 'private:', 'protected:', '};', 'public', 'private', 'protected'):
        return False
    return bool(DECL_START.match(s))


def check_file(path, verbose, cpp):
    out = []
    src = io.open(path, encoding='utf-8').read()
    lines = src.split('\n')
    rel = os.path.relpath(path, ROOT).replace('\\', '/')
    block_len = 0
    hdr = False          # 当前块是不是文件头（文件开头的第一个块注释）
    stack = []           # 'func' / 'block'，只在 C/C++ 维护
    state = 0

    def bad(i, rule, what):
        msg = '%s:%d  %s  %s' % (rel, i, rule, what)
        if verbose:
            msg += '\n      ' + lines[i - 1].strip()
        out.append(msg)

    i = 0
    while i < len(lines):
        line = lines[i]
        text = comment_text(line)

        if text is None:
            block_len = 0
            hdr = False
            if cpp:
                code, state = strip_code(line, state)
                for ch in code:
                    if ch == '{':
                        stack.append(brace_kind(lines, i, code))
                    elif ch == '}':
                        if stack:
                            stack.pop()
            i += 1
            continue

        if block_len == 0 and not any(l.strip() for l in lines[:i]):
            hdr = True
        block_len += 1
        n = i + 1
        if '`' in text:
            bad(n, 'R1', '注释里的反引号')
        if R2.search(text):
            bad(n, 'R2', '装饰分隔线')
        if R3.search(text):
            bad(n, 'R3', 'markdown 强调或符号')
        if R4.search(text):
            bad(n, 'R4', '流水账/变更历史用语')
        if R5.search(text):
            bad(n, 'R5', '口语人称')
        if R6.match(line.replace('//', '// ', 1)) and re.match(r'^\s*(?://|\*)\s*[-*·]\s+\S', line):
            bad(n, 'R6', '行首列表符号')
        if len(line) > 100:
            bad(n, 'R7', '注释行超过 100 列')
        if R10.search(text):
            bad(n, 'R10', '编号记号列举')
        if R11.search(text):
            bad(n, 'R11', '口语/俚语')
        limit = 20 if (i - block_len + 1 == 0 or hdr) else 12
        if block_len == limit + 1 and re.match(r'^\s*(/\*\*|\*)', line):
            bad(n, 'R8', '块注释超过 %d 行' % limit)

        # R9：C/C++ 里"声明处"的注释块必须是块注释。只判一段 // 注释的**末行**
        # （一段里报一次即可），且必须紧贴后面的声明（允许中间空一行），且不在函数体内。
        if cpp and line.strip().startswith('//'):
            k = i + 1
            while k < len(lines) and not lines[k].strip():
                k += 1
            if k < len(lines):
                nxt = lines[k]
                if ('func' not in stack and not nxt.strip().startswith('//')
                        and looks_like_decl(nxt)):
                    bad(i + 1, 'R9', '声明处必须用块注释')
        i += 1
    return out


def main():
    verbose = '-v' in sys.argv
    files = []
    for pat in CPP + JS:
        files += glob.glob(os.path.join(ROOT, pat), recursive=True)
    files = sorted(set(files))
    all_bad = []
    for f in files:
        all_bad += check_file(f, verbose, f.endswith(('.h', '.hpp', '.cpp')))
    for m in all_bad:
        print(m)
    print('检查 %d 个文件，违规 %d 处' % (len(files), len(all_bad)))
    return 1 if all_bad else 0


if __name__ == '__main__':
    sys.exit(main())
