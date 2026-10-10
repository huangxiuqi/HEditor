"""横评文档编码格式：XML、JSON、自定义二进制，以及各自的压缩后体积。

样本取参照项目 DCWriterLite 的 wwwroot/demoDocuments。所有编码基于同一个
payload 抽取结果，字段统一为元素类型、属性键值、文字、子节点个数，因此比较的
只是编码本身的开销，不含信息量差异。每种编码再各压一遍 gzip 与 lzma，用来判断
压缩是否会抹平编码之间的差距。

自定义二进制按最紧凑的方式构造：类型与属性键走符号表，长度与符号索引用
LEB128 变长整数，字符串按 UTF-8 原样存放，不做对齐。这是自研格式能达到的
体积下限，实际实现带上长度前缀、区段表与校验码后只会更大。

用法：
    python probe_encoding_formats.py <目录> [--limit N]
"""

import gzip
import io
import json
import lzma
import os
import re
import sys
import xml.etree.ElementTree as ET

WS = re.compile(rb">[ \t\r\n]+<")


def strip_ns(tag):
    """去掉 ElementTree 拼在标签上的命名空间前缀。"""
    return tag.split("}", 1)[1] if "}" in tag else tag


def extract(el, out):
    """按深度优先把元素树摊平成 payload 记录，保留子节点个数以便重建树形。"""
    out.append(
        {
            "tag": strip_ns(el.tag),
            "attrs": dict(el.attrib),
            "text": (el.text or "").strip(),
            "kids": len(el),
        }
    )
    for child in el:
        extract(child, out)


def leb128(n):
    """无符号变长整数：每字节低 7 位是数据，最高位表示还有后续字节。"""
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def encode_json_full(flat):
    """每个元素一个对象，键名用完整单词，属性直接嵌在对象里。"""
    nodes = []
    for it in flat:
        node = {"tag": it["tag"], "kids": it["kids"]}
        if it["attrs"]:
            node["attrs"] = dict(it["attrs"])
        if it["text"]:
            node["text"] = it["text"]
        nodes.append(node)
    return json.dumps(nodes, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def encode_json_tight(flat):
    """类型与属性键走符号表，记录用位置数组，是 JSON 能达到的体积下限。"""
    tags, keys = {}, {}
    nodes = []
    for it in flat:
        rec = [tags.setdefault(it["tag"], len(tags)), it["kids"]]
        if it["attrs"]:
            pairs = []
            for k, v in it["attrs"].items():
                pairs.append(keys.setdefault(k, len(keys)))
                pairs.append(v)
            rec.append(pairs)
        if it["text"]:
            rec.append(it["text"])
        nodes.append(rec)
    payload = {"tags": list(tags), "keys": list(keys), "nodes": nodes}
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def encode_binary(flat):
    """符号表加 LEB128 的记录流，字符串按 UTF-8 前置长度。"""
    tags, keys = {}, {}
    body = bytearray()
    for it in flat:
        body += leb128(tags.setdefault(it["tag"], len(tags)))
        body += leb128(it["kids"])
        body += leb128(len(it["attrs"]))
        for k, v in it["attrs"].items():
            vb = v.encode("utf-8")
            body += leb128(keys.setdefault(k, len(keys)))
            body += leb128(len(vb))
            body += vb
        tb = it["text"].encode("utf-8")
        body += leb128(len(tb))
        body += tb

    head = bytearray()
    head += leb128(len(tags))
    for t in tags:
        tb = t.encode("utf-8")
        head += leb128(len(tb)) + tb
    head += leb128(len(keys))
    for k in keys:
        kb = k.encode("utf-8")
        head += leb128(len(kb)) + kb
    return bytes(head) + bytes(body)


def measure(path):
    raw = open(path, "rb").read()
    flat = []
    extract(ET.fromstring(raw), flat)

    xml_min = WS.sub(b"><", raw)
    encodings = {
        "xml": raw,
        "xml_min": xml_min,
        "json_full": encode_json_full(flat),
        "json_tight": encode_json_tight(flat),
        "binary": encode_binary(flat),
    }
    row = {"name": os.path.basename(path), "elems": len(flat)}
    for key, blob in encodings.items():
        row[key] = len(blob)
        row[key + "_gz"] = len(gzip.compress(blob, 9))
        row[key + "_xz"] = len(lzma.compress(blob, preset=6))
    return row


def main():
    args = sys.argv[1:]
    root = args[0] if args and not args[0].startswith("--") else "."
    limit = int(args[args.index("--limit") + 1]) if "--limit" in args else 0

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
            print(f"skip {os.path.basename(path)}: {exc}")
    if not rows:
        print("no document measured")
        return

    tot = {k: sum(r[k] for r in rows) for k in rows[0] if k != "name"}
    base = tot["xml"]
    print(f"样本 {len(rows)} 份，原始 XML 合计 {base} 字节\n")

    order = ["xml", "xml_min", "json_full", "json_tight", "binary"]
    label = {
        "xml": "XML 原始",
        "xml_min": "XML 去空白",
        "json_full": "JSON 完整键名",
        "json_tight": "JSON 紧凑(符号表)",
        "binary": "自定义二进制",
    }

    print(f"{'编码':<22}{'未压缩':>10}{'占比':>8}{'gzip':>10}{'占比':>8}{'lzma':>10}{'占比':>8}")
    for key in order:
        print(
            f"{label[key]:<22}{tot[key]:>10}{100.0 * tot[key] / base:>7.1f}%"
            f"{tot[key + '_gz']:>10}{100.0 * tot[key + '_gz'] / base:>7.1f}%"
            f"{tot[key + '_xz']:>10}{100.0 * tot[key + '_xz'] / base:>7.1f}%"
        )

    print("\n相对紧凑 JSON 的倍数")
    for key in order:
        print(
            f"  {label[key]:<22}{tot[key] / tot['json_tight']:>6.2f}x"
            f"{tot[key + '_gz'] / tot['json_tight_gz']:>8.2f}x"
            f"{tot[key + '_xz'] / tot['json_tight_xz']:>8.2f}x"
        )

    print(f"\n元素总数 {tot['elems']}，平均每元素 {base / tot['elems']:.1f} 字节(XML 原始)")


if __name__ == "__main__":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    main()
