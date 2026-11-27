#!/usr/bin/env python3
"""The benchmark's page server: static files, with Range requests answered as
real web servers answer them.  A video that loops or is sought needs them:
WebKit 2.54 stops a looping <video> with a network error without them.

  rangeserve.py PORT DIR
"""
import http.server, os, re, sys


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def end_headers(self):
        self.send_header("Accept-Ranges", "bytes")
        super().end_headers()

    def send_head(self):
        rng = self.headers.get("Range")
        path = self.translate_path(self.path)
        m = re.match(r"bytes=(\d*)-(\d*)$", rng or "")
        if not m or not os.path.isfile(path) or not (m.group(1) or m.group(2)):
            return super().send_head()
        size = os.path.getsize(path)
        if m.group(1):
            start = int(m.group(1))
            end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
        else:                                  # bytes=-N: the last N
            start, end = max(0, size - int(m.group(2))), size - 1
        if start >= size or start > end:
            self.send_error(416)
            return None
        f = open(path, "rb")
        f.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        self.range_left = end - start + 1
        return f

    def copyfile(self, src, dst):
        left = getattr(self, "range_left", None)
        if left is None:
            return super().copyfile(src, dst)
        while left > 0:
            b = src.read(min(65536, left))
            if not b:
                break
            dst.write(b)
            left -= len(b)


if __name__ == "__main__":
    os.chdir(sys.argv[2])
    http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
