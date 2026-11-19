#!/usr/bin/env python3
"""Serves the browser-independent video test page (tests/vm/video.html) and
collects what it reports.

    videoserve.py <port> <results folder>

Binds 127.0.0.1 only: a QEMU VM with user networking reaches it as
http://10.0.2.2:<port>/video.html (and a browser on this machine as
http://127.0.0.1:<port>/video.html). Each report is saved as
<results>/video-<browser>-<mode>.json (the latest one wins; the page sends
one every 5 seconds and a final one at the end).
"""
import json, os, re, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
PORT, OUT = int(sys.argv[1]), sys.argv[2]
os.makedirs(OUT, exist_ok=True)
LOCK = threading.Lock()     # reports can arrive together (a resent one and the next)


def safe(s):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", str(s))[:40] or "x"


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.split("?")[0] in ("/", "/video.html"):
            body = open(os.path.join(HERE, "video.html"), "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        if self.path != "/report" or n > 4 * 1024 * 1024:
            self.send_error(400)
            return
        try:
            r = json.loads(self.rfile.read(n))
            path = os.path.join(OUT, f"video-{safe(r.get('browser'))}-{safe(r.get('mode'))}.json")
            with LOCK:
                try:
                    old = json.load(open(path))
                except Exception:
                    old = None
                # keep the newest (each report holds everything so far)
                if not old or old.get("final") is not True and (r.get("at") or 0) >= (old.get("at") or 0) or r.get("final"):
                    with open(path + ".tmp", "w") as f:
                        json.dump(r, f, indent=1)
                    os.replace(path + ".tmp", path)
            if r.get("final"):
                print(f"{r.get('browser')} {r.get('mode')}: {json.dumps(r.get('summary'))} errors={r.get('errors')}",
                      flush=True)
        except Exception as e:
            print("bad report:", e, flush=True)
        self.send_response(204)
        self.end_headers()


ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
