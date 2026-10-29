# -*- coding: utf-8 -*-
"""位置面结构闸：宿主能碰到的"位置与选区"入口只能在这份清单里。

要证伪的事：同一个坐标概念又长出第二个名字。删名很容易，长回来更容易 ——
新增一条 `setCaretAtSide` 不需要改任何文档，只要在 wasm 里注册、在命令表里加一行，
全仓立刻就有两个"落光标"。所以清单写在这里，进出一个名字都要改这个文件。

三条判据：
  export-drift  wasm 注册的位置类导出与清单不一致（多一个或少一个都算）。
  command-drift 命令表里的位置类命令与清单不一致。
  doc-affinity  文档层出现 affinity —— 它是本地视图态，换窗口宽度就失效，不进文档。

位置类的判定按名字：含 caret / position / select / extend / hit。按名字判会漏掉
"叫别的名字但收位置"的新入口，这是已知边界：那种情况要在评审里拦，脚本拦不住。

已知落在这条边界外的就是两个换算 `flowAt` / `textPosOf`（名字里没有上面那些词）：
它们是坐标之间唯一的桥，多出第三个才是要拦的事，而那种"第三个换算"同样得靠评审。

用法：
  python scripts/check_position_surface.py            # 报告，有问题退出码 1
  python scripts/check_position_surface.py -v         # 打印两边集合
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 位置类的名字识别：这些词一旦出现在导出或命令名里，就要在清单里认领。
POSITION_WORDS = re.compile(r'caret|position|select|extend|hit', re.IGNORECASE)

# wasm 导出（src/wasm/wasm_main.cpp 的 emscripten::function）。
# 分四族：落点（位置 / 地址 / 选区）、读写读数、命令式的移动与选择、外观与呈相。
WASM_EXPORTS = {
    'setCaretAt',            # 地址入口（唯一的地址坐标）
    'setCaretPosition',      # 位置入口：定位
    'extendTo',              # 位置入口：拖选
    'select',                # 选区入口（两端流位置 + 焦点端行别）
    'clearSelection',
    'selectionReport',
    'selectAll',
    'selectCellAtPara',
    'cellSelectAt',
    'selectWordAt',
    'selectParaAt',
    'selectImage',
    'moveSelectionTo',
    'copySelection',
    'deleteSelection',
    'hitTest',               # 唯一的几何入口
    'imageHit',
    'caretPositionJson',     # 读数
    'caretReport',
    'caretOffsetLive',
    'caretVerticalNow',
    'caretBlockReversedNow',
    'moveCaret',             # 命令式移动（收方向，不收位置）
    'moveCaretVertical',
    'tabCaret',
    'caretToLineStart',
    'caretToLineEnd',
    'caretToDocStart',
    'caretToDocEnd',
    'showCaret',             # 外观与呈相
    'caretStyleJson',
    'setCaretStyle',
    'selectionStyleJson',
    'setSelectionStyle',
}

# 命令表（web/src/engine/api.ts 的 COMMANDS 第一列）。
COMMANDS = {
    'setCaretAt',
    'setCaretPosition',
    'extendTo',
    'select',
    'selectPos',
    'clearSelection',
    'selectionReport',
    'selectAll',
    'selectCellAtPara',
    'cellSelectAt',
    'selectWord',
    'selectPara',
    'selectImage',
    'moveSelection',
    'deleteSelection',
    'hit',
    'imageHit',
    'caretPosition',
    'caretReport',
    'caretOffsetLive',
    'caretVerticalNow',
    'caretBlockReversedNow',
    'tabCaret',
    'caretStyle',
    'caretStyleJson',
    'selectionStyle',
    'selectionStyleJson',
}


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
        return f.read()


def wasm_exports():
    src = read('src/wasm/wasm_main.cpp')
    return set(re.findall(r'emscripten::function\("([A-Za-z0-9_]+)"', src))


def commands():
    src = read('web/src/engine/api.ts')
    return set(re.findall(r"^\s*\['([A-Za-z0-9_]+)'", src, re.M))


def positionish(names):
    return {n for n in names if POSITION_WORDS.search(n)}


def diff(actual, declared):
    extra = sorted(actual - declared)
    missing = sorted(declared - actual)
    return extra, missing


problems = []


def check(label, actual, declared):
    extra, missing = diff(actual, declared)
    if extra:
        problems.append(
            f'{label}：多出 {", ".join(extra)} —— 位置面只能有一个名字做一件事；'
            f'要么删掉，要么改这份清单并写清它和现有入口的分工')
    if missing:
        problems.append(f'{label}：清单里有 {", ".join(missing)}，代码里已经没有 —— 清单过期了')


wasm = wasm_exports()
cmds = commands()
check('wasm 导出', positionish(wasm), WASM_EXPORTS)
check('命令表', positionish(cmds), COMMANDS)

# 文档层不许出现 affinity：它是本地视图态，换窗口宽度就失效。
doc_dir = os.path.join(ROOT, 'src', 'core', 'doc')
offenders = []
for dirpath, _dirs, files in os.walk(doc_dir):
    for name in files:
        if not name.endswith(('.h', '.cpp')):
            continue
        path = os.path.join(dirpath, name)
        with open(path, encoding='utf-8') as f:
            for no, line in enumerate(f, 1):
                if 'affinity' in line:
                    rel = os.path.relpath(path, ROOT).replace('\\', '/')
                    offenders.append(f'{rel}:{no}')
if offenders:
    problems.append('文档层出现 affinity（它是视图态，不进文档）：' + ', '.join(offenders))

if '-v' in sys.argv:
    print('wasm 位置类：', ' '.join(sorted(positionish(wasm))))
    print('命令位置类：', ' '.join(sorted(positionish(cmds))))

if problems:
    print('位置面结构闸：{} 条问题'.format(len(problems)))
    for p in problems:
        print('  ' + p)
    sys.exit(1)

print('位置面结构闸：导出 {} 条、命令 {} 条，全部在清单内；文档层没有 affinity'.format(
    len(positionish(wasm)), len(positionish(cmds))))
