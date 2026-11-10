#!/usr/bin/env python3
"""Tests host/curl_mbedtls.c against local HTTP and HTTPS servers (no network needed), plus an optional public-network check.
usage: test_curl.py CURL_TOOL WORKDIR"""
import hashlib, http.server, os, socketserver, ssl, subprocess, sys, threading, shutil

tool, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
BIG = os.urandom(3 * 1024 * 1024 + 17)
BLK = os.urandom(1024 * 1024 + 3)
fails = []

def check(name, cond, info=""):
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  " + str(info)))
    if not cond: fails.append(name)

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send(self, code, body, extra=(), length=True):
        self.send_response(code)
        for k, v in extra: self.send_header(k, v)
        if length: self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True
    def do_GET(self):
        p = self.path
        if p == "/hello": self.send(200, b"hello world", [("X-Test", "1")])
        elif p == "/hdrs": self.send(200, ",".join(sorted(k.lower() for k in self.headers.keys())).encode())
        elif p == "/ua": self.send(200, (self.headers.get("X-Henge-Test", "") + "|" + self.headers.get("Host", "")).encode())
        elif p == "/big": self.send(200, BIG)
        elif p == "/nolen":
            self.send_response(200); self.send_header("Connection", "close"); self.end_headers()
            self.wfile.write(b"close-delimited-body"); self.close_connection = True
        elif p == "/chunked":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.send_header("Connection", "close"); self.end_headers()
            for part in (b"abc", b"defgh", b"0123456789" * 100):
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\nX-Trailer: t\r\n\r\n"); self.close_connection = True
        elif p == "/r1": self.send(302, b"", [("Location", "/hello")])
        elif p == "/r2": self.send(301, b"", [("Location", "r3")] if False else [("Location", "/r1")])
        elif p == "/rel/a": self.send(302, b"", [("Location", "b")])
        elif p == "/rel/b": self.send(200, b"rel-b")
        elif p == "/loop": self.send(302, b"", [("Location", "/loop")])
        elif p == "/to-ip": self.send(302, b"", [("Location", "https://127.0.0.1:%d/hello" % self.server.server_address[1])])
        elif p == "/to-http": self.send(302, b"", [("Location", "http://127.0.0.1:%d/hello" % HTTP_PORT)])
        elif p == "/404": self.send(404, b"nope")
        elif p == "/short":  # promises more than it sends
            self.send_response(200); self.send_header("Content-Length", "100"); self.send_header("Connection", "close"); self.end_headers()
            self.wfile.write(b"only-ten-by"); self.close_connection = True
        elif p.startswith("/gen"):  # generated on the fly, never held in memory: /gen?n=N (sized), /genc?n=N (chunked), /gencut?n=N&cut=M (promises N, sends M, closes)
            q = dict(kv.split("=") for kv in p.split("?", 1)[1].split("&")); n = int(q["n"]); cut = int(q.get("cut", n))
            chunked = p.startswith("/genc?")
            self.send_response(200); self.send_header("Connection", "close")
            if chunked: self.send_header("Transfer-Encoding", "chunked")
            else: self.send_header("Content-Length", str(n))
            self.end_headers()
            sent = 0
            try:
                while sent < cut:
                    piece = BLK[: min(len(BLK), cut - sent)]
                    self.wfile.write((b"%x\r\n" % len(piece) + piece + b"\r\n") if chunked else piece)
                    sent += len(piece)
                if chunked: self.wfile.write(b"0\r\n\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass  # the client stopped on purpose
            self.close_connection = True
        elif p == "/to-stall": self.send(302, b"", [("Location", "http://127.0.0.1:%d/x" % STALL_PORT)])
        elif p == "/garbage":
            self.wfile.write(b"SSH-2.0-not-http\r\n\r\n"); self.close_connection = True
        else: self.send(404, b"")
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        if self.path == "/echo": self.send(200, b"%d:" % n + body + b"|" + self.headers.get("Content-Type", "").encode())
        elif self.path == "/post-redir": self.send(302, b"", [("Location", "/method")])
        else: self.send(404, b"")

class TS(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

# the /method route reports GET vs POST after a redirect
orig_get = H.do_GET
def do_get(self):
    if self.path == "/method": return self.send(200, b"GET-after-redirect")
    return orig_get(self)
H.do_GET = do_get

def mkcert(name, cn_san, extra_san=''):
    key, crt = f"{work}/{name}.key", f"{work}/{name}.crt"
    cfg = f"{work}/{name}.cnf"
    open(cfg, "w").write(f"[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nCN={cn_san}\n[v3]\nsubjectAltName=DNS:{cn_san}{extra_san}\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
    subprocess.check_call(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt, "-days", "2", "-config", cfg], stderr=subprocess.DEVNULL)
    return key, crt

def serve(port_holder, tls=None):
    srv = TS(("127.0.0.1", 0), H)
    if tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(tls[1], tls[0])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]

def run(args, env=None, timeout=60):
    e = dict(os.environ); e.update(env or {})
    r = subprocess.run([tool] + args, capture_output=True, env=e, timeout=timeout)
    err = r.stderr.decode(errors="replace")
    d = {}
    for ln in err.splitlines():
        if ln.startswith("RC="):
            for part in ln.split(" ", 3):
                k, _, v = part.partition("="); d[k] = v
            d["ERR"] = ln.split("ERR=", 1)[1] if "ERR=" in ln else ""
    d["out"] = r.stdout; d["stderr"] = err; d["code"] = r.returncode
    return d

def raw_server(mode):
    """mode 'stall': read the request, then say nothing; 'headers': send a complete header block promising a big body, then nothing; 'drip': header + 1 byte every 0.2 s forever; 'silent': accept and never read or write"""
    import socket
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ls.bind(("127.0.0.1", 0)); ls.listen(50)
    def serve_one(conn):
        try:
            if mode != "silent": conn.recv(65536)
            if mode in ("headers", "drip"): conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 100000000\r\nConnection: close\r\n\r\n")
            while True:
                if mode == "drip": conn.sendall(b"x"); time.sleep(0.2)
                else: time.sleep(0.5)
        except Exception: pass
        finally: conn.close()
    def loop():
        while True:
            c, _ = ls.accept(); threading.Thread(target=serve_one, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()
    return ls.getsockname()[1]

import time
STALL_PORT = raw_server("stall"); HEADERS_PORT = raw_server("headers"); DRIP_PORT = raw_server("drip"); SILENT_PORT = raw_server("silent")

def run_big(args, env=None, timeout=300):
    """runs the tool with stdout to a file; returns the dict of run() fields (out omitted) plus sha256/size of what it wrote and the child's peak RSS in KiB"""
    e = dict(os.environ); e.update(env or {})
    outp = f"{work}/big.out"
    with open(outp, "wb") as fo, open(f"{work}/big.err", "wb") as fe:
        pr = subprocess.Popen([tool] + args, stdout=fo, stderr=fe, env=e)
        _, status, ru = os.wait4(pr.pid, 0)
    err = open(f"{work}/big.err", errors="replace").read()
    d = {"stderr": err, "maxrss_kib": ru.ru_maxrss}
    for ln in err.splitlines():
        if ln.startswith("RC="):
            for part in ln.split(" ", 3):
                k, _, v = part.partition("="); d[k] = v
    h = hashlib.sha256(); n = 0
    with open(outp, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b: break
            h.update(b); n += len(b)
    d["sha"], d["size"] = h.hexdigest(), n
    return d

def expect_sha(n):
    h = hashlib.sha256(); left = n
    while left:
        piece = BLK[: min(len(BLK), left)]; h.update(piece); left -= len(piece)
    return h.hexdigest()

srv_http, HTTP_PORT = serve(None)
B = f"http://127.0.0.1:{HTTP_PORT}"
r = run([B + "/hello", "--headers"])
check("http get", r["RC"] == "0" and r["HTTP"] == "200" and r["out"] == b"hello world", r)
check("header callback format", "H: HTTP/1.1 200\r\n" in r["stderr"] and "H: X-Test: 1" in r["stderr"] and r["stderr"].rstrip().splitlines()[-2:-1] != None, r["stderr"])
r = run([B + "/ua"]); check("custom header + Host", r["out"] == b"yes|127.0.0.1:%d" % HTTP_PORT, r["out"])
r = run([B + "/hdrs"]); check("'Expect:' (empty value) removes the header instead of sending it", b"expect" not in r["out"] and b"x-henge-test" in r["out"], r["out"])
r = run([B + "/big"]); check("3 MB body streamed intact", hashlib.sha256(r["out"]).digest() == hashlib.sha256(BIG).digest(), len(r["out"]))
r = run([B + "/nolen"]); check("close-delimited body", r["RC"] == "0" and r["out"] == b"close-delimited-body", r)
r = run([B + "/chunked"]); check("chunked body", r["RC"] == "0" and r["out"] == b"abc" + b"defgh" + b"0123456789" * 100, r["out"][:60])
r = run([B + "/echo", "--post", "a=1&b=two"]); check("post body + length", r["out"] == b"9:a=1&b=two|application/x-www-form-urlencoded", r["out"])
r = run([B + "/404"]); check("404 is a transfer success with status 404", r["RC"] == "0" and r["HTTP"] == "404" and r["out"] == b"nope", r)
r = run([B + "/r1"]); check("redirect not followed without FOLLOWLOCATION", r["HTTP"] == "302" and r["out"] == b"", r)
r = run([B + "/r2", "--follow"]); check("redirect chain followed", r["HTTP"] == "200" and r["out"] == b"hello world", r)
r = run([B + "/rel/a", "--follow"]); check("relative redirect", r["out"] == b"rel-b", r)
for h in ("gs.apple.com", "GS.Apple.com.akadns.net", "gs.apple.com.v.aaplimg.com"):
    r = run([f"http://{h}/TSS/controller?action=2", "--post", "x=1"])
    check(f"plain http to the TSS host {h} is refused before any lookup", r["RC"] == "1" and "refusing plain http to Apple's TSS server" in r["stderr"], r)
r = run([B + "/loop", "--follow"]); check("redirect loop stops", r["RC"] == "47", r)
r = run([B + "/post-redir", "--post", "x=1", "--follow"]); check("POST becomes GET after 302", r["out"] == b"GET-after-redirect", r["out"])
r = run([B + "/short"]); check("truncated sized body is an error", r["RC"] == "56", r)
r = run([B + "/garbage"]); check("non-HTTP reply is an error", r["RC"] != "0", r)
r = run([B + "/big", "--abort-after", "100000"]); check("write callback stop -> CURLE_WRITE_ERROR", r["RC"] == "23", r)
r = run(["ftp://127.0.0.1/x"]); check("unsupported scheme", r["RC"] == "1", r)
r = run(["http://127.0.0.1:1/x"]); check("connection refused", r["RC"] == "7", r)
r = run(["http://nonexistent.invalid/"]); check("DNS failure", r["RC"] == "6", r)

# --- whole-transfer deadline, external cancel, progress-callback stop (all against servers that go quiet or never finish) ---
def num(d, k): return float(d.get(k, "nan"))
def tm(r):
    d = {}
    for ln in r["stderr"].splitlines():
        if ln.startswith("ELAPSED_MS="):
            for kv in ln.split(): k, _, v = kv.partition("="); d[k] = int(float(v))
    return d
ST = f"http://127.0.0.1:{STALL_PORT}"
r = run([ST + "/x", "--timeout-ms", "1200"]); t = tm(r)
check("TIMEOUT_MS: a server that reads the request and goes silent -> CURLE_OPERATION_TIMEDOUT near the limit (not after the 60 s idle limit)", r["RC"] == "28" and 1100 <= t["ELAPSED_MS"] <= 2500, (r["RC"], t, r["ERR"]))
check("  the error text says what happened", "timed out after 1200 milliseconds" in r["ERR"], r["ERR"])
check("  the socket is closed again (no descriptor left behind)", t["FDS_AFTER"] == t["FDS_BEFORE"], t)
r = run([f"http://127.0.0.1:{HEADERS_PORT}/x", "--timeout-ms", "1000"]); t = tm(r)
check("TIMEOUT_MS: headers arrive, the body never does", r["RC"] == "28" and 900 <= t["ELAPSED_MS"] <= 2500 and r["HTTP"] == "200", (r["RC"], t))
r = run([f"http://127.0.0.1:{DRIP_PORT}/x", "--timeout-ms", "1500"]); t = tm(r)
check("TIMEOUT_MS: a body that trickles in (idle limit never reached) still ends at the total limit", r["RC"] == "28" and 1400 <= t["ELAPSED_MS"] <= 2800 and int(r["BYTES"]) > 0, (r["RC"], t, r["BYTES"]))
r = run([f"http://127.0.0.1:{STALL_PORT}/x", "--timeout-ms", "3000", "--cancel-after-ms", "600"]); t = tm(r)
check("hv_curl_cancel from another thread -> CURLE_ABORTED_BY_CALLBACK long before the deadline", r["RC"] == "42" and t["ELAPSED_MS"] < 1500 and t["FDS_AFTER"] == t["FDS_BEFORE"], (r["RC"], t))
check("  the error text says it was a cancel", "cancel" in r["ERR"].lower(), r["ERR"])
r = run([f"http://127.0.0.1:{DRIP_PORT}/x", "--global-cancel-after-ms", "700"]); t = tm(r)
check("process-wide cancel flag stops a trickling transfer", r["RC"] == "42" and t["ELAPSED_MS"] < 1800, (r["RC"], t))
r = run([ST + "/x", "--abort-after-ms", "1500"]); t = tm(r)
check("a progress callback that says stop is honoured while the peer is silent (called about once a second, as libcurl does)", r["RC"] == "42" and 1400 <= t["ELAPSED_MS"] <= 3200, (r["RC"], t))
r = run([B + "/to-stall", "--follow", "--timeout-ms", "1500"]); t = tm(r)
check("the deadline covers the whole redirect chain", r["RC"] == "28" and 1400 <= t["ELAPSED_MS"] <= 2800, (r["RC"], t))
r = run([ST + "/x", "--timeout-ms", "800", "--also", B + "/hello"]); q = reqs(r) if False else r
check("the same handle works again after a timeout (state reset)", "REQ 2 RC=0 HTTP=200" in r["stderr"], r["stderr"][-300:])
r = run([ST + "/x", "--cancel-after-ms", "400", "--also", B + "/hello"])
check("the same handle works again after a cancel + hv_curl_cancel_reset", "REQ 2 RC=0 HTTP=200" in r["stderr"], r["stderr"][-300:])
r = run([B + "/hello", "--timeout-ms", "5000"]); t = tm(r)
check("a timeout that is not reached changes nothing", r["RC"] == "0" and r["out"] == b"hello world" and t["ELAPSED_MS"] < 1500, (r["RC"], t))
r = run([f"http://127.0.0.1:{SILENT_PORT}/x", "--timeout-ms", "1000"]); t = tm(r)
check("accepts the connection but never reads: the request is small, so it goes out, and waiting for the reply ends at the limit", r["RC"] == "28", (r["RC"], t))

# --- big downloads: streamed, exact, bounded memory; cut off midway ---
N = 64 * 1024 * 1024
small = run_big([B + "/gen?n=1048576", "--discard"])   # same fork from the same parent: its peak RSS is the baseline (a child's ru_maxrss includes the pre-exec copy of the test process)
r = run_big([B + f"/gen?n={N}", "--discard"])
check("64 MB sized body: every byte counted", r["RC"] == "0" and int(r["BYTES"]) == N, (r.get("RC"), r.get("BYTES")))
check("  peak memory grows by < 8 MiB over a 1 MiB download (the body is streamed, not buffered)", r["maxrss_kib"] - small["maxrss_kib"] < 8 * 1024, (small["maxrss_kib"], r["maxrss_kib"]))
r = run_big([B + f"/gen?n={N}"])
check("64 MB sized body written out: size and SHA-256 identical", r["size"] == N and r["sha"] == expect_sha(N), (r["size"], r["sha"][:12]))
r = run_big([B + f"/genc?n={N}"])
check("64 MB chunked body: size and SHA-256 identical", r["RC"] == "0" and r["size"] == N and r["sha"] == expect_sha(N), (r.get("RC"), r["size"]))
r = run_big([B + f"/gencut?n={N}&cut={10 * 1024 * 1024 + 5}", "--discard"])
check("connection closed after 10 MB of a promised 64 MB -> receive error, with the 10 MB counted", r["RC"] == "56" and int(r["BYTES"]) == 10 * 1024 * 1024 + 5, (r.get("RC"), r.get("BYTES")))
r = run_big([B + f"/gen?n={N}", "--discard", "--abort-after", str(20 * 1024 * 1024)])
check("the write callback refusing data stops a 64 MB download early", r["RC"] == "23" and int(r["BYTES"]) < N, (r.get("RC"), r.get("BYTES")))
r = run_big([B + f"/gen?n={N}", "--discard", "--abort-after-ms", "1"])
check("a progress callback that says stop ends a fast download", r["RC"] == "42" and int(r["BYTES"]) < N, (r.get("RC"), r.get("BYTES")))

# --- TLS, offline ---
good = mkcert("good", "localhost")
other = mkcert("other", "wrong.example")
cadir = f"{work}/cadir"; os.makedirs(cadir, exist_ok=True); shutil.copy(good[1], cadir + "/good.pem")
srv_s, S_PORT = serve(None, tls=good)
srv_w, W_PORT = serve(None, tls=other)
S = f"https://localhost:{S_PORT}"
r = run([S + "/hello"], {"HV_CA_PATH": f"{work}/emptyca"}); check("https refused when no CA can be loaded (no fallback to unchecked)", r["RC"] == "77", r)
os.makedirs(f"{work}/otherca", exist_ok=True); shutil.copy(other[1], f"{work}/otherca/o.pem")
r = run([S + "/hello"], {"HV_CA_PATH": f"{work}/otherca"}); check("https refuses an untrusted certificate (VERIFYPEER=0 is ignored)", r["RC"] == "60", r)
check("untrusted certificate: diagnostics carry flags and the server's certificate", "VERIFY_FLAGS=0x" in r["stderr"] and "CERT depth 0" in r["stderr"] and "subject name" in r["stderr"] and "flags=0x" in r["ERR"], r["stderr"][:400])
r = run([S + "/hello"], {"HV_CA_PATH": cadir}); check("https works with a trusted CA", r["RC"] == "0" and r["out"] == b"hello world", r)
r = run([S + "/big"], {"HV_CA_PATH": cadir}); check("https 3 MB body intact", hashlib.sha256(r["out"]).digest() == hashlib.sha256(BIG).digest(), len(r["out"]))
r = run([S + "/echo", "--post", "k=v"], {"HV_CA_PATH": cadir}); check("https post", r["out"].startswith(b"3:k=v"), r["out"])
r = run([f"https://127.0.0.1:{S_PORT}/hello"], {"HV_CA_PATH": cadir}); check("https refuses a host name the certificate does not cover (IP vs DNS SAN)", r["RC"] == "60", r)
r = run([f"https://localhost:{W_PORT}/hello"], {"HV_CA_PATH": f"{work}/otherca"}); check("https refuses wrong host name even though the CA is trusted", r["RC"] == "60", r)
r = run([S + "/to-http", "--follow"], {"HV_CA_PATH": cadir}); check("https->http redirect refused", r["RC"] == "1", r)
r = run([f"https://localhost:{SILENT_PORT}/x", "--timeout-ms", "1200"], {"HV_CA_PATH": cadir}); t = tm(r)
check("the deadline also covers a TLS handshake that never completes", r["RC"] == "28" and 1100 <= t["ELAPSED_MS"] <= 2800 and t["FDS_AFTER"] == t["FDS_BEFORE"], (r["RC"], t))
r = run([f"https://localhost:{SILENT_PORT}/x", "--cancel-after-ms", "500"], {"HV_CA_PATH": cadir}); t = tm(r)
check("hv_curl_cancel interrupts a TLS handshake", r["RC"] == "42" and t["ELAPSED_MS"] < 1500 and t["FDS_AFTER"] == t["FDS_BEFORE"], (r["RC"], t))
r = run([S + "/big", "--discard", "--abort-after-ms", "1"], {"HV_CA_PATH": cadir})
check("https: a progress callback that says stop ends a transfer in progress", r["RC"] == "42", r)

# --- per-request state on one handle: failure -> success, success -> failure, redirect then failure ---
def reqs(r):
    d = {}
    for ln in r["stderr"].splitlines():
        if ln.startswith("REQ ") and " RC=" in ln:
            parts = ln.split(" ", 2)[1:]
            d[int(parts[0])] = dict(kv.split("=", 1) for kv in parts[1].split(" ", 4)[:4] if "=" in kv)
        elif ln.startswith("RC="):
            d[1] = dict(kv.split("=", 1) for kv in ln.split(" ", 3)[:3])
            d[1]["TLS_VERIFIED"] = "1" if "TLS_VERIFIED=1" in r["stderr"].split("RC=")[0] else "0"
            d[1]["DIAG_BYTES"] = r["stderr"].split("REQ 1 DIAG_BYTES=")[1].split()[0]
    return d
WH = f"https://localhost:{W_PORT}"   # certificate from 'other': untrusted when only cadir is trusted
r = run([WH + "/hello", "--also", S + "/hello"], {"HV_CA_PATH": cadir}); q = reqs(r)
check("same handle, failure then success: the failure's diagnostics are gone after the success",
      q[1]["RC"] == "60" and int(q[1]["DIAG_BYTES"]) > 0 and q[2]["RC"] == "0" and q[2]["TLS_VERIFIED"] == "1" and q[2]["DIAG_BYTES"] == "0", r["stderr"][-600:])
r = run([S + "/hello", "--also", WH + "/hello"], {"HV_CA_PATH": cadir}); q = reqs(r)
check("same handle, success then failure: a success leaves no diagnostics, the failure has them",
      q[1]["RC"] == "0" and q[1]["DIAG_BYTES"] == "0" and q[2]["RC"] == "60" and int(q[2]["DIAG_BYTES"]) > 0, r["stderr"][-600:])
r = run([S + "/hello", "--also", B + "/hello"], {"HV_CA_PATH": cadir}); q = reqs(r)
check("same handle, TLS then plain http: diagnostics and TLS flag do not carry over", q[2]["RC"] == "0" and q[2]["TLS_VERIFIED"] == "0" and q[2]["DIAG_BYTES"] == "0", r["stderr"][-400:])

# --- per-host extra trust anchors (the mechanism that lets gs.apple.com chain to Apple Root CA) ---
dual = mkcert("dual", "localhost", ",IP:127.0.0.1")
srv_d, D_PORT = serve(None, tls=dual)
der = ssl.PEM_cert_to_DER_cert(open(dual[1]).read())
pin = hashlib.sha256(der).hexdigest()
D = f"https://localhost:{D_PORT}"
base = {"HV_CA_PATH": f"{work}/otherca"}   # a store that does not trust the server
r = run([D + "/hello"], base); check("control: without an extra anchor the server is untrusted", r["RC"] == "60", r)
r = run([D + "/hello"], dict(base, HV_EXTRA_ANCHOR=f"localhost|{dual[1]}|{pin}")); check("extra anchor with the right SHA-256 makes the host verify", r["RC"] == "0" and r["out"] == b"hello world", r)
r = run([D + "/hello"], dict(base, HV_EXTRA_ANCHOR=f"localhost|{dual[1]}|{'0' * 64}")); check("extra anchor with a wrong SHA-256 is refused (fail closed)", r["RC"] == "77" and "pinned SHA-256" in r["ERR"], r)
r = run([D + "/hello"], dict(base, HV_EXTRA_ANCHOR=f"other.example|{dual[1]}|{pin}")); check("an anchor for another host does not apply", r["RC"] == "60", r)
r = run([D + "/hello"], dict(base, HV_EXTRA_ANCHOR=f"LOCALHOST|{dual[1]}|{pin}")); check("host match ignores case", r["RC"] == "0", r)
r = run([f"https://127.0.0.1:{D_PORT}/hello"], dict(base, HV_EXTRA_ANCHOR=f"127.0.0.1|{dual[1]}|{pin}")); check("control: the certificate also covers 127.0.0.1 when that host is anchored", r["RC"] == "0", r)
r = run([D + "/to-ip", "--follow"], dict(base, HV_EXTRA_ANCHOR=f"localhost|{dual[1]}|{pin}")); check("redirect to another host: its trust is chosen again, so it is refused", r["RC"] == "60", r)
r = run([D + "/to-ip", "--follow"], dict(base, HV_EXTRA_ANCHOR=f"localhost|{os.devnull}|")); check("unreadable/empty anchor file fails closed", r["RC"] == "77", r)

# --- real network, informational: skipped when unreachable ---
r = run(["--ca"]); print("INFO system CA:", r["stderr"].strip())
if r["code"] == 0:
    g = run(["https://gs.apple.com/TSS/controller?action=2", "--discard"], timeout=90)
    if g.get("RC") in ("6", "7", "28", None):
        print("SKIP gs.apple.com (no network):", g.get("ERR"))
    else:
        check("gs.apple.com verifies with the system store + the pinned Apple Root CA", "TLS_VERIFIED=1" in g["stderr"], g["stderr"][:300])
    n = run(["https://example.com/"], timeout=90)
    if n.get("RC") in ("6", "7", "28", "56", None):
        print("SKIP public https (no network):", n.get("ERR"))
    else:
        check("public https example.com with system CAs", n["RC"] == "0" and n["HTTP"] == "200" and b"Example Domain" in n["out"], n)
else:
    print("SKIP public https: no system CA directory on this host")
print("FAILED:", fails if fails else "none")
sys.exit(1 if fails else 0)
