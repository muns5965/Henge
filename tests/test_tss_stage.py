#!/usr/bin/env python3
"""Stop B: the real libtatsu + curl stand-in + connection policy + host/tss_stage.c against a fake TSS server on this machine.
usage: test_tss_stage.py TOOL WORKDIR      Nothing leaves the machine: every listener is on 127.0.0.1."""
import os, re, shutil, socket, ssl, subprocess, sys, threading, time

tool, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
fails = []
CAP = 1024 * 1024

def check(name, cond, info=""):
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  " + str(info)))
    if not cond: fails.append(name)

PLIST_HEAD = b'<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0"><dict><key>ApImg4Ticket</key><data>AAECAwQF</data><key>Pad</key><string>'
PLIST_TAIL = b'</string></dict></plist>'
OK_PREFIX = b"STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING="
def ok_body(total=None):
    base = OK_PREFIX + PLIST_HEAD + PLIST_TAIL
    if total is None: return base
    return OK_PREFIX + PLIST_HEAD + b"x" * (total - len(base)) + PLIST_TAIL

class Server:
    """A small raw HTTP/1.1 server. handlers: path prefix -> function(conn, reqno) writing the whole reply."""
    def __init__(self, tls=None):
        self.sock = socket.socket(); self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0)); self.sock.listen(64)
        self.port = self.sock.getsockname()[1]
        self.accepts = 0; self.requests = []; self.counter = {}
        self.ctx = None
        if tls:
            self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); self.ctx.load_cert_chain(tls[1], tls[0])
        threading.Thread(target=self.loop, daemon=True).start()
    def loop(self):
        while True:
            try: c, _ = self.sock.accept()
            except OSError: return
            self.accepts += 1
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()
    def serve(self, c):
        try:
            c.settimeout(30)
            if self.ctx: c = self.ctx.wrap_socket(c, server_side=True)
            buf = b""
            while b"\r\n\r\n" not in buf:
                d = c.recv(4096)
                if not d: return
                buf += d
            head, _, rest = buf.partition(b"\r\n\r\n")
            path = head.split(b" ")[1].decode()
            m = re.search(rb"(?i)content-length:\s*(\d+)", head); n = int(m.group(1)) if m else 0
            while len(rest) < n:
                d = c.recv(4096)
                if not d: break
                rest += d
            self.requests.append((path, len(rest)))
            k = path.split("?")[0]
            self.counter[k] = self.counter.get(k, 0) + 1
            self.handle(c, k, self.counter[k])
        except Exception:
            pass
        finally:
            try: c.close()
            except Exception: pass
    def reply(self, c, status, body, extra=b"", reason=b"OK"):
        c.sendall(b"HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Length: %d\r\n%s\r\n" % (status, reason, len(body), extra) + body)
    def handle(self, c, k, no):
        if k == "/ok": self.reply(c, 200, ok_body())
        elif k == "/ok_chunked":
            b = ok_body(); c.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n")
            for i in range(0, len(b), 37): p = b[i:i+37]; c.sendall(b"%x\r\n" % len(p) + p + b"\r\n")
            c.sendall(b"0\r\n\r\n")
        elif k == "/rejected": self.reply(c, 200, b"STATUS=8&MESSAGE=This device isn't eligible for the requested build.")
        elif k == "/h500": self.reply(c, 500, b"oops-server-text", reason=b"Internal Server Error")
        elif k == "/h404": self.reply(c, 404, b"oops-server-text", reason=b"Not Found")
        elif k == "/h500s": self.reply(c, 500, b"STATUS=8&MESSAGE=oops-server-text", reason=b"Internal Server Error")
        elif k == "/garbage": self.reply(c, 200, b"oops-server-text")
        elif k == "/notplist": self.reply(c, 200, b"STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING=<<not xml oops-server-text")
        elif k == "/exact": self.reply(c, 200, ok_body(CAP))
        elif k == "/over": self.reply(c, 200, ok_body(CAP + 1))
        elif k == "/big_cl":
            c.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: %d\r\n\r\n" % (CAP * 4)); c.sendall(b"y" * 1000); time.sleep(3)
        elif k == "/big_stream":   # no Content-Length, no chunking: body until close, far over the cap
            c.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n")
            blk = b"z" * 65536
            for _ in range(200): c.sendall(blk)
        elif k == "/big_chunks":
            c.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n")
            blk = b"z" * 65536
            for _ in range(200): c.sendall(b"%x\r\n" % len(blk) + blk + b"\r\n")
            c.sendall(b"0\r\n\r\n")
        elif k == "/big_decl_chunk":
            c.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n%x\r\n" % (CAP * 2)); c.sendall(b"y" * 100); time.sleep(3)
        elif k == "/hdr_flood":
            c.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n")
            line = b"X-Flood: " + b"h" * 4000 + b"\r\n"
            for _ in range(200): c.sendall(line)
            c.sendall(b"\r\n")
        elif k == "/cut_mid":
            c.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 1000\r\n\r\n" + b"q" * 100)
        elif k == "/silent": time.sleep(12)
        elif k == "/seq":
            self.reply(c, 200, ok_body() if no <= 2 else b"STATUS=8&MESSAGE=nope")
        else: self.reply(c, 404, b"")

def run(url, *args, env=None, timeout=120):
    e = dict(os.environ); e.update(env or {})
    t0 = time.time()
    r = subprocess.run([tool, url, *args], capture_output=True, env=e, timeout=timeout)
    return r.stdout.decode(), r.stderr.decode(), time.time() - t0

def parse(out):
    reqs = []
    for m in re.finditer(r"REQ (\d+) CODE=(\w+) ATTEMPTS=(\d+) HTTP=(\d+) TSS=(-?\d+) CURL=(\d+) DENIED=(\d+) TRIED=(\d+) BODY=(\d+) MAXBUF=(\d+)", out):
        g = m.groups()
        reqs.append(dict(i=int(g[0]), code=g[1], attempts=int(g[2]), http=int(g[3]), tss=int(g[4]), curl=int(g[5]), denied=int(g[6]), tried=int(g[7]), body=int(g[8]), maxbuf=int(g[9])))
    s = re.search(r"SENT=(\d+) STOPPED_AT=(-?\d+) CONNECTS=(\d+) ELAPSED_MS=(\d+)", out)
    return reqs, (dict(sent=int(s.group(1)), stopped=int(s.group(2)), connects=int(s.group(3)), ms=int(s.group(4))) if s else None)

LEAKS = ["2d2a8d94f32ba4d2", "3283686610", "ApECID", "ApImg4Ticket", "AAECAwQF", "oops-server-text", "isn't eligible", "REQUEST_STRING"]
def no_leak(name, out, err):
    bad = [w for w in LEAKS if w in out or w in err]
    check(name + ": output carries no ECID, ticket or server text", not bad, bad)

S = Server()
u = lambda s, p: f"http://127.0.0.1:{s.port}{p}"

# 1. normal reply
out, err, _ = run(u(S, "/ok"), "--allow-port", str(S.port)); r, t = parse(out)
check("normal reply: OK, one attempt, one connect(), the server saw one request", r and r[0]["code"] == "OK" and r[0]["attempts"] == 1 and t["connects"] == 1 and len(S.requests) == 1 and r[0]["http"] == 200 and r[0]["tss"] == 0, (out, S.requests))
no_leak("normal reply", out, err)
out, err, _ = run(u(S, "/ok_chunked"), "--allow-port", str(S.port)); r, t = parse(out)
check("chunked normal reply: OK", r and r[0]["code"] == "OK", out)

# 2. policy refusal: connect() never called, no retry, no bytes to the server
S2 = Server()
out, err, dt = run(u(S2, "/ok")); r, t = parse(out)
check("policy refusal: POLICY_DENIED, 0 connect() calls, 1 attempt (no retry), the server saw nothing, fast",
      r and r[0]["code"] == "POLICY_DENIED" and t["connects"] == 0 and r[0]["attempts"] == 1 and S2.accepts == 0 and r[0]["denied"] == 1 and r[0]["tried"] == 0 and dt < 5, (out, S2.accepts, dt))
# the exception is exactly one port: another loopback port stays refused although the test switch is on
out, err, _ = run(u(S2, "/ok"), "--allow-port", str(S.port)); r, t = parse(out)
check("test loopback: a different loopback port is still refused (0 connect(), server saw nothing)", r and r[0]["code"] == "POLICY_DENIED" and t["connects"] == 0 and S2.accepts == 0, (out, S2.accepts))
# private address and another loopback address stay refused with the switch on
out, err, _ = run(f"http://localhost:{S.port}/ok", "--allow-port", str(S.port), "--resolve", "private"); r, t = parse(out)
check("test loopback: private 10.x and 127.0.0.2 refused even on the allowed port (POLICY_DENIED, 0 connect())", r and r[0]["code"] == "POLICY_DENIED" and t["connects"] == 0 and r[0]["denied"] == 2, (out,))
# real unsupported protocol vs policy refusal, straight through the stand-in
out, err, _ = run("ftp://127.0.0.1:9/x", "--direct", "--allow-port", str(S.port))
check("a real unsupported protocol (ftp://) is CURLE_UNSUPPORTED_PROTOCOL without the policy flag", "DIRECT RC=1 POLICY_DENIED=0 DENIED=0 TRIED=0 CONNECTS=0" in out, out)
out, err, _ = run(u(S2, "/ok"), "--direct")
check("a policy refusal through the stand-in: same code 1, with the policy flag, 0 connect()", "DIRECT RC=1 POLICY_DENIED=1 DENIED=1 TRIED=0 CONNECTS=0" in out, out)
# mixed: one address refused, the other allowed but nobody listening -> a connection failure, not a policy refusal
sk = socket.socket(); sk.bind(("127.0.0.1", 0)); dead = sk.getsockname()[1]; sk.close()
out, err, _ = run(f"http://localhost:{dead}/ok", "--allow-port", str(dead), "--resolve", "mixed_dead", "--attempts", "2"); r, t = parse(out)
check("mixed refused + failed connect: NET_CONNECT (not POLICY_DENIED); 1 refused and 1 tried per attempt; retried as before",
      r and r[0]["code"] == "NET_CONNECT" and r[0]["denied"] == 1 and r[0]["tried"] == 1 and r[0]["attempts"] == 2 and t["connects"] == 2, (out,))
# mixed: one refused, the other answers -> OK; the refusal does not overwrite the result
n0 = len(S.requests)
out, err, _ = run(f"http://localhost:{S.port}/ok", "--allow-port", str(S.port), "--resolve", "mixed_ok"); r, t = parse(out)
check("one address refused, another answers: OK, with the refusal counted (1 refused, 1 tried, 1 connect())", r and r[0]["code"] == "OK" and r[0]["denied"] == 1 and r[0]["tried"] == 1 and t["connects"] == 1 and len(S.requests) == n0 + 1, (out,))

# 3. HTTP and TSS errors; libtatsu's retry decision is unchanged, the attempts are reported as observed
for path, code, tss in [("/h404", "HTTP_ERROR", -1), ("/h500s", "HTTP_ERROR", 8), ("/garbage", "BAD_REPLY", -1), ("/notplist", "BAD_REPLY", -1), ("/rejected", "SERVER_REFUSED", 8)]:
    n0 = len(S.requests)
    out, err, _ = run(u(S, path), "--allow-port", str(S.port), "--attempts", "2"); r, t = parse(out)
    seen = len(S.requests) - n0
    check(f"{path}: {code}; the attempts reported ({r[0]['attempts'] if r else '?'}) equal the requests the server saw ({seen})", r and r[0]["code"] == code and r[0]["attempts"] == seen and (tss < 0 or r[0]["tss"] == tss) and r[0]["attempts"] >= 1, (out, seen))
    no_leak(path, out, err)
n0 = len(S.requests)
out, err, dt = run(u(S, "/h500"), "--allow-port", str(S.port)); r, t = parse(out)
check("HTTP 500 without STATUS keeps libtatsu's retry (6 attempts at most) and is reported as HTTP_ERROR with the observed attempts",
      r and r[0]["code"] == "HTTP_ERROR" and r[0]["http"] == 500 and r[0]["attempts"] == len(S.requests) - n0 and 1 <= r[0]["attempts"] <= 6, (out, len(S.requests) - n0))
n0 = len(S.requests)
out, err, _ = run(u(S, "/cut_mid"), "--allow-port", str(S.port), "--attempts", "2"); r, t = parse(out)
check("a reply cut off in the middle is a transport failure (NET_IO), not an HTTP error, although the status line was seen", r and r[0]["code"] == "NET_IO" and r[0]["curl"] == 56, (out,))

# 4. size caps
n0 = len(S.requests)
out, err, _ = run(u(S, "/exact"), "--allow-port", str(S.port)); r, t = parse(out)
check("a body of exactly 1 MiB is accepted", r and r[0]["code"] == "OK" and r[0]["body"] == CAP, (out,))
for path, why in [("/over", "1 MiB + 1 byte"), ("/big_cl", "Content-Length 4 MiB declared"), ("/big_stream", "no length, 12 MiB sent"), ("/big_chunks", "chunked, 12 MiB sent"), ("/big_decl_chunk", "one chunk declaring 2 MiB"), ("/hdr_flood", "800 KB of headers")]:
    n0 = len(S.requests)
    out, err, dt = run(u(S, path), "--allow-port", str(S.port)); r, t = parse(out)
    ok = r and r[0]["code"] == "REPLY_TOO_LARGE" and r[0]["attempts"] == 1 and len(S.requests) - n0 == 1 and r[0]["body"] <= CAP and r[0]["maxbuf"] <= 262144
    check(f"{path} ({why}): REPLY_TOO_LARGE, one attempt, at most 1 MiB reached libtatsu ({r[0]['body'] if r else '?'}), shim buffers stayed small ({r[0]['maxbuf'] if r else '?'})", ok, (out, len(S.requests) - n0))
    if path in ("/over", "/big_cl", "/big_decl_chunk"):
        check(f"{path}: nothing of the body reached libtatsu", r and r[0]["body"] == 0, out)

# 5. a sequence stops at the first request that is not OK
n0 = len(S.requests)
out, err, _ = run(u(S, "/seq"), "--allow-port", str(S.port), "--n", "5", "--attempts", "1"); r, t = parse(out)
check("sequence of 5: stops at the first failure (third), sends no more", t and t["sent"] == 3 and t["stopped"] == 2 and [x["code"] for x in r] == ["OK", "OK", "SERVER_REFUSED"] and S.counter.get("/seq") == 3, (out, S.counter))

# 6. cancel: the stages the code supports
S.counter.clear()
out, err, dt = run(u(S, "/silent"), "--allow-port", str(S.port), "--cancel-after-ms", "500", "--attempts", "1"); r, t = parse(out)
check("cancel while waiting for the reply: CANCELLED within the 200 ms wait slice (<3 s of a 12 s silence)", r and r[0]["code"] == "CANCELLED" and dt < 3, (out, dt))
out, err, dt = run(u(S, "/h500"), "--allow-port", str(S.port), "--cancel-after-ms", "800"); r, t = parse(out)
check("cancel during libtatsu's retry wait: CANCELLED, no later than the end of that wait plus the next attempt's first check (sleep(2) is not interruptible: documented, bound 5 s)", r and r[0]["code"] == "CANCELLED" and dt < 5, (out, dt))

# 7. TLS: a bad certificate stops at once and nothing is sent
def mkcert(name, cn):
    key, crt, cfg = f"{work}/{name}.key", f"{work}/{name}.crt", f"{work}/{name}.cnf"
    open(cfg, "w").write(f"[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nCN={cn}\n[v3]\nsubjectAltName=DNS:{cn}\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
    subprocess.check_call(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt, "-days", "2", "-config", cfg], stderr=subprocess.DEVNULL)
    return key, crt
c = mkcert("good", "localhost"); cad = f"{work}/cadir_good"; os.makedirs(cad, exist_ok=True); shutil.copy(c[1], cad + "/c.pem")
ST = Server(tls=c)
out, err, _ = run(f"https://localhost:{ST.port}/ok", "--allow-port", str(ST.port), env={"HV_CA_PATH": cad}); r, t = parse(out)
check("https over a trusted certificate: OK", r and r[0]["code"] == "OK" and len(ST.requests) == 1, (out, err[-200:]))
other = f"{work}/cadir_other"; os.makedirs(other, exist_ok=True); shutil.copy(mkcert("other", "other.example")[1], other + "/o.pem")
ST.requests.clear()
out, err, dt = run(f"https://localhost:{ST.port}/ok", "--allow-port", str(ST.port), env={"HV_CA_PATH": other}); r, t = parse(out)
check("untrusted certificate: TLS_FAILED, one attempt, no request reached the server", r and r[0]["code"] == "TLS_FAILED" and r[0]["attempts"] == 1 and len(ST.requests) == 0 and dt < 8, (out, len(ST.requests)))
mm = mkcert("mismatch", "other.example"); mmd = f"{work}/cadir_mm"; os.makedirs(mmd, exist_ok=True); shutil.copy(mm[1], mmd + "/m.pem")
SM = Server(tls=mm)
out, err, dt = run(f"https://localhost:{SM.port}/ok", "--allow-port", str(SM.port), env={"HV_CA_PATH": mmd}); r, t = parse(out)
check("trusted certificate for another host name: TLS_FAILED, one attempt, nothing sent", r and r[0]["code"] == "TLS_FAILED" and r[0]["attempts"] == 1 and len(SM.requests) == 0, (out,))
no_leak("tls", out, err)

# 8. no URL that is not https (libtatsu's own check) -> NOT_SENT
out, err, _ = run("ftp://127.0.0.1/x", "--allow-port", str(S.port)); r, t = parse(out)
check("a URL libtatsu refuses before any attempt: NOT_SENT, 0 connect()", r and r[0]["code"] == "NOT_SENT" and t["connects"] == 0, out)

print("FAILED:", fails if fails else "none")
sys.exit(1 if fails else 0)
