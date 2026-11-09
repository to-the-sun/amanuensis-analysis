#!/usr/bin/env python3
"""Serve an audio-pattern report directory with byte-range support.

Usage:
    python3 pattern_report_server.py --root /path/to/report_dir --port 8000

The audio element needs HTTP Range support for seeking. SimpleHTTPRequestHandler
does not provide it, so this server adds a minimal 206 implementation.
"""
import argparse
import functools
import http.server
import os
import re
import socketserver


class RangeHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        rng = self.headers.get('Range')
        if not rng:
            return super().do_GET()
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            return super().do_GET()
        m = re.match(r'bytes=(\d*)-(\d*)', rng)
        if not m:
            return super().do_GET()
        size = os.path.getsize(path)
        start = int(m.group(1) or 0)
        end = int(m.group(2)) if m.group(2) else size - 1
        if start >= size:
            self.send_error(416)
            return
        end = min(end, size - 1)
        length = end - start + 1
        self.send_response(206)
        self.send_header('Content-Type', self.guess_type(path))
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        self.send_header('Content-Length', str(length))
        self.end_headers()
        with open(path, 'rb') as f:
            f.seek(start)
            self.wfile.write(f.read(length))

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser(description='Range-capable static server for pattern reports.')
    ap.add_argument('--root', required=True, help='Report directory to serve')
    ap.add_argument('--port', type=int, default=8000)
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        raise SystemExit('Not a directory: %s' % root)
    handler = functools.partial(RangeHandler, directory=root)
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(('0.0.0.0', args.port), handler) as httpd:
        print('Serving %s on http://0.0.0.0:%d' % (root, args.port), flush=True)
        httpd.serve_forever()


if __name__ == '__main__':
    main()
