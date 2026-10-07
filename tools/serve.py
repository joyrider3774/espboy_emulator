"""Serves the web build (build_web/) on http://127.0.0.1:8000.

    python tools/serve.py [port] [folder]

Unlike a bare "python -m http.server", every file goes out with its proper
type (.wasm as application/wasm, which older Pythons do not know, so the
browser compiles it while it downloads) and with caching off, so a rebuild
is what the next reload gets.
"""
import functools
import http.server
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".json": "application/json",
        ".bin": "application/octet-stream",
    }

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    folder = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "build_web")
    handler = functools.partial(Handler, directory=folder)
    print("http://127.0.0.1:%d/ESPboy_Emulator.html  (serving %s)" % (port, folder))
    http.server.ThreadingHTTPServer(("127.0.0.1", port), handler).serve_forever()


if __name__ == "__main__":
    main()
