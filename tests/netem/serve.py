import http.server, socketserver
BLOCK = bytes(65 + i % 26 for i in range(65536))
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        try: n = int(self.path.strip('/'))
        except ValueError: n = 1 << 20
        self.send_response(200)
        self.send_header('Content-Length', str(n)); self.send_header('Connection', 'close'); self.end_headers()
        while n > 0:
            k = min(n, 65536); self.wfile.write(BLOCK[:k]); n -= k
    def log_message(self, *a): print('served', self.path, flush=True)
socketserver.ThreadingTCPServer.allow_reuse_address = True
socketserver.ThreadingTCPServer(('10.9.0.1', 8099), H).serve_forever()
