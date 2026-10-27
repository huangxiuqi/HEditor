"""从 C++ 的写出代码抽载荷字段表，生成三份东西。

三份产物（第一份入库被 review，后两份由它生成）：

    docs/api/payloads.json                 载荷名 + 键名 + 粗种类 + 是否可选（按写出顺序）
    src/core/engine/payload_schema.gen.h   constexpr 键表，给 pk::checked 用
    web/types/payloads.gen.d.ts            TS interface，给实现与 diag 用

为什么是"从 C++ 抽"而不是手写 schema：C++ 的写出代码是唯一真相，手写一份就等于两份。
这个脚本把真相投影成一份快照；`--check` 保证快照与代码一致，`pk::checked` 保证运行时
写出来的键与快照一致。两个方向合起来，改键名不可能静默通过。

用法：
    python scripts/gen_payloads.py            # 重生成三份产物
    python scripts/gen_payloads.py --check    # 只比对，不一致退出 1（CI / 提交前用）

★ 只收**顶层**键。`linesJson` 每个 line 对象里还有十几个键，那些不在这里 —— 运行时检查
  也只比顶层，因为顶层就是"这份载荷有哪些字段"的定义。
★ `optional`：写在 `if` 里的键（C++ 花括号深度 > 1）可能不出现，检查按"有序子序列"比。
  这一列是判据的一部分，不是注释：非可选的键漏写会抛。
"""
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = [
    'src/wasm/wasm_main.cpp',
    'src/core/engine/engine_audit.cpp',
    'src/core/engine/engine_paint.cpp',
    'src/core/engine/engine_diag.cpp',
    'src/core/engine/engine_table.cpp',
    'src/core/engine/engine_image.cpp',
]
SCHEMA = 'docs/api/payloads.json'
HEADER = 'src/core/engine/payload_schema.gen.h'
TSD = 'web/types/payloads.gen.d.ts'

FUNC_RE = re.compile(r'\n(std::string (?:EditorEngine::)?([A-Za-z_][A-Za-z0-9_]*)\([^)]*\)[^{;]*\{)')
LIT_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
UNESCAPE = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\', "'": "'"}

# 少数键的值是结构体，靠"看源码怎么求值"分不出来（`<< r.anchor` 与 `<< 3` 长得一样）。
#   这一列只能人补，补的时候写清为什么。目前只有这两个。
KIND_OVERRIDES = {
    ('selectionReport', 'anchor'): 'obj',
    ('selectionReport', 'focus'): 'obj',
}


def mask_comments(text):
    """把注释字符替换成空格（等长），保留字符串与位置。

    为什么必须先做：源码的注释里会出现 ASCII 引号（"走到了局部路径但没省下来"与"根本
    没走到"），不遮掉的话正则会把注释当字面量，那条注释就插进了 JSON 结构流里 ——
    实测因此把紧跟其后的 `rebuildPath` 判成了"不是键"，契约表里少一个键，运行时 abort。
    等长替换是为了让所有源码下标继续有效（报错信息与 classify 都按原下标读）。
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                out[i] = ' '
                i += 1
            continue
        if ch == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
            continue
        if ch in '"\'':
            q = ch
            i += 1
            while i < n and text[i] != q:
                i += 2 if text[i] == '\\' else 1
            i += 1
            continue
        i += 1
    return ''.join(out)


def body_of(src, brace):
    """brace 是 '{' 的下标；从它后面开始数（这个括号本身不能再数一遍）。"""
    i = brace + 1
    depth = 1
    while i < len(src) and depth > 0:
        if src[i] == '{':
            depth += 1
        elif src[i] == '}':
            depth -= 1
        i += 1
    return src[brace:i]


def literal_stream(body, base):
    """把 C++ 的字符串字面量按顺序拼成一条流，并记住每个字符的源码位置。

    为什么不是直接在源码上数花括号：源码里同时有 C++ 的 `{`（循环、函数）与 JSON 的 `{`
    （字面量里的 `"{"`）。只有**字面量里的**那些属于 JSON。把它们拼起来之后，值的部分
    缺失（那是运行时插进去的）并不影响结构 —— 数字与字符串里不会再冒出花括号。
    """
    chars, src_at = [], []
    for m in LIT_RE.finditer(body):
        raw = m.group(1)
        i = 0
        while i < len(raw):
            ch = raw[i]
            if ch == '\\' and i + 1 < len(raw):
                chars.append(UNESCAPE.get(raw[i + 1], raw[i + 1]))
                src_at.append(base + m.start(1) + i)
                i += 2
                continue
            chars.append(ch)
            src_at.append(base + m.start(1) + i)
            i += 1
    return ''.join(chars), src_at


def code_depth(src, brace, at):
    """键所在的 C++ 花括号深度（函数体本身算 1）。跳过字符串与注释。"""
    depth, i = 1, brace + 1
    while i < at and i < len(src):
        ch = src[i]
        if ch == '/' and i + 1 < len(src) and src[i + 1] == '/':
            while i < at and src[i] != '\n':
                i += 1
            continue
        if ch == '/' and i + 1 < len(src) and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            i = (j + 2) if j >= 0 else at
            continue
        if ch in '"\'':
            q = ch
            i += 1
            while i < at and src[i] != q:
                i += 2 if src[i] == '\\' else 1
            i += 1
            continue
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        i += 1
    return depth


def top_keys(src, brace, body):
    """返回 [{'name','at','depth'}]，按写出顺序，只要最外层对象那一层。"""
    text, src_at = literal_stream(body, brace)
    depth = 0
    keys = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == '"':
            j = i + 1
            buf = []
            while j < n and text[j] != '"':
                buf.append(text[j])
                j += 1
            prev = text[:i].rstrip()[-1:] if text[:i].strip() else ''
            if depth == 1 and prev in ('{', ','):
                at = src_at[i + 1] if i + 1 < len(src_at) else brace
                keys.append({'name': ''.join(buf), 'at': at,
                             'depth': code_depth(src, brace, at)})
            i = j + 1
            continue
        if ch in '{[':
            depth += 1
        elif ch in '}]':
            depth -= 1
        i += 1
    return keys


def classify(src, at):
    """看键之后那一小段源码，判粗种类。只分五类，不区分 int 与小数位。

    at 指向键名的第一个字符（在字面量内部）。键与"值怎么来"可能在同一段字面量里
    （`"{\\"lines\\":["`），也可能在它后面的表达式里（`<< jsonEscape(x)`），两段都要看。
    """
    i = at
    while i < len(src):
        if src[i] == '\\':
            i += 2
            continue
        if src[i] == '"':
            break
        i += 1
    head = src[at:i].replace('\\', '')
    # 只看这一句（到 ';' 为止），并在下一个键字面量处截断：一条语句里可能连写好几个键，
    #   不截断的话「?'true':'false'」会命中**下一个键**的写法 —— 实测那正是把 num 判成
    #   bool 的来源（reusedParagraphs / totalParagraphs 都被冤枉过）。
    tail = src[i + 1:i + 240].split(';')[0]
    nxt = re.search(r'\\"[A-Za-z0-9_]+\\":', tail)
    if nxt:
        tail = tail[:nxt.start()]
    after = head.split(':', 1)[1] if ':' in head else ''
    if after.startswith('"'):
        after = after[1:]           # 键自己的收尾引号
    b = (after + tail).lstrip()
    if b.startswith('<<'):
        b = b[2:].lstrip()
    if b.startswith('[') or b.startswith('"['):
        return 'arr'
    if b.startswith('{') or b.startswith('"{'):
        return 'obj'
    if b.startswith('true') or b.startswith('false'):
        return 'bool'
    if '? "true" : "false"' in b[:120]:
        return 'bool'
    if 'jsonEscape' in b[:60]:
        return 'str'
    if b.startswith('*') or b.startswith('"'):
        return 'str'
    return 'num'


def extract():
    out = {}
    for rel in FILES:
        src = mask_comments(io.open(os.path.join(ROOT, rel), encoding='utf-8').read())
        for m in FUNC_RE.finditer(src):
            brace = m.end() - 1
            body = body_of(src, brace)
            if '\\"' not in body:
                continue
            # 转调别人的写出函数（auditFrame/auditBase 就是 -> diffJson）：键表属于被调的那份，
            #   在这里再生成一份只会得到"提前返回那一小段"的键，与实际输出对不上。
            if 'diffJson(' in body:
                continue
            keys = top_keys(src, brace, body)
            if not keys:
                continue
            name = m.group(2)[:-4] if m.group(2).endswith('Json') else m.group(2)
            # 同一个键可能出现在多条路径里：提前返回的字面量（`{"diff":-1,"cached":0}`）与
            #   主路径都会写 diff/cached。**顺序取最后一次出现**（主路径），可选性取"所有
            #   出现里只要有一次是必需的就算必需"。反过来取第一次的话，顺序会变成提前返回
            #   那一小段的顺序 —— 实测 auditPage 就是这样把 cached 排到了 w 前面。
            last = {}
            for i, k in enumerate(keys):
                last[k['name']] = i
            required_any = {}
            for k in keys:
                required_any[k['name']] = required_any.get(k['name'], True) and (k['depth'] > 1)
            rows = []
            for i, k in enumerate(keys):
                if last[k['name']] != i:
                    continue
                kind = KIND_OVERRIDES.get((name, k['name']), classify(src, k['at']))
                rows.append({'name': k['name'], 'kind': kind,
                             'optional': required_any[k['name']]})
            info = {'cxx': m.group(2), 'file': rel, 'keys': rows}
            # 有多个 return 的写出函数（提前返回）各条路的键集合不同 —— 例如
            #   auditPage 在"页缓存没有"时只给 diff/cached。这类载荷按有序子序列比：
            #   多出来的键、改名、顺序错仍然会红，只是不再要求"每个键都出现"。
            if len(re.findall(r'\breturn\b', body)) > 1:
                info['partial'] = True
            out[name] = info
    return out


def snake(name):
    return re.sub(r'(?<!^)(?=[A-Z])', '_', name).lower()


def pascal(name):
    return name[:1].upper() + name[1:]


def ts_type(kind):
    return {
        'num': 'number',
        'str': 'string',
        'bool': 'boolean',
        'arr': 'unknown[]',
        'obj': 'Record<string, unknown>',
    }[kind]


def schema_text(payloads):
    doc = {
        '_note': '由 scripts/gen_payloads.py 从 C++ 写出代码抽出，勿手改。',
        '_rule': 'keys 的顺序 = 写出顺序；kind 只有 num/str/bool/arr/obj 五类，'
                 '不区分 int 与小数位；optional 表示该键写在 if 里、可能不出现。',
        'payloads': {name: {
            'cxx': info['cxx'], 'file': info['file'],
            **({'partial': True} if info.get('partial') else {}),
            'keys': [{k2: v for k2, v in k.items() if not (k2 == 'optional' and not v)}
                     for k in info['keys']],
        } for name, info in payloads.items()},
    }
    return json.dumps(doc, ensure_ascii=False, indent=2) + '\n'


def header_text(payloads):
    out = [
        '// 由 scripts/gen_payloads.py 生成，勿手改。',
        '//',
        '// 来源：docs/api/payloads.json（C++ 写出代码的投影）。',
        '// 用途：pk::checked 拿它比对运行时写出的顶层键 —— 见 src/core/engine/payload_schema.h。',
        '#pragma once',
        '',
        '#include <iterator>',
        '#include <string_view>',
        '',
        'namespace pk {',
        '',
        '/**',
        ' * 一个键：名字 + 粗种类（种类只给 TS 用，C++ 侧只比名字）+ 是否可能缺席。',
        ' */',
        'struct KeySpec {',
        '    std::string_view name;',
        '    std::string_view kind;',
        '    bool optional;',
        '};',
        '',
        '/**',
        ' * 一份载荷：名字 + 按写出顺序排列的键 + 是否允许缺键（多出口）。',
        ' */',
        'struct PayloadSpec {',
        '    std::string_view name;',
        '    const KeySpec* keys;',
        '    int count;',
        '    bool partial;',
        '};',
        '',
    ]
    for name, info in payloads.items():
        out.append('namespace %s {' % snake(name))
        out.append('inline constexpr KeySpec kKeys[] = {')
        for k in info['keys']:
            opt = 'true' if k.get('optional') else 'false'
            out.append('    {"%s", "%s", %s},' % (k['name'], k['kind'], opt))
        out.append('};')
        out.append('inline constexpr PayloadSpec kSpec{"%s", kKeys, '
                   'static_cast<int>(std::size(kKeys)), %s};'
                   % (name, 'true' if info.get('partial') else 'false'))
        out.append('}  // namespace %s' % snake(name))
        out.append('')
    out.append('}  // namespace pk')
    out.append('')
    return '\n'.join(out)


def tsd_text(payloads):
    out = [
        '/**',
        ' * 由 scripts/gen_payloads.py 生成，勿手改。',
        ' *',
        ' * 这些是 read() 返回的原始载荷（`diag` 与实现内部用）。对外产品面用的是',
        ' * editor.d.ts 里的 Pos / Caret / Selection 这类形状，由实现从这里映射过去。',
        ' *',
        ' * optional 的键在类型上是可选的（`?`）：它们在 C++ 里写在 if 里，不保证出现。',
        ' */',
        '',
    ]
    for name, info in payloads.items():
        out.append('export interface %s {' % pascal(name))
        for k in info['keys']:
            q = '?' if k.get('optional') else ''
            out.append('  %s%s: %s;' % (k['name'], q, ts_type(k['kind'])))
        out.append('}')
        out.append('')
    out.append('/** 载荷名 -> 形状。readPayload() 用它做运行时校验。 */')
    out.append('export interface Payloads {')
    for name in payloads:
        out.append('  %s: %s;' % (name, pascal(name)))
    out.append('}')
    out.append('')
    out.append('export type PayloadName = keyof Payloads;')
    out.append('')
    return '\n'.join(out)


def main():
    check = '--check' in sys.argv
    payloads = extract()
    total = sum(len(v['keys']) for v in payloads.values())
    outputs = {
        SCHEMA: schema_text(payloads),
        HEADER: header_text(payloads),
        TSD: tsd_text(payloads),
    }
    if check:
        bad = []
        for rel, want in outputs.items():
            path = os.path.join(ROOT, rel)
            have = io.open(path, encoding='utf-8').read() if os.path.exists(path) else ''
            if have != want:
                bad.append(rel)
        if bad:
            print('载荷契约与 C++ 不一致：%s' % ', '.join(bad))
            print('  改过写出代码就重跑：python scripts/gen_payloads.py')
            print('  然后 review docs/api/payloads.json 的 diff —— 键名变了会连带影响 JS 读取点。')
            return 1
        print('载荷契约一致：%d 份载荷 / %d 个键（%s）' % (len(payloads), total, SCHEMA))
        return 0
    for rel, text in outputs.items():
        path = os.path.join(ROOT, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        io.open(path, 'w', encoding='utf-8', newline='\n').write(text)
    print('已生成 %d 份载荷 / %d 个键：' % (len(payloads), total))
    for rel in outputs:
        print('  ' + rel)
    return 0


if __name__ == '__main__':
    sys.exit(main())
