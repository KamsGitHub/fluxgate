#!/usr/bin/env python3
"""A small backend for trying FluxGate: echoes each request back as JSON."""

import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class EchoHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _respond(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        payload = json.dumps({
            "method": self.command,
            "path": self.path,
            "headers": dict(self.headers.items()),
            "body_bytes": len(body),
        }, indent=2).encode() + b"\n"
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(payload)

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = _respond


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    args = parser.parse_args()
    ThreadingHTTPServer((args.host, args.port), EchoHandler).serve_forever()


if __name__ == "__main__":
    main()
