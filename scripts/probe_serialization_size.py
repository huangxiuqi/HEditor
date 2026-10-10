"""测量病历文档在 XML 与 JSON 两种语法下的体积构成。

样本取自参照项目 DCWriterLite 的 wwwroot/demoDocuments（23 份真实病历模板）。
对每份文档统计：原始字节、元素数、文字与属性值（有效载荷）、格式开销（标签与缩进）、
内嵌 base64 载荷；再按三种编码各序列化一遍并压缩，比较最终体积。

用法：
    python probe_serialization_size.py <目录> [--limit N] [--detail 文件名]
"""

import gzip
import io
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

B64 = re.compile(rb"[A-Za-z0-9+/]{200,}={0,2}")
WS = re.compile(rb">[ \t\r\n]+<")
EMPTY_SELF = re.compile(rb"<([A-Za-z_][\w:.-]*)((?:\s+[^<>]*?)?)\s*/>")
EMPTY_PAIR = re.compile(rb"<([A-Za-z_][\w:.-]*)((?:\s+[^<>]*?)?)\s*>\s*</\1>")


def overhead(raw):
    """把格式开销拆成三桶：标签间空白、空元素（自闭合与空对）、其余。"""
    squeezed = WS.sub(b"><", raw)
    blank = len(raw) - len(squeezed)
    empties = [m.group(0) for m in EMPTY_SELF.finditer(raw)]
    empties += [m.group(0) for m in EMPTY_PAIR.finditer(raw)]
    empty_bytes = sum(len(e) for e in empties)
    return {
        "blank": blank,
        "empty_bytes": empty_bytes,
        "empty_count": len(empties),
        "min_xml": len(squeezed),
        "gz_min_xml": len(gzip.compress(squeezed, 9)),
    }


def strip_ns(tag):
    return tag.split("}", 1)[1] if "}" in tag else tag


def walk(el, out):
    """把元素树摊平成 (标签, 属性字典, 文字) 三元组序列，深度优先。"""
    out.append((strip_ns(el.tag), dict(el.attrib), (el.text or "").strip()))
    for child in el:
        walk(child, out)


def encode_json_full(flat):
    """逐元素保留完整标签名与属性名，键名用完整单词。"""
    nodes = []
    for tag, attrs, text in flat:
        node = {"tag": tag}
        if attrs:
            node["attrs"] = dict(attrs)
        if text:
            node["text"] = text
        nodes.append(node)
    return json.dumps(nodes, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def encode_json_tight(flat):
    """标签名与属性名走符号表，键名取单字母，是 JSON 能达到的体积下限。"""
    tags, keys = {}, {}
    nodes = []
    for tag, attrs, text in flat:
        tid = tags.setdefault(tag, len(tags))
        rec = [tid]
        if attrs:
            pairs = []
            for k, v in attrs.items():
                pairs.append(keys.setdefault(k, len(keys)))
                pairs.append(v)
            rec.append(pairs)
        if text:
            rec.append(text)
        nodes.append(rec)
    payload = {"tags": list(tags), "keys": list(keys), "nodes": nodes}
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def measure(path):
    raw = open(path, "rb").read()
    root = ET.fromstring(raw)
    flat = []
    walk(root, flat)

    text_bytes = sum(len(t.encode("utf-8")) for _, _, t in flat if t)
    attr_bytes = sum(
        len(v.encode("utf-8")) for _, attrs, _ in flat for v in attrs.values()
    )
    b64_bytes = sum(len(m.group(0)) for m in B64.finditer(raw))

    jf = encode_json_full(flat)
    jt = encode_json_tight(flat)

    row = {
        "name": os.path.basename(path),
        "raw": len(raw),
        "elems": len(flat),
        "text": text_bytes,
        "attr": attr_bytes,
        "b64": b64_bytes,
        "json_full": len(jf),
        "json_tight": len(jt),
        "gz_xml": len(gzip.compress(raw, 9)),
        "gz_json_full": len(gzip.compress(jf, 9)),
        "gz_json_tight": len(gzip.compress(jt, 9)),
    }
    row.update(overhead(raw))
    return row


def mb(n):
    return f"{n / 1048576:.2f}"


def main():
    args = sys.argv[1:]
    root = args[0] if args and not args[0].startswith("--") else "."
    limit = 0
    detail = None
    if "--limit" in args:
        limit = int(args[args.index("--limit") + 1])
    if "--detail" in args:
        detail = args[args.index("--detail") + 1]

    files = sorted(
        (os.path.join(dp, f) for dp, _, fs in os.walk(root) for f in fs if f.endswith(".xml")),
        key=os.path.getsize,
        reverse=True,
    )
    if limit:
        files = files[:limit]

    rows = []
    for path in files:
        try:
            rows.append(measure(path))
        except Exception as exc:
            print(f"跳过 {os.path.basename(path)}: {exc}")

    if not rows:
        print("没有可测量的文档")
        return

    if detail:
        for r in rows:
            if r["name"] == detail:
                print(f"== {r['name']} 明细 ==")
                for k, v in r.items():
                    print(f"  {k}: {v}")
                print()
                break

    tot = {k: sum(r[k] for r in rows) for k in rows[0] if k != "name"}
    print(f"样本 {len(rows)} 份\n")

    print("逐份（字节）")
    print(f"{'文档':<46}{'原始':>10}{'元素':>8}{'文字':>10}{'属性值':>10}{'base64':>10}{'JSON':>10}{'紧凑JSON':>10}")
    for r in rows:
        print(
            f"{r['name'][:44]:<46}{r['raw']:>10}{r['elems']:>8}"
            f"{r['text']:>10}{r['attr']:>10}{r['b64']:>10}"
            f"{r['json_full']:>10}{r['json_tight']:>10}"
        )

    print("\n合计（字节 / 占比）")
    def pct(part):
        return f"{100.0 * tot[part] / tot['raw']:5.1f}%"

    print(f"  XML 原始          {tot['raw']:>10}  100.0%")
    print(f"    其中 文字         {tot['text']:>10}  {pct('text')}")
    print(f"    其中 属性值       {tot['attr']:>10}  {pct('attr')}")
    print(f"    其中 base64 内嵌  {tot['b64']:>10}  {pct('b64')}")
    print(f"    格式开销(标签+缩进) {tot['raw'] - tot['text'] - tot['attr']:>10}"
          f"  {100.0 * (tot['raw'] - tot['text'] - tot['attr']) / tot['raw']:5.1f}%")
    print(f"  JSON 完整键名      {tot['json_full']:>10}  {100.0 * tot['json_full'] / tot['raw']:5.1f}%")
    print(f"  JSON 紧凑(符号表)  {tot['json_tight']:>10}  {100.0 * tot['json_tight'] / tot['raw']:5.1f}%")
    print(f"  gzip(XML)         {tot['gz_xml']:>10}  {100.0 * tot['gz_xml'] / tot['raw']:5.1f}%")
    print(f"  gzip(JSON 完整)    {tot['gz_json_full']:>10}  {100.0 * tot['gz_json_full'] / tot['raw']:5.1f}%")
    print(f"  gzip(JSON 紧凑)    {tot['gz_json_tight']:>10}  {100.0 * tot['gz_json_tight'] / tot['raw']:5.1f}%")

    print("\n开销拆解")
    print(f"  标签间空白(缩进+换行) {tot['blank']:>10}  {100.0 * tot['blank'] / tot['raw']:5.1f}%")
    print(f"  空元素(自闭合/空对)   {tot['empty_bytes']:>10}  {100.0 * tot['empty_bytes'] / tot['raw']:5.1f}%"
          f"  共 {tot['empty_count']} 个")
    print(f"  XML 去空白(minify)    {tot['min_xml']:>10}  {100.0 * tot['min_xml'] / tot['raw']:5.1f}%")
    print(f"  gzip(XML 去空白)      {tot['gz_min_xml']:>10}  {100.0 * tot['gz_min_xml'] / tot['raw']:5.1f}%")


if __name__ == "__main__":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    main()
