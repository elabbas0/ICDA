import os, socket, socketserver

SIZE = 65536
class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.request.settimeout(20)
        self.request.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        try:
            for turn in range(16):
                data = self.rfile.read(SIZE)
                expected = bytes((i + turn * 17) % 251 for i in range(SIZE))
                if data != expected:
                    mismatch = next((i for i,(a,b) in enumerate(zip(data,expected)) if a != b), len(data))
                    print(f"FAIL round={turn} received={len(data)} mismatch={mismatch}", flush=True)
                    self.wfile.write(b"FAIL\n"); self.wfile.flush(); return
                self.wfile.write(b"OK\n"); self.wfile.flush()
            print("PASS verified 1048576 upload bytes over persistent TCP", flush=True)
        except (OSError, TimeoutError) as e:
            print(f"FAIL {type(e).__name__}: {e}", flush=True)
class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
print("ICDA transport fixture listening on port 19080", flush=True)
with Server((os.environ.get("ICDA_TEST_BIND", "127.0.0.1"),19080), Handler) as server:
    server.serve_forever()
