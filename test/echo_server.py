#!/usr/bin/env python3
"""Minimal HTTP/1.1 echo server for cofetch tests.

Endpoints:
  ANY /status/<code>  -> responds with that HTTP status
  ANY /delay/<secs>   -> sleeps up to 10s, then echoes
  ANY /redirect/<n>   -> 302-hops n times, landing on /get
  ANY <path>          -> 200 with JSON echo of method/url/headers/body
"""
import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # keep-alive

    def _read_body(self):
        n = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(n).decode("utf-8", "replace") if n else ""

    def _respond(self):
        body = self._read_body()
        if self.path.startswith("/redirect/"):
            n = int(self.path.rsplit("/", 1)[1])
            target = "/get" if n <= 1 else "/redirect/%d" % (n - 1)
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.startswith("/delay/"):
            time.sleep(min(float(self.path.rsplit("/", 1)[1]), 10.0))
        if self.path.startswith("/status/"):
            status = int(self.path.rsplit("/", 1)[1])
            payload = b"{}"
        else:
            status = 200
            payload = json.dumps({
                "method": self.command,
                "url": self.path,
                "headers": dict(self.headers),
                "data": body,
            }).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    do_GET = _respond
    do_POST = _respond
    do_PUT = _respond
    do_DELETE = _respond

    def log_message(self, *args):
        pass


class Server(ThreadingHTTPServer):
    request_queue_size = 128  # default 5 rejects concurrent bursts
    daemon_threads = True


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18089)
    args = parser.parse_args()
    Server(("127.0.0.1", args.port), Handler).serve_forever()
