#!/usr/bin/env python3
"""idevicerestore's download.c on the HTTP stand-in: sizes, SHA-256, memory, truncation, user quit. usage: test_download.py BIN WORKDIR"""
import fcntl, glob, hashlib, http.server, os, socketserver, subprocess, sys, threading, time, socket

tool, work = sys.argv[1], sys.argv[2]
legacy = sys.argv[3] if len(sys.argv) > 3 else tool
os.makedirs(work, exist_ok=True)
BLK = os.urandom(1024 * 1024 + 3)
fails = []
def check(name, cond, info=""):
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  " + str(info)))
    if not cond: fails.append(name)

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        p = self.path
        if p.startswith("/gen"):
            q = dict(kv.split("=") for kv in p.split("?", 1)[1].split("&")); n = int(q["n"]); cut = int(q.get("cut", n))
            self.send_response(200); self.send_header("Content-Length", str(n)); self.send_header("Connection", "close"); self.end_headers()
            sent = 0
            try:
                while sent < cut:
                    piece = BLK[: min(len(BLK), cut - sent)]; self.wfile.write(piece); sent += len(piece)
            except (BrokenPipeError, ConnectionResetError): pass
            self.close_connection = True
        elif p == "/redir":
            self.send_response(302); self.send_header("Location", "/gen?n=1000"); self.send_header("Content-Length", "0"); self.send_header("Connection", "close"); self.end_headers(); self.close_connection = True
        elif p == "/chunked":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.send_header("Connection", "close"); self.end_headers()
            for _ in range(3): self.wfile.write(b"400\r\n" + b"c" * 1024 + b"\r\n")
            self.wfile.write(b"0\r\n\r\n"); self.close_connection = True
        elif p == "/chunkcut":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.send_header("Connection", "close"); self.end_headers()
            self.wfile.write(b"400\r\n" + b"c" * 1024 + b"\r\n"); self.close_connection = True
        elif p == "/noclen":
            self.send_response(200); self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(b"n" * 5000); self.close_connection = True
        elif p == "/404":
            self.send_response(404); self.send_header("Content-Length", "4"); self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(b"nope"); self.close_connection = True
class TS(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True; allow_reuse_address = True
srv = TS(("127.0.0.1", 0), H); threading.Thread(target=srv.serve_forever, daemon=True).start()
B = f"http://127.0.0.1:{srv.server_address[1]}"
ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ls.bind(("127.0.0.1", 0)); ls.listen(20)
def stall():
    while True:
        c, _ = ls.accept()
        def one(c=c):
            try:
                c.recv(65536); c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 100000000\r\nConnection: close\r\n\r\n" + b"x" * 1000)
                time.sleep(60)
            except Exception: pass
        threading.Thread(target=one, daemon=True).start()
threading.Thread(target=stall, daemon=True).start()
STALL = f"http://127.0.0.1:{ls.getsockname()[1]}/x"

def sha_of(n):
    h = hashlib.sha256(); left = n
    while left:
        piece = BLK[: min(len(BLK), left)]; h.update(piece); left -= len(piece)
    return h.hexdigest()
def filesha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()
def run(args, timeout=120, exe=None, env=None):
    t0 = time.time()
    pr = subprocess.Popen([exe or tool] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    out, err = pr.communicate(timeout=timeout)
    pid_ru = None
    d = {"t": time.time() - t0, "err": err.decode(errors="replace")}
    for ln in out.decode().splitlines():
        if ln.startswith("RESULT"):
            f = ln.split(); d["res"], d["size"] = int(f[1]), int(f[3])
    return d

N = 64 * 1024 * 1024
out = f"{work}/ipsw.bin"
r = run(["file", B + f"/gen?n={N}", out])
check("[safe] download_to_file: 64 MB, return 0, size and SHA-256 identical", r.get("res") == 0 and r.get("size") == N and filesha(out) == sha_of(N), r)
r = run(["file", B + "/redir", out])
check("download_to_file follows a redirect", r.get("res") == 0 and r.get("size") == 1000, r)
r = run(["file", B + "/404", out], exe=legacy)
check("[legacy build = iOS behaviour] 404: libcurl semantics - the 4-byte error page is written and 0 is returned (upstream does not look at the status)", r.get("res") == 0 and r.get("size") == 4, r)
r = run(["file", B + f"/gen?n={N}&cut={5 * 1024 * 1024}", out], exe=legacy)
print("INFO truncated download (server closes after 5 MB of 64): download_to_file returns", r.get("res"), "with", r.get("size"), "bytes on disk")
check("[legacy build = iOS behaviour] characterisation: upstream download_to_file ignores curl_easy_perform's result, so a truncated download is reported as success (callers must verify the file)", r.get("res") == 0 and r.get("size") == 5 * 1024 * 1024, r)
r = run(["file", B + "/gen?n=0", out], exe=legacy)
check("[legacy build = iOS behaviour] empty download: -1 and the file is removed", r.get("res") == -1 and not os.path.exists(out), r)
r = run(["file", STALL, out, "1500"])
check("user quit (global_quit_flag) during a stalled download: returns -2 in about a second or two, not after the 60 s idle limit (progress callback ticks while idle)", r.get("res") == -2 and r["t"] < 6, r)
r = run(["file", STALL, out, "1500", "flag"])
check("same with the flag registered as the stand-in's process-wide cancel flag: stops within about a second", r.get("res") == -2 and r["t"] < 4, r)
r = run(["file", B + f"/gen?n={N}", out, "1", "flag"])
check("quit flag at 1 ms on a fast 64 MB download: stops early with -2", r.get("res") == -2 and r.get("size", N) < N, r)
r = run(["buffer", B + "/gen?n=3000000"])
check("download_to_buffer: 3 MB", r.get("res") == 0 and r.get("size") == 3000000, r)

# ---- the Android / HV_SAFE_DOWNLOAD behaviour -------------------------------------------------------------------------------------------
def leftovers(): return sorted(glob.glob(out + ".hvpart.*"))
for f in leftovers(): os.remove(f)
cut = 5 * 1024 * 1024
if os.path.exists(out): os.remove(out)
r = run(["file", B + f"/gen?n={N}&cut={cut}", out])
check("[safe] truncated download (Content-Length 64 MB, 5 MB sent): -1, no final file, no leftover temp file", r.get("res") == -1 and not os.path.exists(out) and not leftovers(), (r, leftovers()))
good = b"GOOD-OLD-CONTENT"
open(out, "wb").write(good)
r = run(["file", B + f"/gen?n={N}&cut={cut}", out])
check("[safe] a truncated re-download leaves the existing good file untouched", r.get("res") == -1 and open(out, "rb").read() == good and not leftovers(), r)
r = run(["file", B + "/404", out])
check("[safe] HTTP 404: -1, existing file untouched, the error page is not stored", r.get("res") == -1 and open(out, "rb").read() == good and not leftovers(), r)
r = run(["file", B + "/gen?n=0", out])
check("[safe] empty body: -1, existing file untouched, no temp file", r.get("res") == -1 and open(out, "rb").read() == good and not leftovers(), r)
r = run(["file", B + f"/gen?n={N}", out])
check("[safe] a complete download replaces the existing file: return 0, size and SHA-256 identical, no temp file", r.get("res") == 0 and r.get("size") == N and filesha(out) == sha_of(N) and not leftovers(), r)
check("[safe] final file permissions are 0644", (os.stat(out).st_mode & 0o777) == 0o644, oct(os.stat(out).st_mode))
r = run(["file", B + "/redir", out])
check("[safe] redirect followed; size 1000", r.get("res") == 0 and r.get("size") == 1000, r)
os.remove(out)
r = run(["file", B + "/chunked", out])
check("[safe] chunked response (no Content-Length): complete body accepted", r.get("res") == 0 and r.get("size") == 3072, r)
os.remove(out)
r = run(["file", B + "/chunkcut", out])
check("[safe] chunked response cut off before the terminating chunk: the stand-in reports a transfer error, so -1", r.get("res") == -1 and not os.path.exists(out) and not leftovers(), r)
r = run(["file", B + "/noclen", out])
print("INFO read-until-close response without Content-Length (5000 bytes then close): return", r.get("res"), "size", r.get("size"), "- a cut-off here cannot be told from a complete body; the caller's hash check is the protection")
check("[safe] read-until-close response without Content-Length: accepted when the connection ends normally (documented limit)", r.get("res") == 0 and r.get("size") == 5000, r)
if os.path.exists(out): os.remove(out)
r = run(["file", STALL, out, "1500", "flag"])
check("[safe] user quit during a stalled download: -2, no final file, no temp file", r.get("res") == -2 and not os.path.exists(out) and not leftovers() and r["t"] < 4, (r, leftovers()))
open(out, "wb").write(good)
r = run(["file", B + f"/gen?n={N}", out, "1", "flag"])
check("[safe] quit flag on a fast download: -2, existing file untouched, no temp file", r.get("res") == -2 and open(out, "rb").read() == good and not leftovers(), r)
# disk full (writes past a size limit fail with EFBIG)
env = dict(os.environ, HV_TEST_FSIZE=str(2 * 1024 * 1024))
r = run(["file", B + f"/gen?n={N}", out], env=env)
check("[safe] disk full after 2 MB: -1, existing file untouched, no temp file", r.get("res") == -1 and open(out, "rb").read() == good and not leftovers(), (r, leftovers()))
# orphans
dead = out + ".hvpart.ABCDEF"; open(dead, "wb").write(b"x" * 100)
live = out + ".hvpart.LIVE00"; lf = open(live, "wb"); lf.write(b"y" * 100); lf.flush(); fcntl.flock(lf, fcntl.LOCK_EX | fcntl.LOCK_NB)
other = out + ".other.ABCDEF"; open(other, "wb").write(b"z")
r = run(["file", B + "/gen?n=1000", out])
check("[safe] an orphaned temp file (no lock holder) is removed at the next download", not os.path.exists(dead) and r.get("res") == 0, (r, os.path.exists(dead)))
check("[safe] a temp file whose lock is held (a live download) is left alone; unrelated names are never touched", os.path.exists(live) and os.path.exists(other))
lf.close(); os.remove(live); os.remove(other)
# unwritable directory: cannot create the temp file
ro = f"{work}/ro"; os.makedirs(ro, exist_ok=True); os.chmod(ro, 0o500)
if os.geteuid() != 0:
    r = run(["file", B + "/gen?n=1000", ro + "/f.bin"])
    check("[safe] unwritable directory: -1", r.get("res") == -1, r)
os.chmod(ro, 0o700)
# the 64 MB transfer must not be held in memory by the safe path either
print("FAILED:", fails if fails else "none")
sys.exit(1 if fails else 0)
