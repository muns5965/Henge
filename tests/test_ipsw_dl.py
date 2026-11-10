#!/usr/bin/env python3
"""Tests of host/ipsw_dl.c through the ipsw_dl tool, against local Range servers. Nothing leaves the machine.
usage: test_ipsw_dl.py TEST_TOOL SHIPPED_TOOL WORKDIR
TEST_TOOL is built with HV_DL_TEST + HV_DL_ALLOW_HTTP_TEST (loopback http, test arguments); SHIPPED_TOOL without them."""
import hashlib, http.server, os, random, re, shutil, signal, socket, socketserver, subprocess, sys, threading, time

tool, shipped, work = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3])
shutil.rmtree(work, ignore_errors=True); os.makedirs(work)
fails = []
def check(name, ok, info=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(info)[:700])); sys.stdout.flush()
    if not ok: fails.append(name)

rnd = random.Random(7)
N = 25 * 1024 * 1024 + 12345
DATA = rnd.randbytes(N)
SHA = hashlib.sha256(DATA).hexdigest()
M = {}            # server behaviour, set per test
REQS = []         # (path, range, if_range)

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        rng, ifr = self.headers.get("Range"), self.headers.get("If-Range")
        REQS.append((self.path, rng, ifr))
        if "status" in M:
            self.send_response(M["status"]); self.send_header("Content-Length", "0"); self.send_header("Connection", "close"); self.end_headers(); return
        big = self.path.startswith("/big")
        total = BIG_TOTAL if big else N
        etag = M.get("etag", '"e1"')
        start, end = 0, total - 1
        mt = re.match(r"bytes=(\d+)-(\d*)$", rng or "")
        if mt: start = int(mt[1]); end = int(mt[2]) if mt[2] else total - 1
        full = (not mt) or M.get("ignore_range") or (ifr is not None and ifr != etag and not M.get("no_ifrange"))
        if full: start, end = 0, total - 1
        body_len = end - start + 1
        self.send_response(200 if full else 206)
        if not full:
            cr_start = start + M.get("cr_start_add", 0)
            cr_total = M.get("cr_total", total)
            cr_end = end + M.get("cr_end_add", 0)
            self.send_header("Content-Range", f"bytes {cr_start}-{cr_end}/{cr_total}")
        if etag: self.send_header("ETag", etag)
        self.send_header("Content-Length", str(body_len)); self.send_header("Connection", "close"); self.end_headers()
        sent = 0; chunk = 256 * 1024
        drop = M.pop("drop_once", None) if "drop_once" in M else None
        try:
            while sent < body_len:
                n = min(chunk, body_len - sent)
                if drop is not None and sent + n > drop: n = drop - sent
                if big: piece = (b"\0" * n) if start + sent + n <= BIG_PRE else TAIL[start + sent - BIG_PRE: start + sent - BIG_PRE + n]
                else: piece = DATA[start + sent: start + sent + n]
                self.wfile.write(piece); sent += n
                if drop is not None and sent >= drop:
                    self.wfile.flush(); self.connection.shutdown(socket.SHUT_RDWR); break
                if M.get("slow"): time.sleep(0.1)
        except OSError: pass
        self.close_connection = True

class TS(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True
srv = TS(("127.0.0.1", 0), H); threading.Thread(target=srv.serve_forever, daemon=True).start()
URL = f"http://127.0.0.1:{srv.server_address[1]}/f.ipsw"
BIG_PRE = 4 * 1024**3 + 512 * 1024**2        # 4.5 GiB of zeros, then a random tail
TAIL = rnd.randbytes(6 * 1024 * 1024 + 99)
BIG_TOTAL = BIG_PRE + len(TAIL)
BURL = f"http://127.0.0.1:{srv.server_address[1]}/big.ipsw"

def fresh(name):
    d = os.path.join(work, name); shutil.rmtree(d, ignore_errors=True); os.makedirs(d); M.clear(); del REQS[:]; return d
def args(d, *extra, url=URL, size=N, sha=SHA, stop=None, cap=None, reserve=1 << 20):
    a = ["--dir", d, "--url", url, "--size", str(size), "--sha256", sha, "--name", "t.ipsw", "--reserve", str(reserve), "--commit-every", str(1 << 20)]
    if stop: a += ["--stop-after", str(stop)]
    if cap is not None: a += ["--avail-cap", str(cap)]
    return a + list(extra)
def run(a, t=tool, timeout=120):
    t0 = time.time(); r = subprocess.run([t] + a, capture_output=True, timeout=timeout)
    out = r.stdout.decode() + r.stderr.decode(); return out, r.returncode, time.time() - t0
def fsize(p): return os.path.getsize(p) if os.path.exists(p) else None
def P(d, ext=""): return os.path.join(d, "t.ipsw" + ext)
def committed(d):
    out, _, _ = run(["--status", "--dir", d, "--name", "t.ipsw", "--size", str(N), "--url", URL, "--sha256", SHA])
    m = re.search(r"committed=(-?\d+)", out); return int(m[1]) if m else None
def fnv(b):
    h = 1469598103934665603
    for c in b: h ^= c; h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h
def write_meta(d, url, size, etag, com):
    body = f"HENGE-DL 1\nurl={url}\nsize={size}\netag={etag}\ncommitted={com}\n".encode()
    open(P(d, ".meta"), "wb").write(body + f"check={fnv(body):016x}\n".encode())
def read(p): return open(p, "rb").read()

# ---- 1. a complete download ----
d = fresh("t1"); out, rc, _ = run(args(d))
check("fresh download: exit 0, SHA-256 verified, installed, the .part/.meta files are gone", rc == 0 and read(P(d)) == DATA and fsize(P(d, ".part")) is None and fsize(P(d, ".meta")) is None and "SHA256 computed " + SHA in out, (rc, out[-500:]))
check("  one request, 'bytes=0-', no If-Range on the first request", len(REQS) == 1 and REQS[0][1] == "bytes=0-" and REQS[0][2] is None, REQS)
out, rc, _ = run(args(d)); n = len(REQS)
check("  run again with the file in place: verified from disk, no request", rc == 0 and "already complete" in out and n == 1, (rc, out[-300:], n))

# ---- 2. pause and resume ----
d = fresh("t2"); out, rc, _ = run(args(d, stop=5000001))
check("pause at 5000001 bytes: exit 7, .part is exactly that long, metadata says the same", rc == 7 and fsize(P(d, ".part")) == 5000001 and committed(d) == 5000001 and fsize(P(d)) is None, (rc, out[-300:], fsize(P(d, ".part")), committed(d)))
out, rc, _ = run(args(d, stop=5000001))
check("  paused again at the same point: no request is sent", rc == 7 and len(REQS) == 1, (rc, len(REQS), out[-200:]))
out, rc, _ = run(args(d, stop=9000000))
check("  continue to 9000000: Range from 5000001 with If-Range and the stored strong ETag", rc == 7 and REQS[-1][1] == "bytes=5000001-" and REQS[-1][2] == '"e1"' and fsize(P(d, ".part")) == 9000000 and "REPLY 206 Content-Range bytes 5000001-" in out and "equals the stored one" in out, (rc, REQS, out[-300:]))
out, rc, _ = run(args(d))
check("  continue to the end: exit 0, the file equals the original, resumed at 9000000", rc == 0 and read(P(d)) == DATA and REQS[-1][1] == "bytes=9000000-" and "re-reading 9000000" in out, (rc, REQS[-1], out[-400:]))

# ---- 3. the connection drops ----
d = fresh("t3"); M["drop_once"] = 10 * 1024 * 1024 + 777
out, rc, _ = run(args(d))
ps = fsize(P(d, ".part"))
check("connection dropped mid-body: exit 5, the metadata and .part agree on what was kept", rc == 5 and ps is not None and ps > 9 * 1024 * 1024 and committed(d) == ps, (rc, ps, committed(d), out[-300:]))
out, rc, _ = run(args(d))
check("  run again: resumes from the kept bytes and completes, verified", rc == 0 and read(P(d)) == DATA and REQS[-1][1] == f"bytes={ps}-", (rc, REQS[-1], out[-300:]))

# ---- 4. the file on the server changes ----
d = fresh("t4"); run(args(d, stop=5 << 20)); before = (read(P(d, ".part")), read(P(d, ".meta")))
M["etag"] = '"e2"'
out, rc, _ = run(args(d))
check("ETag changed (server answers 200 to If-Range): exit 4, nothing written, .part and metadata untouched, nothing installed",
      rc == 4 and fsize(P(d)) is None and (read(P(d, ".part")), read(P(d, ".meta"))) == before and "200" in out, (rc, out[-300:]))
M["no_ifrange"] = True
out, rc, _ = run(args(d))
check("ETag changed (server ignores If-Range, answers 206 with the new ETag): exit 4 'ETag changed', files untouched", rc == 4 and "ETag changed" in out and read(P(d, ".part")) == before[0], (rc, out[-300:]))
M.clear()
out, rc, _ = run(args(d))
check("  the original file back on the server: the download continues and verifies", rc == 0 and read(P(d)) == DATA, (rc, out[-300:]))

# ---- 5. replies that must not be trusted ----
for name, setting, want in [("Range ignored (200 on the first request)", {"ignore_range": True}, "200"),
                            ("Content-Range starts one byte late", {"cr_start_add": 1}, "starts at"),
                            ("Content-Range names another total", {"cr_total": N + 1}, "file size"),
                            ("Content-Range ends early", {"cr_end_add": -1}, "ends at"),
                            ("weak ETag", {"etag": 'W/"w"'}, "weak"),
                            ("no ETag at all", {"etag": None}, "no ETag")]:
    d = fresh("t5"); M.update(setting)
    out, rc, _ = run(args(d))
    check(f"{name}: exit 4, nothing written, nothing installed", rc == 4 and want in out and (fsize(P(d, ".part")) or 0) == 0 and fsize(P(d)) is None and len(REQS) == 1, (rc, out[-300:], fsize(P(d, ".part"))))
for st, rcw in [(503, 5), (429, 5), (403, 4), (404, 4), (416, 4)]:
    d = fresh("t5s"); M["status"] = st; out, rc, _ = run(args(d))
    check(f"HTTP {st}: exit {rcw} ({'try again' if rcw == 5 else 'not trusted'})", rc == rcw and (fsize(P(d, ".part")) or 0) == 0, (rc, out[-200:]))

# ---- 6. integrity ----
d = fresh("t6"); out, rc, _ = run(args(d, sha="0" * 64))
check("wrong expected SHA-256: exit 6, the real hash is printed, .part and metadata kept, nothing installed", rc == 6 and SHA in out and fsize(P(d)) is None and fsize(P(d, ".part")) == N and fsize(P(d, ".meta")), (rc, out[-400:]))
n = len(REQS); out, rc, _ = run(args(d))
check("  the right value later: the kept file is verified without a new request and installed", rc == 0 and len(REQS) == n and read(P(d)) == DATA, (rc, len(REQS), n, out[-300:]))
d = fresh("t6b"); run(args(d, stop=5 << 20))
f = open(P(d, ".part"), "r+b"); f.seek(1000); f.write(b"\xff"); f.close()
out, rc, _ = run(args(d))
check("a damaged byte inside the committed prefix (same size): the download completes but the SHA-256 catches it, exit 6", rc == 6 and fsize(P(d)) is None and fsize(P(d, ".part")) == N, (rc, out[-300:]))

# ---- 7. damaged or inconsistent state: no automatic resume ----
d = fresh("t7a"); run(args(d, stop=5 << 20)); f = open(P(d, ".part"), "ab"); f.write(b"J" * 1000); f.close(); n = len(REQS)
out, rc, _ = run(args(d))
check("crash after the data but before the metadata (.part longer): the excess is cut off, the download completes and verifies", rc == 0 and read(P(d)) == DATA and "cutting 1000 uncommitted" in out, (rc, out[-300:]))
d = fresh("t7b"); run(args(d, stop=5 << 20)); os.truncate(P(d, ".part"), (5 << 20) - 10); n = len(REQS)
out, rc, _ = run(args(d))
check(".part shorter than the metadata committed: exit 3, no request", rc == 3 and len(REQS) == n and "shorter" in out, (rc, out[-300:]))
d = fresh("t7c"); run(args(d, stop=5 << 20)); m = bytearray(read(P(d, ".meta"))); m[20] ^= 1; open(P(d, ".meta"), "wb").write(bytes(m)); n = len(REQS)
out, rc, _ = run(args(d))
check("metadata damaged: exit 3, no request", rc == 3 and len(REQS) == n, (rc, out[-300:]))
d = fresh("t7d"); run(args(d, stop=5 << 20)); os.unlink(P(d, ".meta")); n = len(REQS)
out, rc, _ = run(args(d))
check(".part without metadata: exit 3, no request, the data is not touched", rc == 3 and len(REQS) == n and fsize(P(d, ".part")) == 5 << 20, (rc, out[-300:]))
d = fresh("t7e"); run(args(d, stop=5 << 20)); os.unlink(P(d, ".part")); n = len(REQS)
out, rc, _ = run(args(d))
check("metadata without .part: exit 3, no request", rc == 3 and len(REQS) == n, (rc, out[-300:]))
d = fresh("t7f"); run(args(d, stop=5 << 20)); n = len(REQS)
out, rc, _ = run(args(d, url=URL + "?other"))
check("metadata for another URL: exit 3, no request", rc == 3 and len(REQS) == n, (rc, out[-300:]))
d = fresh("t7g"); open(P(d), "wb").write(b"x" * 100); n = len(REQS)
out, rc, _ = run(args(d))
check("a file with the final name but another size: exit 3, left alone", rc == 3 and fsize(P(d)) == 100 and len(REQS) == n, (rc, out[-300:]))
d = fresh("t7h"); open(P(d), "wb").write(b"x" * N)
out, rc, _ = run(args(d))
check("a file with the final name, right size, wrong content: exit 6, no request", rc == 6 and len(REQS) == 0, (rc, out[-300:]))

# ---- 8. free space, URL policy ----
d = fresh("t8"); out, rc, _ = run(args(d, cap=1000))
check("not enough free space: exit 2 before any request, no .part created", rc == 2 and len(REQS) == 0 and fsize(P(d, ".part")) is None and "not enough free space" in out, (rc, out[-300:]))
d = fresh("t8b"); run(args(d, stop=20 << 20, reserve=0)); n = len(REQS)
need = N - (20 << 20) + (1 << 20)
out, rc, _ = run(args(d, cap=need - 5000))
check("resume needs only the REMAINING bytes plus the reserve: a cap a little below that is refused...", rc == 2 and len(REQS) == n, (rc, out[-300:]))
out, rc, _ = run(args(d, cap=need + 5000))
check("  ...and a cap a little above it is enough", rc == 0 and read(P(d)) == DATA, (rc, out[-300:]))
for bad in ["http://example.com/f.ipsw", "ftp://127.0.0.1/f.ipsw", "file:///etc/passwd"]:
    d = fresh("t8c"); out, rc, _ = run(args(d, url=bad))
    check(f"URL {bad}: exit 2, nothing sent", rc == 2 and len(REQS) == 0 and "not https" in out, (rc, out[-200:]))
d = fresh("t8d"); out, rc, _ = run(args(d, sha="12345"))
check("a malformed expected SHA-256: exit 2", rc == 2 and len(REQS) == 0, (rc, out[-200:]))

# ---- 9. one downloader at a time, cancel ----
d = fresh("t9"); M["slow"] = True
p = subprocess.Popen([tool] + args(d), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
for _ in range(100):
    if REQS and (fsize(P(d, ".part")) or 0) > 0: break
    time.sleep(0.1)
out2, rc2, _ = run(args(d))
check("a second downloader on the same file: exit 2 'another download'", rc2 == 2 and "another download" in out2 and len(REQS) == 1, (rc2, out2[-200:]))
out3, rc3, _ = run(["--delete-partial", "--dir", d, "--name", "t.ipsw", "--size", str(N), "--url", URL, "--sha256", SHA])
check("  deleting the partial files while it runs is refused", rc3 == 2 and fsize(P(d, ".part")) is not None, (rc3, out3))
time.sleep(0.5); p.send_signal(signal.SIGTERM)
try: o, _ = p.communicate(timeout=15)
except subprocess.TimeoutExpired: p.kill(); o = b"TIMEOUT"
ps = fsize(P(d, ".part"))
check("SIGTERM during the transfer: exit 5, the state is flushed and consistent", p.returncode == 5 and ps and committed(d) == ps, (p.returncode, ps, committed(d), o[-200:]))
M.clear(); out, rc, _ = run(args(d))
check("  and the download resumes and verifies", rc == 0 and read(P(d)) == DATA, (rc, out[-300:]))

# ---- 10. delete-partial ----
d = fresh("t10"); run(args(d, stop=5 << 20))
out, rc, _ = run(["--delete-partial", "--dir", d, "--name", "t.ipsw", "--size", str(N), "--url", URL, "--sha256", SHA])
check("delete-partial: .part and .meta are removed", rc == 0 and fsize(P(d, ".part")) is None and fsize(P(d, ".meta")) is None, (rc, out))
run(args(d)); out, rc, _ = run(["--delete-partial", "--dir", d, "--name", "t.ipsw", "--size", str(N), "--url", URL, "--sha256", SHA])
check("  a finished file is never removed by it", rc == 0 and read(P(d)) == DATA, (rc, out))

# ---- 11. offsets beyond 4 GiB ----
d = fresh("t11"); h = hashlib.sha256(); z = b"\0" * (64 << 20)
for _ in range(BIG_PRE // len(z)): h.update(z)
h.update(TAIL); bsha = h.hexdigest()
f = open(P(d, ".part"), "wb"); f.truncate(BIG_PRE); f.close()
write_meta(d, BURL, BIG_TOTAL, '"e1"', BIG_PRE)
t0 = time.time(); out, rc, _ = run(args(d, url=BURL, size=BIG_TOTAL, sha=bsha), timeout=600)
check("a 4.5 GiB prefix already on disk: Range 'bytes=4831838208-', the tail is fetched, the SHA-256 over all %.1f GiB verifies" % (BIG_TOTAL / 2**30),
      rc == 0 and REQS and REQS[-1][1] == f"bytes={BIG_PRE}-" and os.path.getsize(P(d)) == BIG_TOTAL, (rc, REQS, out[-400:]))

# ---- 12. the shipped build ----
d = fresh("t12"); n = len(REQS)
out, rc, _ = run(["--dir", d], t=shipped)
check("shipped build without --stop-after: refused, nothing sent ('a full download is not enabled')", rc == 2 and "not enabled" in out, (rc, out))
out, rc, _ = run(["--dir", d, "--stop-after", str(2 << 30)], t=shipped)
check("shipped build with a stop point above 1 GiB: refused", rc == 2 and "not enabled" in out, (rc, out))
out, rc, _ = run(["--dir", d, "--stop-after", "1000", "--url", URL], t=shipped)
check("shipped build has no --url (or any other test argument)", rc == 2 and "unknown argument" in out, (rc, out))
st = subprocess.run(["strings", shipped], capture_output=True).stdout.decode()
check("shipped binary: compiled-in Apple URL and SHA-256, no loopback allowance", "updates.cdn-apple.com" in st and "3d87686b691ac765" in st and "127.0.0.1" not in st, "")
check("test binary does contain the loopback allowance (so the check above is not vacuous)", "127.0.0.1" in subprocess.run(["strings", tool], capture_output=True).stdout.decode(), "")

print("FAILED:", fails if fails else "none")
sys.exit(1 if fails else 0)
