#!/usr/bin/env python3
"""Serve test media to a TV on the local network, with HTTP Range support.

The webOS media pipeline seeks with Range requests, which Python's built-in
http.server ignores, so seeking would restart from the beginning.

  media-server.py <dir> [--port 8099] [--bind 0.0.0.0]
"""
import argparse
import http.server
import os
import re
import shutil


class RangeHandler(http.server.SimpleHTTPRequestHandler):
    def send_head(self):
        path = self.translate_path(self.path)
        if os.path.isdir(path) or not os.path.isfile(path):
            return super().send_head()
        size = os.path.getsize(path)
        m = re.match(r"bytes=(\d*)-(\d*)$", self.headers.get("Range", "").strip())
        f = open(path, "rb")
        ctype = self.guess_type(path)
        if not m or (m.group(1) == "" and m.group(2) == ""):
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(size))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            self._range = None
            return f
        if m.group(1) == "":  # suffix range: last N bytes
            start, end = max(0, size - int(m.group(2))), size - 1
        else:
            start = int(m.group(1))
            end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
        if start >= size or start > end:
            f.close()
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.end_headers()
            return None
        self.send_response(206)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        f.seek(start)
        self._range = end - start + 1
        return f

    def copyfile(self, source, outputfile):
        n = getattr(self, "_range", None)
        if n is None:
            return shutil.copyfileobj(source, outputfile)
        while n > 0:
            buf = source.read(min(n, 1 << 16))
            if not buf:
                break
            try:
                outputfile.write(buf)
            except (BrokenPipeError, ConnectionResetError):
                break  # players drop connections when they seek
            n -= len(buf)

    def log_message(self, fmt, *args):
        rng = self.headers.get("Range") if hasattr(self, "headers") else None
        print(f"{self.client_address[0]} {fmt % args}" + (f" [{rng}]" if rng else ""), flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--bind", default="0.0.0.0")
    a = ap.parse_args()
    handler = lambda *x, **k: RangeHandler(*x, directory=a.dir, **k)  # noqa: E731
    with http.server.ThreadingHTTPServer((a.bind, a.port), handler) as srv:
        print(f"serving {a.dir} on {a.bind}:{a.port}", flush=True)
        srv.serve_forever()


if __name__ == "__main__":
    main()
