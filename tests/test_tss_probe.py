#!/usr/bin/env python3
"""host/tss_probe_core.c against a mock IPSW (Range server) and mock TSS servers on 127.0.0.1. usage: test_tss_probe.py TOOL WORKDIR
Checks: exactly the right identity is used, ONE attempt whatever goes wrong, nothing is sent when anything before the send fails,
and no ECID / nonce / ticket value ever appears in the output."""
import base64, http.server, io, os, plistlib, re, socket, socketserver, subprocess, sys, threading, time, zipfile

tool, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
fails = []
def check(name, cond, info=""):
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  " + str(info)))
    if not cond: fails.append(name)

ECID = 0x2d2a8d94f32ba4d2
def comp(i): return {"Digest": bytes([i]) * 48, "Trusted": True, "Info": {"Path": f"x/{i}"}}
def ident(chip, board, variant, tag):
    return {"UniqueBuildID": bytes([tag]) * 20, "ApChipID": chip, "ApBoardID": board, "ApSecurityDomain": "0x01", "Info": {"Variant": variant, "DeviceClass": "vma2macosap"},
            "Manifest": {"LLB": comp(tag), "iBoot": comp(tag + 1)}}
IDS = [ident("0x6022", "0x8", "Customer Erase Install (IPSW)", 1), ident("0xfe00", "0x20", "Customer Erase Install (IPSW)", 2),
       ident("0xfe00", "0x20", "Customer Upgrade Install (IPSW)", 3), ident("0xfe00", "0x20", "macOS Customer", 4)]
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
    zf.writestr("BuildManifest.plist", plistlib.dumps({"BuildIdentities": IDS}))
    for i in range(30): zf.writestr(f"Firmware/f{i}.im4p", b"x" * 1000 * (i % 5 + 1))
ZIP = buf.getvalue()

tss_seen = []      # request bodies
ipsw_mode = {"v": "ok"}
class Ipsw(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        m = re.match(r"bytes=(\d*)-(\d*)$", self.headers.get("Range", ""))
        mode = ipsw_mode["v"]
        if not m or mode == "ignore":
            self.send_response(200); self.send_header("Content-Length", str(len(ZIP))); self.send_header("ETag", '"e1"'); self.end_headers(); self.wfile.write(ZIP); self.close_connection = True; return
        a, b = m.groups(); t = len(ZIP)
        s, e = (max(0, t - int(b)), t - 1) if a == "" else (int(a), min(int(b) if b else t - 1, t - 1))
        self.send_response(206); self.send_header("Content-Range", f"bytes {s}-{e}/{t}")
        if mode != "noetag": self.send_header("ETag", '"e1"')
        self.send_header("Last-Modified", "Mon, 02 Nov 2026 10:00:00 GMT"); self.send_header("Content-Length", str(e - s + 1)); self.end_headers()
        self.wfile.write(ZIP[s:e + 1]); self.close_connection = True

TICKET = b"\x01\x02\x03\x04TICKETBYTES"
RESP = plistlib.dumps({"ApImg4Ticket": TICKET, "Other": "x"})
class Tss(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        tss_seen.append((self.path, body))
        code = 200
        if self.path.startswith("/ok"): out = b"STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING=" + RESP
        elif self.path.startswith("/rej94"): out = b"STATUS=94&MESSAGE=This device isn't eligible for the requested build."
        elif self.path.startswith("/rej8"): out = b"STATUS=8&MESSAGE=An internal error occurred 2d2a8d94f32ba4d2 for ECID 3274619340111111111"
        elif self.path.startswith("/garbage"): out = b"this is not a TSS reply"
        elif self.path.startswith("/http500"): out = b"<html>Internal Server Error</html>"; code = 500
        elif self.path.startswith("/badplist"): out = b"STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING=not xml"
        elif self.path.startswith("/hang"): time.sleep(8); out = b"late"
        else: out = b"STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING=" + RESP
        try:
            self.send_response(code); self.send_header("Content-Length", str(len(out))); self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(out)
        except OSError: pass
        self.close_connection = True

class TS(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True
def serve(h):
    s = TS(("127.0.0.1", 0), h); threading.Thread(target=s.serve_forever, daemon=True).start(); return s.server_address[1]
PI, PT = serve(Ipsw), serve(Tss)
IPSW = f"http://127.0.0.1:{PI}/x.ipsw"
def tss(path): return f"http://127.0.0.1:{PT}/{path}"

def run(url, *args, ipsw=IPSW, timeout=60):
    del tss_seen[:]
    t0 = time.time()
    r = subprocess.run([tool, ipsw, url, *args], capture_output=True, timeout=timeout)
    out = r.stdout.decode() + r.stderr.decode()
    return out, r.returncode, time.time() - t0

SECRET = [str(ECID), f"{ECID:x}", f"{ECID:X}", "5c" * 6, "6d" * 6, base64.b64encode(bytes([0x5c]) * 32).decode()[:12], base64.b64encode(TICKET).decode()[:8], "TICKETBYTES", "3274619340111111111"]
def clean(out): return not any(x in out for x in SECRET)

out, rc, _ = run(tss("ok"))
check("success: a ticket is reported, exit 0, the TSS server saw exactly one request", rc == 0 and "ticket issued" in out and len(tss_seen) == 1, (rc, out[-500:], len(tss_seen)))
body = tss_seen[0][1] if tss_seen else b""
pl = plistlib.loads(body) if body else {}
check("the request carries identity 2's components (LLB/iBoot digests of tag 2/3), its UniqueBuildID, ApECID and both nonces, and the Img4 flag",
      pl.get("LLB", {}).get("Digest") == bytes([2]) * 48 and pl.get("iBoot", {}).get("Digest") == bytes([3]) * 48 and pl.get("UniqueBuildID") == bytes([2]) * 20
      and pl.get("ApECID") == ECID and pl.get("ApNonce") == bytes([0x5c]) * 32 and pl.get("SepNonce") == bytes([0x6d]) * 20 and pl.get("@ApImg4Ticket") is True, {k: (type(v).__name__) for k, v in pl.items()})
check("  no ECID, nonce or ticket value in the output", clean(out), out)
check("  the Range reads stayed inside the approved limits (<= 24 requests, <= 4 MiB)", re.search(r"manifest read: \d+ bytes inflated, (\d+) requests, (\d+) bytes", out) and int(re.search(r"(\d+) requests", out)[1]) <= 24 and int(re.search(r", (\d+) bytes received", out)[1]) <= 4 << 20, out[:400])

out, rc, _ = run(tss("rej94"))
check("a refusal (STATUS 94): exit 1, one attempt, status and message in the account, classified as an answer from the server", rc == 1 and len(tss_seen) == 1 and "STATUS=94" in out and "answered and refused" in out, (rc, out[-400:], len(tss_seen)))
out, rc, _ = run(tss("rej8"))
check("a message that contains the ECID in decimal and hex: the output masks it", rc == 1 and clean(out) and len(tss_seen) == 1, out[-400:])
out, rc, dt = run(tss("garbage"))
check("a reply that is not a TSS answer: one attempt", rc == 1 and len(tss_seen) == 1 and "without STATUS" in out and dt < 8, (rc, len(tss_seen), dt, out[-300:]))
out, rc, dt = run(tss("http500"))
check("HTTP 500: one attempt, the HTTP status is in the account", rc == 1 and len(tss_seen) == 1 and "HTTP 500" in out, (rc, len(tss_seen), out[-300:]))
out, rc, dt = run(tss("badplist"))
check("SUCCESS but not a plist: one attempt, no ticket claimed", rc == 1 and len(tss_seen) == 1 and "ticket issued" not in out, (rc, len(tss_seen), out[-300:]))
out, rc, dt = run(tss("hang"), "--timeout", "2")
check("a server that never answers: one attempt, ends at the timeout, classified as transport", rc == 1 and dt < 7 and "transport" in out, (rc, dt, out[-300:]))
sk = socket.socket(); sk.bind(("127.0.0.1", 0)); dead = sk.getsockname()[1]; sk.close()
out, rc, dt = run(f"http://127.0.0.1:{dead}/ok")
check("connection refused: one attempt (no 5-fold retry), classified as transport", rc == 1 and dt < 6 and "transport" in out, (rc, dt, out[-300:]))

# nothing is sent when anything before the send is wrong
out, rc, _ = run(tss("ok"), "--variant", "Customer Does Not Exist")
check("no identity with that variant: exit 2, nothing sent", rc == 2 and len(tss_seen) == 0 and "no build identity" in out, (rc, out[-300:], len(tss_seen)))
out, rc, _ = run(tss("ok"), "--nononce")
check("no nonce: exit 2, nothing sent", rc == 2 and len(tss_seen) == 0, (rc, out[-300:]))
out, rc, _ = run(tss("ok"), "--nosend")
check("send disabled: the request is built and checked, nothing is sent", rc == 1 and len(tss_seen) == 0 and "request built by libtatsu" in out and "send disabled" in out, (rc, out[-300:]))
ipsw_mode["v"] = "noetag"
out, rc, _ = run(tss("ok"))
check("IPSW server without a strong ETag: manifest refused, nothing sent", rc == 2 and len(tss_seen) == 0 and "strong ETag" in out, (rc, out[-300:]))
ipsw_mode["v"] = "ignore"
out, rc, _ = run(tss("ok"))
check("IPSW server that ignores Range: manifest refused, nothing sent", rc == 2 and len(tss_seen) == 0, (rc, out[-300:]))
ipsw_mode["v"] = "ok"
out, rc, _ = run(tss("ok"), ipsw="http://example.com/x.ipsw")
check("a non-https IPSW URL (outside loopback): refused, nothing sent", rc == 2 and len(tss_seen) == 0, (rc, out[-300:]))
print("FAILED:", fails if fails else "none")
sys.exit(1 if fails else 0)
