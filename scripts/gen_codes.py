#!/usr/bin/env python3
"""把 src/core/utils/api_codes.h 里的常量表生成为 web/src/codes.gen.ts。

为什么要生成：同一份数据抄在两边时不会报错，只会在某一天对不上 —— 面板上是 21 厘米、
写进文档是 20.99，或者日期选择器少一个单位。生成的失败方式是 check 红，不是用户看见。

用法：
  python scripts/gen_codes.py           生成
  python scripts/gen_codes.py --check   只比对，不一致退出码 1
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = 'src/core/utils/api_codes.h'
OUT = 'web/src/codes.gen.ts'

TABLE_RE = re.compile(r'inline constexpr (\w+) (k\w+)\[\] = \{(.*?)\n\};', re.S)
ROW_RE = re.compile(r'^\s*\{([^{}]*)\},?\s*$', re.M)
SCALAR_RE = re.compile(r'inline constexpr (\w+) (k\w+) = ([^;]+);')

# 表名 -> 生成物里的标识符与注释。新增一张表要在这里补一行，否则它不会被生成。
TABLES = {
    'kPaperSizes': (
        'PAPER_PT',
        '固定纸张的纵向宽高（磅）。横向由 paperSizePt 把宽高对调。',
        'name-w-h',
        'satisfies Record<string, { w: number; h: number }>',
    ),
    'kDateUnitBits': (
        'DATE_UNIT_BITS',
        '日期单位的位掩码，顺序与日期选择器里的先后一致。',
        'name-bit',
        'Record<string, number>',
    ),
    'kImageLayouts': (
        'IMAGE_LAYOUT_CODES',
        '图片版式的对外码。anchor 与 wrap 在引擎里是两个字段，对外只有一个码。',
        'name-code',
        'Record<string, number>',
    ),
    'kEditModes': ('EDIT_MODE_CODES', '编辑模式（与 EditMode 枚举同序）。', 'name-code',
                   'Record<string, number>'),
    'kMarksModes': ('MARKS_MODE_CODES', '不可见字符标记（与 MarksMode 枚举同序）。', 'name-code',
                    'Record<string, number>'),
}


def strip_f(text):
    """去掉 C++ 的 float 后缀；只对数字列调，不碰字符串。"""
    return re.sub(r'(?<=[0-9.])f$', '', text.strip())


def eval_number(text):
    """求一个数字列的值。位掩码写成 1u << n，去掉 u 之后交给 Python 算。"""
    t = strip_f(text).replace('u', '')
    v = eval(t)  # noqa: S307 —— 输入是本仓库自己的头文件
    return int(v) if float(v) == int(v) else float(v)


def eval_bits(text):
    """求位掩码列，返回整数。"""
    return eval_number(text)


def parse(src):
    tables = {}
    for m in TABLE_RE.finditer(src):
        ctype, name, body = m.group(1), m.group(2), m.group(3)
        rows = []
        for row in ROW_RE.finditer(body):
            cols = [c.strip() for c in row.group(1).split(',')]
            if len(cols) < 2:
                continue
            rows.append(cols)
        tables[name] = (ctype, rows)
    scalars = {m.group(2): m.group(3).strip() for m in SCALAR_RE.finditer(src)}
    return tables, scalars


def ts_value(kind, text):
    if kind == 'name':
        return "'" + text.strip().strip('"') + "'"
    n = eval_number(text)
    if isinstance(n, int):
        return str(n)
    return repr(n)


def render(tables, scalars):
    out = []
    out.append('// 由 scripts/gen_codes.py 从 src/core/utils/api_codes.h 生成，不要手改。')
    out.append('// 改常量请改那个头文件，再跑 python scripts/gen_codes.py。')
    out.append('')

    order = ['kPaperSizes', 'kDateUnitBits', 'kImageLayouts', 'kEditModes', 'kMarksModes']
    for name in order:
        if name not in tables:
            raise SystemExit('头文件里找不到表 ' + name)
        _, rows = tables[name]
        ident, doc, fields, anno = TABLES[name]
        kinds = fields.split('-')
        if anno.startswith('satisfies'):
            decl = 'export const ' + ident + ' = {'
            tail = ' ' + anno
        else:
            decl = 'export const ' + ident + ': ' + anno + ' = {'
            tail = ''
        lines = ['/**', ' * ' + doc, ' */', decl]
        for cols in rows:
            parts = []
            for kind, col in zip(kinds, cols):
                parts.append(ts_value(kind, col))
            if len(kinds) == 3:
                lines.append('  %s: { w: %s, h: %s },' % (parts[0], parts[1], parts[2]))
            else:
                lines.append('  %s: %s,' % (parts[0], parts[1]))
        lines.append('}' + tail + ';')
        out.extend(lines)
        out.append('')

    idx = scalars.get('kDefaultPaperIndex')
    if idx is None:
        raise SystemExit('头文件里找不到 kDefaultPaperIndex')
    _, paper_rows = tables['kPaperSizes']
    default_name = paper_rows[eval_number(idx)][0].strip().strip('"')
    out.append('/**')
    out.append(' * 默认纸张的规格名。空白文档与默认页度量都用它。')
    out.append(' */')
    out.append("export const DEFAULT_PAPER = '" + default_name + "';")
    out.append('')

    margin = scalars.get('kDefaultMarginPt')
    if margin is None:
        raise SystemExit('头文件里找不到 kDefaultMarginPt')
    out.append('/**')
    out.append(' * 默认页边距（磅），四边相等（2 厘米）。')
    out.append(' */')
    out.append('export const DEFAULT_MARGIN_PT = ' + ts_value('bit', margin) + ';')
    out.append('')
    return '\n'.join(out)


def check_engine_api(tables, scalars):
    """api.ts 必须零 import，那三个页度量常量只能手抄 —— 这里替它对账。"""
    src = io.open(os.path.join(ROOT, 'web/src/engine/api.ts'), encoding='utf-8').read()
    _, paper_rows = tables['kPaperSizes']
    row = paper_rows[eval_number(scalars['kDefaultPaperIndex'])]
    want = [
        ('kPageWidthPt', eval_number(row[1])),
        ('kPageHeightPt', eval_number(row[2])),
        ('kPageMarginPt', eval_number(scalars['kDefaultMarginPt'])),
    ]
    problems = []
    for key, val in want:
        m = re.search(r'const ' + key + r' = ([0-9.]+);', src)
        if m is None:
            problems.append('api.ts 里找不到 ' + key)
            continue
        if abs(float(m.group(1)) - float(val)) > 1e-6:
            problems.append('%s = %s，头文件里是 %s' % (key, m.group(1), val))
    return problems


def main():
    src = io.open(os.path.join(ROOT, SRC), encoding='utf-8').read()
    tables, scalars = parse(src)
    text = render(tables, scalars)
    path = os.path.join(ROOT, OUT)
    check = '--check' in sys.argv[1:]
    if check:
        old = io.open(path, encoding='utf-8').read() if os.path.exists(path) else ''
        bad = check_engine_api(tables, scalars)
        # 行尾归一：仓库是 CRLF 检出，比对的是内容不是换行符。
        if old.replace('\r\n', '\n') != text:
            bad.append('生成物不一致：' + OUT + '（跑 python scripts/gen_codes.py）')
        if bad:
            for b in bad:
                print(b)
            return 1
        print('跨语言常量一致：' + OUT + '（含 api.ts 的 boot 页度量）')
        return 0
    io.open(path, 'w', encoding='utf-8', newline='\n').write(text)
    print('已生成 ' + OUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())
