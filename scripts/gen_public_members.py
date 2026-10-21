"""从契约 `web/types/editor.d.ts` 生成公开成员名单，给运行期的白名单闸用。

为什么要生成：`web/src/guard.ts` 那个 Proxy 要一份"哪些键是公开的"清单。手抄一份就等于
第二份真相 —— 契约加了个方法、白名单忘了加，症状是"文档里写了、调用时抛不是公开 API"，
而且没有任何判据会红。这里从契约抽，`--check` 保证它不漂。

产物：web/src/public_members.gen.ts

用法：
    python scripts/gen_public_members.py            # 重生成
    python scripts/gen_public_members.py --check    # 只比对
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = 'web/types/editor.d.ts'
OUT = 'web/src/public_members.gen.ts'

# diag 由 debug 开关控制，不在默认白名单里 —— 它单独一个常量，guard 按模式加。
CONDITIONAL = {'diag'}


def members():
    text = io.open(os.path.join(ROOT, SRC), encoding='utf-8').read()
    start = text.index('export interface Editor {')
    end = text.index('\n}', start)
    body = text[start:end]
    names = []
    for line in body.split('\n'):
        # 泛型方法写成 `on<K extends ...>(`，所以分隔符也要认 '<'
        m = re.match(r'^  (?:readonly )?([A-Za-z_][A-Za-z0-9_]*)[(:<]', line)
        if m:
            names.append(m.group(1))
    return names


def render(names):
    pub = [n for n in names if n not in CONDITIONAL]
    out = [
        '// 由 scripts/gen_public_members.py 从 web/types/editor.d.ts 生成，勿手改。',
        '//',
        '// 用途：guard.ts 的白名单（未知成员一律抛），以及 api_check.mjs 的成员对拍。',
        '',
        '/** 契约里的公开成员（不含 diag —— 它由 debug 开关控制）。 */',
        'export const PUBLIC_MEMBERS: readonly string[] = [',
    ]
    for n in pub:
        out.append("  '%s'," % n)
    out.append('];')
    out.append('')
    out.append("/** 只有 createEditor({ debug: true }) 时才在实例上。 */")
    out.append("export const DIAG_MEMBER = 'diag';")
    out.append('')
    return '\n'.join(out)


def main():
    check = '--check' in sys.argv
    text = render(members())
    path = os.path.join(ROOT, OUT)
    if check:
        have = io.open(path, encoding='utf-8').read() if os.path.exists(path) else ''
        if have != text:
            print('公开成员名单与契约不一致：%s' % OUT)
            print('  重跑：python scripts/gen_public_members.py')
            return 1
        print('公开成员名单一致（%d 项 + diag）' % (len(members()) - len(CONDITIONAL)))
        return 0
    os.makedirs(os.path.dirname(path), exist_ok=True)
    io.open(path, 'w', encoding='utf-8', newline='\n').write(text)
    print('已生成 %s（%d 项 + diag）' % (OUT, len(members()) - len(CONDITIONAL)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
