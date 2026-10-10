#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把一次 WorkBuddy 会话的原始 JSONL 轨迹导成可读文档。

原始文件是 append-only 的 JSONL，一行一条记录，类型有：
  session-meta / message(user|assistant) / reasoning / function_call /
  function_call_result / file-history-snapshot / ai-title

导出三份东西：
  会话记录.md    —— 用户提问 + 助手正文（全文），每轮附折叠的工具调用清单
  会话记录.html  —— 在 md 基础上再带思考过程、工具入参、工具结果（超长截断），可折叠
  session-*.raw.jsonl —— 原始文件逐字节副本
另把会话里引用到的图片 blob 复制到 images/ 下。

用法：
  python _export_session.py <原始 jsonl 路径> <输出目录>
"""

import json
import os
import re
import shutil
import sys
import html as html_mod
from datetime import datetime

sys.stdout.reconfigure(encoding="utf-8")
sys.stderr.reconfigure(encoding="utf-8")

USER_QUERY_RE = re.compile(r"<user_query>(.*?)</user_query>", re.S)
REMINDER_RE = re.compile(r"<system-reminder[^>]*>.*?</system-reminder>", re.S)
NOTE_MARKERS = [
    ("<craft_mode>", "系统：切换到 Agent 模式"),
    ("<plan_mode>", "系统：切换到 Plan 模式"),
    ("<ask_mode>", "系统：切换到 Ask 模式"),
]

# 截断预算（HTML 版）
TRUNC_REASONING = 800
TRUNC_ARG = 400
TRUNC_RESULT = 900
TRUNC_RESULT_TAIL = 200


def ts_str(ms):
    if not ms:
        return ""
    return datetime.fromtimestamp(ms / 1000).strftime("%Y-%m-%d %H:%M:%S")


def trunc(s, n, tail=0, label="省略"):
    if s is None:
        return ""
    if len(s) <= n:
        return s
    if tail and n > tail + 40:
        cut = len(s) - n
        return s[: n - tail] + f"\n……（中间{label} {cut} 字符）……\n" + s[-tail:]
    return s[:n] + f"\n……（后续{label} {len(s) - n} 字符）……"


def one_line(s, n=200):
    s = re.sub(r"\s+", " ", (s or "").strip())
    return s[:n] + ("…" if len(s) > n else "")


def summarize_call(name, raw_args):
    """把工具入参压成一行，便于在目录/折叠栏里扫读。"""
    args = {}
    if isinstance(raw_args, str):
        try:
            args = json.loads(raw_args)
        except Exception:
            return one_line(raw_args)
    elif isinstance(raw_args, dict):
        args = raw_args
    if not isinstance(args, dict):
        return one_line(str(args))
    for key in ("command", "file_path", "notebook_path", "path", "pattern",
                "query", "url", "prompt", "description", "content"):
        if key in args and args[key]:
            v = args[key]
            if key == "content":
                v = one_line(v, 80)
            return one_line(str(v))
    if "description" in args and args["description"]:
        return one_line(str(args["description"]))
    return one_line(json.dumps(args, ensure_ascii=False))


class Turn:
    def __init__(self, idx, ts, query, images):
        self.idx = idx
        self.ts = ts
        self.query = query
        self.images = images          # [{path, orig}]
        self.assistant = []           # [text]
        self.reasoning = []           # [text]
        self.calls = []               # [{name, summary, args, result, status}]
        self.notes = []               # [str]


def parse(path):
    turns = []
    preamble = Turn(0, None, None, [])
    cur = preamble
    pending = {}          # callId -> call dict
    order = []            # 保持调用顺序的 (name, callId)
    title = None
    cwd = None
    t_min = t_max = None
    n_snapshot = 0
    n_raw = 0

    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            n_raw += 1
            try:
                o = json.loads(line)
            except Exception:
                continue
            t = o.get("type")
            ts = o.get("timestamp")
            if ts:
                t_min = ts if t_min is None else min(t_min, ts)
                t_max = ts if t_max is None else max(t_max, ts)
            if o.get("cwd"):
                cwd = o["cwd"]

            if t == "ai-title":
                title = o.get("aiTitle") or title
            elif t == "file-history-snapshot":
                n_snapshot += 1
            elif t == "message":
                role = o.get("role")
                parts = o.get("content") or []
                if role == "user":
                    text = "\n".join(p.get("text", "") for p in parts
                                     if p.get("type") == "input_text")
                    imgs = []
                    for p in parts:
                        if p.get("type") == "image_blob_ref":
                            imgs.append({"path": p.get("blob_path"),
                                         "blob_id": p.get("blob_id"),
                                         "orig": p.get("original_filename")})
                    m = USER_QUERY_RE.search(text)
                    if m:
                        q = m.group(1).strip()
                        cur = Turn(len(turns) + 1, ts, q, imgs)
                        turns.append(cur)
                    else:
                        # 注入的上下文/模式切换，没有真实提问
                        stripped = REMINDER_RE.sub("", text).strip()
                        if not stripped:
                            note = "系统注入上下文"
                            for marker, label in NOTE_MARKERS:
                                if marker in text:
                                    note = label
                                    break
                            cur.notes.append(note)
                        else:
                            cur.notes.append(one_line(stripped, 200))
                elif role == "assistant":
                    txt = "\n".join(p.get("text", "") for p in parts
                                    if p.get("type") == "output_text").strip()
                    if txt:
                        cur.assistant.append(txt)
            elif t == "reasoning":
                raw = o.get("rawContent") or []
                txt = "\n".join(p.get("text", "") for p in raw
                                if isinstance(p, dict)).strip()
                if not txt:
                    txt = "\n".join(p.get("text", "") for p in (o.get("content") or [])
                                    if isinstance(p, dict)).strip()
                if txt:
                    cur.reasoning.append(txt)
            elif t == "function_call":
                cid = o.get("callId") or o.get("id")
                call = {"name": o.get("name") or "?",
                        "args": o.get("arguments"),
                        "summary": summarize_call(o.get("name"), o.get("arguments")),
                        "result": "", "status": ""}
                cur.calls.append(call)
                if cid:
                    pending[cid] = call
                    order.append(cid)
            elif t == "function_call_result":
                cid = o.get("callId")
                out = o.get("output") or {}
                txt = out.get("text") if isinstance(out, dict) else str(out)
                if txt is None:
                    txt = json.dumps(out, ensure_ascii=False)
                call = pending.get(cid)
                if call is None:
                    call = {"name": o.get("name") or "?", "args": None,
                            "summary": one_line(str(txt)), "result": "", "status": ""}
                    cur.calls.append(call)
                    pending[cid] = call
                call["result"] = txt
                call["status"] = o.get("status") or ""
    return dict(turns=turns, preamble=preamble, title=title, cwd=cwd,
                t_min=t_min, t_max=t_max, n_snapshot=n_snapshot, n_raw=n_raw)


def collect_images(sess):
    seen = {}
    for tn in sess["turns"]:
        for im in tn.images:
            p = im.get("path")
            if p and os.path.isfile(p) and p not in seen:
                seen[p] = im
    return seen


# ---------------------------------------------------------------- Markdown

def md_tool_lines(turn):
    if not turn.calls:
        return ""
    lines = [f"<details>", f"<summary>工具调用 {len(turn.calls)} 次</summary>", ""]
    for i, c in enumerate(turn.calls, 1):
        lines.append(f"{i}. `{c['name']}` {c['summary']}")
    lines += ["", "</details>", ""]
    return "\n".join(lines)


def write_md(sess, out_path, img_map):
    turns = sess["turns"]
    L = []
    L.append(f"# 会话记录 · {sess['sid']}")
    L.append("")
    if sess["title"]:
        L.append(f"- **标题**：{sess['title']}")
    if sess["cwd"]:
        L.append(f"- **工作目录**：`{sess['cwd']}`")
    L.append(f"- **时间范围**：{ts_str(sess['t_min'])} ~ {ts_str(sess['t_max'])}（本地时间）")
    L.append(f"- **规模**：{len(turns)} 轮用户提问 · "
             f"{sum(len(t.assistant) for t in turns)} 条助手回复 · "
             f"{sum(len(t.calls) for t in turns)} 次工具调用")
    L.append(f"- **导出时间**：{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    L.append("")
    L.append("> 本文件是「对话正文」版：用户提问与助手回复全文照录，"
             "每轮末尾把工具调用收进折叠块。含思考过程与工具结果的完整轨迹见 "
             "`会话记录.html`；逐字节原始数据见同名 `.raw.jsonl`。")
    L.append("")
    if sess["preamble"].notes:
        L.append("> 开场前系统注入：" + "；".join(sess["preamble"].notes))
        L.append("")
    L.append("## 目录")
    L.append("")
    for t in turns:
        head = one_line(t.query, 46)
        L.append(f"- [第 {t.idx} 轮 · {ts_str(t.ts)[:16]} · {head}](#t{t.idx})")
    L.append("")
    L.append("---")
    L.append("")
    for t in turns:
        L.append(f'<a id="t{t.idx}"></a>')
        L.append("")
        L.append(f"## 第 {t.idx} 轮 · {ts_str(t.ts)}")
        L.append("")
        if t.notes:
            for n in t.notes:
                L.append(f"> _（{n}）_")
            L.append("")
        L.append("### 用户")
        L.append("")
        L.append(t.query)
        L.append("")
        for im in t.images:
            b = img_map.get(im.get("path"))
            if b:
                L.append(f"![{im.get('orig') or '截图'}](images/{b})")
                L.append("")
        L.append("### 助手")
        L.append("")
        if t.assistant:
            L.append("\n\n".join(t.assistant))
        else:
            L.append("_（本轮无正文输出）_")
        L.append("")
        tl = md_tool_lines(t)
        if tl:
            L.append(tl)
        L.append("---")
        L.append("")
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(L))
    return len("\n".join(L))


# ---------------------------------------------------------------- HTML

CSS = """
:root{--bg:#1b1b1f;--panel:#232329;--panel2:#1f1f24;--fg:#d8d8de;--dim:#8b8b96;
--line:#33333c;--user:#4da3ff;--asst:#63c98d;--tool:#c9a227;--think:#9a7fd4;--err:#e06c75;}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.75 -apple-system,"Segoe UI","Microsoft YaHei",Roboto,sans-serif;}
header{position:sticky;top:0;z-index:9;background:rgba(27,27,31,.94);
backdrop-filter:blur(6px);border-bottom:1px solid var(--line);padding:12px 24px}
header h1{margin:0 0 4px;font-size:16px;color:#fff;font-weight:600}
header .meta{color:var(--dim);font-size:12px;line-height:1.9}
header .meta b{color:var(--fg);font-weight:500}
#toc{position:fixed;left:0;top:0;bottom:0;width:270px;overflow:auto;
background:var(--panel2);border-right:1px solid var(--line);padding:14px 10px;font-size:12px}
#toc a{display:block;color:var(--dim);text-decoration:none;padding:3px 6px;
border-radius:5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#toc a:hover{background:#2c2c34;color:var(--fg)}
#toc .ttl{color:#fff;font-weight:600;font-size:12px;padding:0 6px 8px;display:block}
main{margin-left:270px;padding:18px 26px 90px;max-width:1180px}
.turn{margin:0 0 26px;border-left:3px solid var(--line);padding-left:16px}
.turn.is-user{border-left-color:var(--user)}
h2{font-size:14px;color:#fff;margin:0 0 10px;font-weight:600}
h2 .no{background:var(--user);color:#0b1a2a;border-radius:5px;padding:1px 7px;
margin-right:8px;font-size:12px;font-weight:700}
h2 .time{color:var(--dim);font-weight:400;font-size:12px;margin-left:8px}
.block{margin:10px 0 14px}
.lbl{font-size:11px;letter-spacing:.08em;text-transform:uppercase;
color:var(--dim);margin-bottom:4px}
.q{white-space:pre-wrap;word-break:break-word;background:var(--panel);
border:1px solid var(--line);border-left:3px solid var(--user);
border-radius:6px;padding:10px 13px}
.a{white-space:pre-wrap;word-break:break-word;padding:2px 2px}
.note{color:var(--dim);font-style:italic;font-size:12px;margin:6px 0}
img.shot{max-width:min(760px,100%);border:1px solid var(--line);border-radius:6px;
margin:8px 0;display:block}
details{border:1px solid var(--line);border-radius:6px;background:var(--panel2);
margin:8px 0;padding:0 10px}
details[open]{padding-bottom:8px}
summary{cursor:pointer;color:var(--tool);font-size:12px;padding:7px 0;user-select:none}
summary::marker{color:var(--dim)}
.c{display:flex;gap:8px;padding:5px 0;border-top:1px dashed var(--line);align-items:baseline}
.c .i{color:var(--dim);font-size:11px;min-width:26px;text-align:right}
.c .n{color:var(--tool);font-size:12px;font-weight:600;min-width:80px}
.c .s{color:var(--fg);font-size:12px;word-break:break-all;opacity:.85}
pre{background:#161619;border:1px solid var(--line);border-radius:5px;padding:8px 10px;
overflow:auto;max-height:340px;font:12px/1.6 "Cascadia Mono",Consolas,monospace;
color:#c6c6cf;white-space:pre-wrap;word-break:break-word;margin:6px 0}
pre.err{border-color:#6b2b2f}
.rl{color:var(--think);font-size:11px;letter-spacing:.06em;text-transform:uppercase}
.top{position:fixed;right:18px;bottom:18px;background:var(--panel);border:1px solid var(--line);
color:var(--dim);border-radius:20px;padding:6px 13px;font-size:12px;text-decoration:none}
.top:hover{color:#fff}
"""

JS = """
document.querySelectorAll('details').forEach(function(d,i){
  if(d.dataset.open==='1') d.open=true;
});
window.addEventListener('keydown',function(e){
  if(e.key==='Escape'){document.querySelectorAll('details').forEach(function(d){d.open=false;});}
});
"""


def esc(s):
    return html_mod.escape(s or "")


def is_errish(s):
    if not s:
        return False
    low = s.lower()
    return ("error" in low or "failed" in low or "traceback" in low
            or "exception" in low or "错误" in s or "失败" in s)


def write_html(sess, out_path, img_map):
    turns = sess["turns"]
    H = []
    H.append("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>")
    title = sess["title"] or "会话记录"
    H.append(f"<title>{esc(title)}</title>")
    H.append(f"<style>{CSS}</style></head><body>")

    # TOC
    H.append("<nav id='toc'><span class='ttl'>会话目录 · "
             f"{len(turns)} 轮</span>")
    for t in turns:
        H.append(f"<a href='#t{t.idx}'>#{t.idx} {esc(one_line(t.query, 26))}</a>")
    H.append("</nav>")

    H.append("<main>")
    H.append("<header><h1>" + esc(title) + "</h1><div class='meta'>")
    H.append(f"会话 ID：<b>{esc(sess['sid'])}</b>　·　"
             f"工作目录：<b>{esc(sess['cwd'] or '')}</b><br>")
    H.append(f"时间范围：<b>{ts_str(sess['t_min'])}</b> ~ <b>{ts_str(sess['t_max'])}</b>　·　"
             f"轮次 <b>{len(turns)}</b>　·　工具调用 <b>{sum(len(t.calls) for t in turns)}</b>　·　"
             f"导出 <b>{datetime.now().strftime('%Y-%m-%d %H:%M')}</b>")
    H.append("</div></header>")

    for t in turns:
        H.append(f"<section class='turn is-user' id='t{t.idx}'>")
        H.append(f"<h2><span class='no'>{t.idx}</span>{esc(one_line(t.query, 40))}"
                 f"<span class='time'>{ts_str(t.ts)}</span></h2>")
        for n in t.notes:
            H.append(f"<div class='note'>（{esc(n)}）</div>")
        H.append("<div class='block'><div class='lbl'>用户</div>")
        H.append(f"<div class='q'>{esc(t.query)}</div>")
        for im in t.images:
            b = img_map.get(im.get("path"))
            if b:
                H.append(f"<img class='shot' src='images/{esc(b)}' "
                         f"alt='{esc(im.get('orig') or '')}'>")
        H.append("</div>")

        H.append("<div class='block'><div class='lbl'>助手</div>")
        if t.assistant:
            for txt in t.assistant:
                H.append(f"<div class='a'>{esc(txt)}</div>")
        else:
            H.append("<div class='note'>（本轮无正文输出）</div>")
        H.append("</div>")

        if t.reasoning:
            joined = "\n\n———\n\n".join(trunc(r, TRUNC_REASONING, label="省略")
                                      for r in t.reasoning)
            H.append("<details><summary>思考过程 " + str(len(t.reasoning)) +
                     " 段</summary><span class='rl'>reasoning</span>"
                     f"<pre>{esc(joined)}</pre></details>")

        if t.calls:
            H.append("<details><summary>工具调用 " + str(len(t.calls)) +
                     " 次（点击展开入参与结果）</summary>")
            for i, c in enumerate(t.calls, 1):
                H.append("<div class='c'><span class='i'>" + str(i) + "</span>" +
                         "<span class='n'>" + esc(c["name"]) + "</span>" +
                         "<span class='s'>" + esc(one_line(c["summary"], 260)) +
                         "</span></div>")
                if c["args"]:
                    argtxt = c["args"] if isinstance(c["args"], str) else \
                        json.dumps(c["args"], ensure_ascii=False, indent=2)
                    H.append(f"<pre>{esc(trunc(argtxt, TRUNC_ARG, label='省略'))}</pre>")
                if c["result"]:
                    cls = "pre err" if is_errish(c["result"]) else "pre"
                    H.append("<div class='rl'>result</div>")
                    H.append(f"<{cls.replace('pre', 'pre')} class='{cls}'>" +
                             esc(trunc(c["result"], TRUNC_RESULT,
                                       TRUNC_RESULT_TAIL, label="省略")) +
                             "</pre>")
            H.append("</details>")
        H.append("</section>")

    H.append("</main><a class='top' href='#toc'>↑ 目录</a>")
    H.append(f"<script>{JS}</script></body></html>")
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(H))
    return len("\n".join(H))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    src, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    sess = parse(src)
    sess["sid"] = os.path.basename(src).split(".")[0]

    img_dir = os.path.join(outdir, "images")
    os.makedirs(img_dir, exist_ok=True)
    img_map = {}
    for path, im in collect_images(sess).items():
        ext = os.path.splitext(path)[1] or ".png"
        name = (im.get("blob_id") or os.path.basename(path))[:16] + ext
        try:
            shutil.copy2(path, os.path.join(img_dir, name))
            img_map[path] = name
        except Exception as e:
            print("图片复制失败:", path, e)

    md_path = os.path.join(outdir, "会话记录.md")
    html_path = os.path.join(outdir, "会话记录.html")
    raw_path = os.path.join(outdir, sess["sid"] + ".raw.jsonl")

    n_md = write_md(sess, md_path, img_map)
    n_html = write_html(sess, html_path, img_map)
    if os.path.abspath(src) != os.path.abspath(raw_path):
        shutil.copy2(src, raw_path)

    print(f"轮次 {len(sess['turns'])} · 助手回复 {sum(len(t.assistant) for t in sess['turns'])} · "
          f"工具调用 {sum(len(t.calls) for t in sess['turns'])} · "
          f"思考段 {sum(len(t.reasoning) for t in sess['turns'])}")
    print(f"时间 {ts_str(sess['t_min'])} ~ {ts_str(sess['t_max'])}")
    print(f"{md_path}  {n_md / 1e6:.2f} MB")
    print(f"{html_path}  {n_html / 1e6:.2f} MB")
    print(f"{raw_path}  {os.path.getsize(raw_path) / 1e6:.2f} MB")
    print("图片", len(img_map), "张")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
