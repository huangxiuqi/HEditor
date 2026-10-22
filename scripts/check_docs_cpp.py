# -*- coding: utf-8 -*-
"""C++ 头文件的 Doxygen 检查：类/结构体成员要有块注释，方法要标 @param / @return。

为什么单独一份：lint_comments.py 管的是"注释本身怎么写"（反引号、装饰线、行宽），
这一份管"声明有没有注释、参数有没有标"。两者判据不同，合并会让报错混在一起。

判据（只对 src/core 下的头文件，生成的 *.gen.h 除外）：
  missing-doc    类/结构体作用域里的成员声明、命名空间层的函数声明，上方没有紧邻的块注释。
  missing-gap    有块注释，但它与上一条声明之间没有空行。
  missing-param  函数声明里有参数，块注释里缺对应的 @param。
  missing-return 非 void 的函数，块注释里缺 @return。
  trailing-dup   成员有块注释，行尾又写了一行 // 说明 —— 同一句话写两遍，
                 读的人要在"块里那句"与"行尾那句"之间对一遍。行尾注释要么删掉，
                 要么把它挪进块里（并入正文再删）。

几条刻意不查的：`= default` / `= delete` 的声明（编译器生成的，语义在类型自己的注释里）、
运算符重载（== 的语义读签名就知道，硬写 @param o 是噪音）、多行声明的续行（不是新成员）。

用法：
  python scripts/check_docs_cpp.py            # 报告，有问题退出码 1
  python scripts/check_docs_cpp.py --fix-gap  # 只补"缺空行"这一类
  python scripts/check_docs_cpp.py -v         # 打印违规原文
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TYPE_WORDS = {
    'void', 'int', 'bool', 'float', 'double', 'char', 'size_t', 'uint8_t', 'uint16_t',
    'uint32_t', 'uint64_t', 'int8_t', 'int16_t', 'int32_t', 'int64_t', 'auto',
}


def strip_code(line):
    """去掉行尾 // 注释之外的东西不管，先把字符串与字符量粗略挖掉。"""
    s = re.sub(r'"(\\.|[^"\\])*"', '""', line)
    s = re.sub(r"'(\\.|[^'\\])*'", "''", s)
    return s


def analyze(lines):
    """逐字符走一遍：去掉注释与字符串，返回每行的"行前深度"与"去注释后的代码"。

    按行做正则剥注释是不够的：块注释会跨行，`/* ... */` 里的花括号会把深度算错，
    于是一个类体永远等不到它的 `}`（bitmap.h 就被整份跳过了）。字符串里的花括号同理。
    """
    state = 'code'
    code_lines = []
    for raw in lines:
        buf = []
        i = 0
        while i < len(raw):
            ch = raw[i]
            if state == 'code':
                if ch == '/' and i + 1 < len(raw) and raw[i + 1] == '/':
                    break
                if ch == '/' and i + 1 < len(raw) and raw[i + 1] == '*':
                    state = 'block'
                    i += 2
                    continue
                if ch == '"':
                    state = 'string'
                    buf.append(' ')
                    i += 1
                    continue
                if ch == "'":
                    state = 'char'
                    buf.append(' ')
                    i += 1
                    continue
                buf.append(ch)
            elif state == 'block':
                if ch == '*' and i + 1 < len(raw) and raw[i + 1] == '/':
                    state = 'code'
                    i += 2
                    continue
            else:  # string / char
                if ch == '\\':
                    i += 2
                    continue
                if (state == 'string' and ch == '"') or (state == 'char' and ch == "'"):
                    state = 'code'
            i += 1
        code_lines.append(''.join(buf))
    return code_lines


def scan(path):
    """返回 ([(类体起始行, 结束行, 体内第一层的深度)], 原始行)。

    类体按"class/struct 名字 ... { ... }"配对；前向声明（`class Foo;`）不是类体。
    """
    lines = open(path, encoding='utf-8').read().split('\n')
    code = analyze(lines)
    ranges = []         # [起始行, 结束行, 体内第一层的深度]
    stack = []          # 每层类体：(开括号之后的深度, ranges 里的下标)
    pending = False     # 看到了 class/struct 名字，还在等它的 {
    depth = 0
    for i, line in enumerate(code):
        m = re.match(r'^\s*(?:template\s*<[^>]*>\s*)?(struct|class)\s+[A-Za-z_]', line)
        if m:
            # 前向声明：这一行以 ; 收尾且没有 {
            if '{' not in line and line.rstrip().endswith(';'):
                pending = False
            else:
                pending = True
        for ch in line:
            if ch == ';' and pending:
                pending = False
            elif ch == '{':
                depth += 1
                if pending:
                    ranges.append([i, None, depth])
                    stack.append((depth, len(ranges) - 1))
                    pending = False
            elif ch == '}':
                depth -= 1
                # 类体在深度掉到"开括号那一层之下"时才结束 —— 掉回同一层只是内联函数体
                # 结束（它的 `}` 正好把深度还原成类体那层）。
                if stack and depth == stack[-1][0] - 1:
                    _, ri = stack.pop()
                    ranges[ri][1] = i
    return [tuple(r) for r in ranges], lines


def in_class_body(ranges, idx):
    for a, b in ranges:
        if b is not None and a < idx <= b:
            return True
    return False


def class_depth(ranges, idx):
    """在类体里的第几层（1 = 直接成员）。用于跳过内联函数体里的局部变量。"""
    for a, b in ranges:
        if b is not None and a < idx <= b:
            return True
    return False


def strip_line(line):
    """去掉字符串/字符量与行尾注释，只留代码骨架。"""
    s = re.sub(r'"(\\.|[^"\\])*"', '""', line)
    s = re.sub(r"'(\\.|[^'\\])*'", "''", s)
    s = re.sub(r'//.*$', '', s)
    return s


def decl_text(lines, i, limit=8):
    """从第 i 行起拼出一份声明：到括号外出现 ; 或 { 为止。

    参数默认值里会有 {}（std::vector 的 {}），所以不能"看到 { 就停" —— 得看它是不是
    在括号里。拼多了会把下一行的成员当成自己的参数（曾经把 A4 的 margin_pt_ 记到
    slab_pt 头上）。
    """
    out = []
    depth = 0
    for k in range(i, min(i + limit, len(lines))):
        raw = strip_line(lines[k])
        out.append(raw.strip())
        for ch in raw:
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
            elif ch in ';{' and depth <= 0:
                return ' '.join(out)
    return ' '.join(out)


def parse_params(text):
    """从 'f(a b, c d = 1)' 里取参数名。只看第一层括号。"""
    start = text.find('(')
    if start < 0:
        return []
    depth = 0
    end = -1
    for k in range(start, len(text)):
        if text[k] == '(':
            depth += 1
        elif text[k] == ')':
            depth -= 1
            if depth == 0:
                end = k
                break
    if end < 0:
        return []
    raw = text[start + 1:end].strip()
    if not raw or raw == 'void':
        return []
    names = []
    depth = 0
    part = ''
    for ch in raw:
        if ch in '(<[':
            depth += 1
        elif ch in ')>]':
            depth -= 1
        if ch == ',' and depth == 0:
            names.append(part)
            part = ''
        else:
            part += ch
    names.append(part)
    out = []
    for part in names:
        part = part.split('=')[0].strip()
        m2 = re.search(r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?$', part)
        if m2:
            out.append(m2.group(1))
    return out


RET = re.compile(r'^\s*(?:virtual\s+|static\s+|inline\s+|explicit\s+|constexpr\s+)*'
                 r'([A-Za-z_][\w:<>,\s\*&]*?)\s*[\s\*&]+(~?[A-Za-z_]\w*|operator\S*)\s*\(')


def candidate_lines(lines, ranges):
    """哪些行是"一个声明的开头"，以及它要不要标参数与返回值。

    两类：类/结构体体里的直接成员（起始深度 = 体第一层），以及命名空间层的函数声明
    （起始深度 = 1）。后者不能漏 —— 头文件里大量接口就是命名空间层的自由函数。

    私有段里的成员只要求有块注释，不要求 @param / @return：那是内部实现，块注释把
    "返回 -1 表示没有"这类约定写在正文里就够了；把它们也按公开契约要求，只会逼出一堆
    "@return 结果"这种没有信息量的行。
    """
    depth_before = []
    depth = 0
    for raw in lines:
        depth_before.append(depth)
        line = strip_line(raw)
        depth += line.count('{') - line.count('}')

    out = []
    for a, b, body_depth in ranges:
        if b is None:
            continue
        # 是 class 还是 struct：默认访问权限不同。
        is_class = lines[a].strip().startswith('class')
        access = 'private' if is_class else 'public'
        for i in range(a, b):
            st = strip_line(lines[i]).strip()
            m = re.match(r'^(public|private|protected)\s*:', st)
            if m and depth_before[i] <= body_depth:
                access = m.group(1)
                continue
            m2 = re.match(r'^\s*(?:template\s*<[^>]*>\s*)?(?:struct|class)\s+([A-Za-z_]\w*)',
                          lines[a])
            owner = m2.group(1) if m2 else ''
            if depth_before[i] == body_depth and access in ('public', 'protected'):
                out.append((i, True, owner))
            elif depth_before[i] == body_depth:
                out.append((i, False, owner))

    for i, raw in enumerate(lines):
        if depth_before[i] != 1:
            continue
        st = strip_line(raw).strip()
        if not st or st[0] in '}{' or st.startswith(('//', '*', '/*', 'using ', 'typedef ',
                                                     'template', 'extern ', 'namespace', 'enum ',
                                                     '#')):
            continue
        decl = decl_text(lines, i)
        if not decl.rstrip().endswith(';'):
            continue  # 定义（内联函数体）与变量声明不在这里查
        if not RET.match(decl):
            continue
        out.append((i, True, ''))

    return out, depth_before


def check(path):
    ranges, lines = scan(path)
    if not ranges:
        return []
    out = []
    cands, depth_before = candidate_lines(lines, ranges)
    for i, require_tags, owner in cands:
        st = strip_line(lines[i]).strip()
        if not st or st[0] in '}{' or st.startswith(('//', '*', '/*')):
            continue
        if st.startswith(('using ', 'typedef ', 'friend ', 'template', 'public:', 'private:',
                          'protected:', '#', 'enum ', 'struct ', 'class ', '};')):
            continue
        # 多行声明的续行不是新成员：上一行没以 ; { } 收尾，也不是注释/预处理。
        prev = ''
        for k in range(i - 1, -1, -1):
            if lines[k].strip():
                prev = strip_line(lines[k]).strip()
                break
        if prev and prev[-1] not in ';{}\\' and not prev.startswith(('#', '/', '*')):
            continue
        decl = decl_text(lines, i)
        # 编译器生成的比较运算符不写注释：语义在类型自己的注释里，写一句"等于"是复述。
        if re.search(r'=\s*(default|delete)\s*;', decl):
            continue
        has_block = i > 0 and lines[i - 1].strip().endswith('*/') and \
            not lines[i - 1].strip().startswith('//')
        # 有块注释又在行尾写一遍：同一句话出现两次。
        if has_block and re.search(r';\s+//\s*\S', lines[i]):
            out.append((path, i + 1, 'trailing-dup', st[:60]))
        if not has_block:
            # 命名空间层的变量（inline constexpr）不强制写块注释：它们本身就是要读的常量。
            if depth_before[i] == 1 and not RET.match(decl):
                continue
            out.append((path, i + 1, 'missing-doc', st[:60]))
            continue
        k = i - 1
        while k >= 0 and not lines[k].strip().startswith('/**'):
            k -= 1
        brace_before = k > 0 and lines[k - 1].strip().endswith('{')
        if k > 0 and lines[k - 1].strip() != '' and not brace_before:
            out.append((path, k + 1, 'missing-gap', st[:60]))
        if not require_tags:
            continue
        block = '\n'.join(lines[k:i])
        # 运算符不标参数与返回值：== / != 的语义读签名就知道，硬写 @param o 是噪音。
        if re.search(r'\boperator\s*[^\s(]*\s*\(', decl):
            continue
        for p in parse_params(decl):
            if not re.search(r'@param(?:\[[^\]]*\])?\s+' + re.escape(p) + r'\b', block):
                out.append((path, k + 1, 'missing-param', f'{st[:40]} 缺 @param {p}'))
        m = RET.match(decl)
        # 构造函数没有返回类型：名字等于所在类的名字时不是"返回这个类"。
        if m and m.group(2) == owner:
            continue
        if m and m.group(1).strip() not in ('void', 'return', 'else') and \
                not st.startswith('~'):
            if not re.search(r'@returns?\b', block):
                out.append((path, k + 1, 'missing-return', f'{st[:40]} 缺 @return'))
    return out


def main():
    verbose = '-v' in sys.argv
    fix_gap = '--fix-gap' in sys.argv
    # 生成的文件不查：注释会被下一次生成覆盖掉，写了等于没写。
    files = [f for f in sorted(glob.glob(os.path.join(ROOT, 'src/core/**/*.h'), recursive=True))
             if not f.endswith('.gen.h')]
    problems = []
    for f in files:
        problems.extend(check(f))

    if fix_gap:
        by_file = {}
        for path, line, kind, _ in problems:
            if kind == 'missing-gap':
                by_file.setdefault(path, []).append(line)
        n = 0
        for path, rows in by_file.items():
            lines = open(path, encoding='utf-8').read().split('\n')
            for line in sorted(rows, reverse=True):
                idx = line - 1
                if idx > 0 and lines[idx - 1].strip() != '':
                    lines.insert(idx, '')
                    n += 1
            open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
        print('补空行 %d 处' % n)
        return 0

    if '--json' in sys.argv:
        import json
        print(json.dumps([
            {'file': os.path.relpath(p, ROOT), 'line': l, 'kind': k, 'text': t}
            for p, l, k, t in problems
        ], ensure_ascii=False, indent=1))
        return 1 if problems else 0

    groups = {}
    for path, line, kind, text in problems:
        groups.setdefault(kind, []).append((path, line, text))
    for kind, rows in sorted(groups.items()):
        print('\n[%s] %d 处' % (kind, len(rows)))
        for path, line, text in rows[:60]:
            rel = os.path.relpath(path, ROOT)
            print(f'  {rel}:{line}  {text}')
        if len(rows) > 60:
            print('  … 另有 %d 处' % (len(rows) - 60))
        if verbose and rows:
            print('  原文：', rows[0][2])
    print('\n检查 %d 个头文件，问题 %d 处' % (len(files), len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
