# -*- coding: utf-8 -*-
"""验证台用的开发静态服务：每个响应都发 no-store，并补齐 wasm/ttf 的 MIME。

为什么不用 python -m http.server：
  它只发 Last-Modified、不发 Cache-Control，浏览器于是按"启发式新鲜度"（通常是距
  Last-Modified 的 10%）直接用磁盘缓存，连问都不问服务器。于是重新编译了 wasm、
  磁盘上的文件明明换了，页面照旧跑旧的那一份 —— 表现是"改了没生效"，非常费时间。
  这里对每个响应加 no-store：开发期每次刷新都拿真文件。生产部署不该用这个脚本。

用法：
  python scripts/dev_server.py --port 8781 --root .
"""

import argparse
import functools
import http.server
import mimetypes
import os
import socketserver

# 浏览器对 wasm / 字体的 MIME 有要求：类型不对，WASM 的流式实例化会直接拒绝。
mimetypes.add_type('application/wasm', '.wasm')
mimetypes.add_type('font/ttf', '.ttf')
mimetypes.add_type('text/javascript', '.js')


class NoStoreHandler(http.server.SimpleHTTPRequestHandler):
    """在标准静态处理器上加一行缓存策略。"""

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate, max-age=0')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        super().end_headers()


class Server(socketserver.ThreadingTCPServer):
    """多线程静态服务。

    backlog 必须放大：默认的 request_queue_size 只有 5，而四个 worker 同时起首屏时
    一次要开十几个连接（页面、库、wasm、worker 壳），超出的连接会被直接丢掉 ——
    表现是某个页面只剩一条横幅、脚本压根没跑起来。
    """

    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 128


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=8781)
    ap.add_argument('--root', default=os.getcwd())
    args = ap.parse_args()

    handler = functools.partial(NoStoreHandler, directory=os.path.abspath(args.root))
    with Server(('127.0.0.1', args.port), handler) as httpd:
        print('服务根目录：%s' % os.path.abspath(args.root), flush=True)
        print('验证台地址：http://localhost:%d/web/index.html' % args.port, flush=True)
        print('按 Ctrl+C 停止', flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print('已停止', flush=True)


if __name__ == '__main__':
    main()
