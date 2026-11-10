#!/usr/bin/env python3
"""The real libtatsu (tss_request_send) against mock TSS servers, through host/curl_mbedtls.c. usage: test_tatsu.py TOOL WORKDIR"""
import http.server, os, socketserver, ssl, subprocess, sys, threading, time

tool, work = sys.argv[1], sys.argv[2]
ship = sys.argv[3] if len(sys.argv) > 3 else None   # the build the app ships: https only, no test option
os.makedirs(work, exist_ok=True)
fails = []
seen = []   # (headers dict lower-case, body)

def check(name, cond, info=""):
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  " + str(info)))
    if not cond: fails.append(name)

PLIST = b'<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0"><dict><key>ApImg4Ticket</key><data>AQIDBA==</data><key>Other</key><string>x</string></dict></plist>'

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        seen.append(({k.lower(): v for k, v in self.headers.items()}, body, self.path))
        if self.path.startswith("/ok"):
            out = b"STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING=" + PLIST
        elif self.path.startswith("/chunked"):
            out = None
        elif self.path.startswith("/nostatus"):
            out = b"garbage without a status"
        elif self.path.startswith("/rejected"):
            out = b"STATUS=8&MESSAGE=This device isn't eligible for the requested build."
        else:
            out = b"STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING=" + PLIST
        self.send_response(200)
        self.send_header("Connection", "close")
        if out is None:
            body_out = b"STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING=" + PLIST
            self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            for i in range(0, len(body_out), 37):
                part = body_out[i:i + 37]
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(out))); self.end_headers(); self.wfile.write(out)
        self.close_connection = True

class TS(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

def start(tls=None):
    s = TS(("127.0.0.1", 0), H)
    if tls:
        c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c.load_cert_chain(tls[1], tls[0]); s.socket = c.wrap_socket(s.socket, server_side=True)
    threading.Thread(target=s.serve_forever, daemon=True).start()
    return s.server_address[1]

def run(url, env=None, timeout=90, exe=None):
    e = dict(os.environ); e.update(env or {})
    t0 = time.time()
    r = subprocess.run([exe or tool, url], capture_output=True, env=e, timeout=timeout)
    return r.stdout.decode(), r.stderr.decode(), time.time() - t0

P = start()
out, err, _ = run(f"http://127.0.0.1:{P}/ok")
check("tss_request_send gets a plist back from a SUCCESS reply", "RESULT: plist" in out and "ApImg4Ticket" in out, (out, err))
h, body, path = seen[-1]
check("request is a POST of the plist XML with the right Content-Length", body.startswith(b"<?xml") and b"ApECID" in body and int(h["content-length"]) == len(body), (h, body[:80]))
check("libtatsu's headers arrive (Content-type, Cache-Control, User-Agent) and 'Expect:' does not", h.get("content-type", "").startswith("text/xml") and h.get("cache-control") == "no-cache" and h.get("user-agent") == "InetURL/1.0" and "expect" not in h, h)
out, err, _ = run(f"http://127.0.0.1:{P}/chunked")
check("a chunked reply is reassembled for libtatsu", "RESULT: plist" in out and "ApImg4Ticket" in out, (out, err))
out, err, dt = run(f"http://127.0.0.1:{P}/rejected")
check("a TSS error reply (STATUS=8) gives NULL; the server's text does not reach the log (it can echo identifiers)", "RESULT: null" in out and "isn't eligible" not in err and "TSS server returned" not in err, (out, err))

# TLS: a server whose certificate is not trusted. libtatsu retries 15 times with 2 s sleeps; the message from our client must reach its log.
def mkcert(name, cn):
    key, crt, cfg = f"{work}/{name}.key", f"{work}/{name}.crt", f"{work}/{name}.cnf"
    open(cfg, "w").write(f"[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nCN={cn}\n[v3]\nsubjectAltName=DNS:{cn}\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
    subprocess.check_call(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt, "-days", "2", "-config", cfg], stderr=subprocess.DEVNULL)
    return key, crt
c = mkcert("tssok", "localhost")
cad = f"{work}/tsscadir"; os.makedirs(cad, exist_ok=True)
import shutil; shutil.copy(c[1], cad + "/c.pem")
PT = start(tls=c)
out, err, _ = run(f"https://localhost:{PT}/ok", {"HV_CA_PATH": cad})
check("https TSS reply over a trusted certificate", "RESULT: plist" in out, (out, err))
os.makedirs(f"{work}/tssemptyca", exist_ok=True)
shutil.copy(mkcert("tssother", "other.example")[1], f"{work}/tssemptyca/o.pem")
out, err, dt = run(f"https://localhost:{PT}/ok", {"HV_CA_PATH": f"{work}/tssemptyca"})
check("untrusted certificate: libtatsu stops at once (no retry, no other host); its log names only the curl code, no error text",
      "RESULT: null" in out and "not retrying (curl" in err and dt < 8, (out, err[-400:], dt))
# host name mismatch: the certificate is trusted but is for another name
os.makedirs(f"{work}/tssmism", exist_ok=True)
cm = mkcert("tssmismatch", "other.example"); shutil.copy(cm[1], f"{work}/tssmism/m.pem")
PM = start(tls=cm); n0 = len(seen)
out, err, dt = run(f"https://localhost:{PM}/ok", {"HV_CA_PATH": f"{work}/tssmism"})
check("trusted certificate for another host name: refused, not retried, the request never reaches the handler",
      "RESULT: null" in out and "not retrying" in err and dt < 8 and len(seen) == n0, (out, err[-300:], dt, len(seen) - n0))
# retry limit: replies without STATUS are retried, six attempts at most (2 s apart)
n0 = len(seen)
out, err, dt = run(f"http://127.0.0.1:{P}/nostatus")
check("a reply without STATUS: exactly 6 attempts, then NULL", "RESULT: null" in out and len(seen) - n0 == 6, (out, len(seen) - n0, err[-200:]))
# connection refused: retryable, bounded
s0 = socketserver.TCPServer.allow_reuse_address
import socket
sk = socket.socket(); sk.bind(("127.0.0.1", 0)); dead = sk.getsockname()[1]; sk.close()
out, err, dt = run(f"http://127.0.0.1:{dead}/ok")
check("connection refused: retried (6 attempts, ~2 s apart), then NULL within the bound", "RESULT: null" in out and 8 <= dt < 40, (out, dt))
# https only: the build the app ships
if ship:
    n0 = len(seen)
    out, err, dt = run(f"http://127.0.0.1:{P}/ok", exe=ship)
    check("shipped build: an http:// URL is refused, nothing is sent", "RESULT: null" in out and "not https" in err and len(seen) == n0 and dt < 3, (out, err[-200:], len(seen) - n0))
    out, err, dt = run("http://gs.apple.com/TSS/controller?action=2", exe=ship)
    check("shipped build: http:// to the Apple host is refused", "RESULT: null" in out and "not https" in err and dt < 3, (out, err[-200:]))
    out, err, dt = run(f"https://localhost:{PT}/ok", {"HV_CA_PATH": cad}, exe=ship)
    check("shipped build: https to a trusted mock still works (the same code path as the test build)", "RESULT: plist" in out, (out, err[-200:]))
print("FAILED:", fails if fails else "none")
sys.exit(1 if fails else 0)
