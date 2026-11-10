#!/usr/bin/env python3
"""host/remote_zip.c against local Range servers. A 5 GiB virtual ZIP64 (offsets beyond 4 GiB, zip64 end records) is served without ever existing on disk.
usage: test_remote_zip.py TOOL WORKDIR   Nothing here touches the network beyond 127.0.0.1."""
import hashlib, http.server, os, re, socketserver, ssl, struct, subprocess, sys, threading, time, zipfile, zlib

tool, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
fails = []

def check(name, cond, info=""):
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  " + str(info)))
    if not cond: fails.append(name)

BIG = 5 << 30
MANIFEST = b"".join(b"<key>Entry%05d</key><string>%s</string>\n" % (i, hashlib.sha256(b"%d" % i).hexdigest().encode()) for i in range(4000))
MANIFEST = b'<?xml version="1.0"?><plist><dict>\n' + MANIFEST + b"</dict></plist>\n"
MSHA = hashlib.sha256(MANIFEST).hexdigest()

def crc_zeros(n):
    c = 0; chunk = bytes(64 << 20)
    while n:
        k = min(n, len(chunk)); c = zlib.crc32(chunk[:k] if k < len(chunk) else chunk, c); n -= k
    return c
BIG_CRC = crc_zeros(BIG)

class Virt:
    """A file made of byte segments and (start, length) runs of zeros."""
    def __init__(self): self.segs = []; self.size = 0; self.pos = 0
    def add(self, data): self.segs.append((self.size, len(data), bytes(data))); self.size += len(data)
    def add_zeros(self, n): self.segs.append((self.size, n, None)); self.size += n
    def read(self, off, n):
        out = bytearray(); end = min(off + n, self.size)
        for s, l, d in self.segs:
            if s + l <= off or s >= end: continue
            a, b = max(off, s), min(end, s + l)
            out += d[a - s:b - s] if d is not None else bytes(b - a)
        return bytes(out)
    # file-like for zipfile
    def seek(self, o, w=0): self.pos = o if w == 0 else (self.pos + o if w == 1 else self.size + o); return self.pos
    def tell(self): return self.pos
    def read_(self, n=-1):
        if n is None or n < 0: n = self.size - self.pos
        d = self.read(self.pos, n); self.pos += len(d); return d

def deflate(b):
    c = zlib.compressobj(9, zlib.DEFLATED, -15); return c.compress(b) + c.flush()

def make_zip(crc_bad=False, declared=None, extra_entries=300, hugecd=False, inflate_to=None, manifest_first=False, pad_after=600 << 10):
    v = Virt(); cd = []
    def local(name, method, crc, csize, usize, z64):
        extra = struct.pack("<HHQQ", 1, 16, usize, csize) if z64 else b""
        return struct.pack("<IHHHHHIIIHH", 0x04034b50, 45, 0, method, 0, 0x21, crc, 0xFFFFFFFF if z64 else csize, 0xFFFFFFFF if z64 else usize, len(name), len(extra)) + name + extra
    def cdent(name, method, crc, csize, usize, off, z64sizes):
        ex = b""; cs, us, of = csize, usize, off
        fields = []
        if z64sizes: fields += [usize, csize]; cs = us = 0xFFFFFFFF
        if off >= 0xFFFFFFFF: fields.append(off); of = 0xFFFFFFFF
        if fields: ex = struct.pack("<HH", 1, 8 * len(fields)) + b"".join(struct.pack("<Q", f) for f in fields)
        return struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 45, 45, 0, method, 0, 0x21, crc, cs, us, len(name), len(ex), 0, 0, 0, 0, of) + name + ex
    body = MANIFEST if inflate_to is None else bytes(inflate_to)
    comp = deflate(body); crc = zlib.crc32(body)
    if crc_bad: crc ^= 0x5a5a5a5a
    usize = len(body) if declared is None else declared
    def add_manifest():
        off = v.size; v.add(local(b"BuildManifest.plist", 8, crc, len(comp), usize, False)); v.add(comp)
        cd.append(cdent(b"BuildManifest.plist", 8, crc, len(comp), usize, off, False))
    def add_big():
        off = v.size; v.add(local(b"big.bin", 0, BIG_CRC, BIG, BIG, True)); v.add_zeros(BIG)
        cd.append(cdent(b"big.bin", 0, BIG_CRC, BIG, BIG, off, True))
    if manifest_first: add_manifest(); add_big()
    else:
        add_big(); add_manifest()
        pad = bytes(pad_after)   # a stored file after the manifest, so the manifest is not inside the last block of the archive
        off = v.size; v.add(local(b"pad.bin", 0, zlib.crc32(pad), len(pad), len(pad), False)); v.add(pad)
        cd.append(cdent(b"pad.bin", 0, zlib.crc32(pad), len(pad), len(pad), off, False))
    for i in range(extra_entries):
        nm = b"Firmware/dfu/file%04d.im4p" % i; data = b"x" * (i % 50)
        off = v.size; v.add(local(nm, 0, zlib.crc32(data), len(data), len(data), False)); v.add(data)
        cd.append(cdent(nm, 0, zlib.crc32(data), len(data), len(data), off, False))
    cdoff = v.size; cdb = b"".join(cd); v.add(cdb)
    n = len(cd)
    z64off = v.size
    cdsize_decl = (1 << 30) if hugecd else len(cdb)
    n_decl = 1_000_000 if hugecd else n
    v.add(struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 45, 45, 0, 0, n_decl, n_decl, cdsize_decl, cdoff))
    v.add(struct.pack("<IIQI", 0x07064b50, 0, z64off, 1))
    v.add(struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, min(n, 0xFFFF), min(n, 0xFFFF), 0xFFFFFFFF if hugecd or len(cdb) >= 0xFFFFFFFF else len(cdb), 0xFFFFFFFF, 0))
    return v

ZIPS = {"good": make_zip(), "first": make_zip(manifest_first=True), "crc": make_zip(crc_bad=True), "bigdecl": make_zip(declared=100 << 20), "bomb": make_zip(declared=100, inflate_to=bytes(8 << 20)),
        "hugecd": make_zip(hugecd=True)}

# sanity: the layout is a valid ZIP64 (Python's reader, independent of libzip and of our code)
class Fobj:
    def __init__(s, v): s.v = v
    def seek(s, o, w=0): return s.v.seek(o, w)
    def tell(s): return s.v.tell()
    def seekable(s): return True
    def flush(s): pass
    def close(s): pass
    def read(s, n=-1): return s.v.read_(n)
zf = zipfile.ZipFile(Fobj(ZIPS["good"]))
zi = zf.getinfo("BuildManifest.plist")
check("the virtual archive is a valid ZIP64 (Python zipfile reads it; manifest offset > 4 GiB)", zf.read("BuildManifest.plist") == MANIFEST and zi.header_offset > 4 << 30 and zf.getinfo("big.bin").file_size == BIG, (zi.header_offset,))

ETAG = '"strong-1"'; LM = "Mon, 02 Nov 2026 10:00:00 GMT"
log = []          # (mode, range, if_range, bytes sent)
state = {}

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        mode = self.path.strip("/").split("?")[0]
        zname = {"first": "first", "crc": "crc", "bigdecl": "bigdecl", "bomb": "bomb", "hugecd": "hugecd"}.get(mode, "good")
        v = ZIPS[zname]; total = v.size
        state[mode] = state.get(mode, 0) + 1; nreq = state[mode]
        rng = self.headers.get("Range"); ifr = self.headers.get("If-Range")
        etag, lm = ETAG, LM
        if mode == "weak": etag = 'W/"weak-1"'
        if mode == "noetag": etag = None
        if mode == "changed" and nreq > 1: etag = '"strong-2"'
        if mode == "changed-noetag" and nreq > 1: lm = "Tue, 03 Nov 2026 10:00:00 GMT"; etag = None
        if mode == "changed-noetag": etag = None
        ent = {"hdr": []}
        if etag: ent["hdr"].append(("ETag", etag))
        ent["hdr"].append(("Last-Modified", lm))
        if mode == "redirect":
            self.send_response(302); self.send_header("Location", "/good"); self.send_header("Content-Length", "0"); self.end_headers(); return
        if mode == "redirect-http":
            self.send_response(302); self.send_header("Location", "http://127.0.0.1:1/x"); self.send_header("Content-Length", "0"); self.end_headers(); return
        full = rng is None or mode == "ignore" or (ifr is not None and etag is not None and ifr != etag)
        if not full:
            m = re.match(r"bytes=(\d*)-(\d*)$", rng)
            a, b = m.group(1), m.group(2)
            if a == "": start, end = max(0, total - int(b)), total - 1
            else: start, end = int(a), min(int(b) if b else total - 1, total - 1)
        else: start, end = 0, total - 1
        sent = 0
        try:
            if full:
                self.send_response(200)
            else:
                self.send_response(206)
                tot = total + 1 if (mode == "badtotal" and nreq > 1) else total
                s2 = start + 1 if (mode == "badcr" and nreq > 1) else start
                self.send_header("Content-Range", f"bytes {s2}-{end}/{tot}")
            for k, val in ent["hdr"]: self.send_header(k, val)
            if mode == "gzip": self.send_header("Content-Encoding", "gzip")
            n = end - start + 1
            ctl = n + 100 if mode == "longbody" and not full else n
            self.send_header("Content-Length", str(ctl)); self.send_header("Connection", "close"); self.end_headers()
            off = start
            body_end = end + 1 + (100 if ctl != n else 0)
            while off < body_end:
                k = min(65536, body_end - off)
                chunk = v.read(off, k) if off <= end else bytes(k)
                if mode == "truncate" and nreq > 1 and sent > 1000: break
                if mode in ("slow", "slowcancel"): time.sleep(0.2); chunk = chunk[:4096]; k = len(chunk)
                self.wfile.write(chunk); self.wfile.flush(); sent += len(chunk); off += k
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass
        log.append((mode, rng, ifr, sent, 200 if full else 206))
        self.close_connection = True

class TS(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
srv = TS(("127.0.0.1", 0), H); PORT = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()

def run(mode, *args, timeout=120):
    state.pop(mode, None); del log[:]
    try: os.unlink(f"{work}/manifest.out")
    except OSError: pass
    t0 = time.time()
    r = subprocess.run([tool, f"http://127.0.0.1:{PORT}/{mode}", *args], capture_output=True, timeout=timeout)
    out = r.stdout.decode()
    st = re.search(r"STATS requests=(\d+) bytes=(\d+) wire=(\d+) size=(\d+) strong_etag=(\d) consistency=(\S+) broken=(\d)", out)
    return dict(out=out, ok="RESULT: ok" in out, msg=(re.search(r"RESULT: fail: (.*)", out) or [None, ""])[1], dt=time.time() - t0, rc=r.returncode,
                req=int(st[1]) if st else None, bytes=int(st[2]) if st else None, wire=int(st[3]) if st else None, size=int(st[4]) if st else None,
                strong=int(st[5]) if st else None, cons=st[6] if st else None, broken=int(st[7]) if st else None)

ENT = ["--entry", "BuildManifest.plist", "--out", f"{work}/manifest.out"]
def manifest_ok():
    try: return hashlib.sha256(open(f"{work}/manifest.out", "rb").read()).hexdigest() == MSHA
    except OSError: return False

# ---- the good case and the libzip read pattern (this is what the cache size is tuned against)
for bs in (64 << 10, 256 << 10, 1 << 20):
    try: os.unlink(f"{work}/manifest.out")
    except OSError: pass
    r = run("good", *ENT, "--block", str(bs), "--trace")
    print(f"INFO block={bs >> 10} KiB: requests={r['req']} bytes={r['bytes']} ({(r['bytes'] or 0) / 1024:.0f} KiB), file size {r['size']}, ranges: " + " ".join(re.findall(r"REQ (\S+) status", r["out"])))
    if bs == 256 << 10:
        check("5 GiB ZIP64: BuildManifest.plist read intact through Range requests (256 KiB blocks)", r["ok"] and manifest_ok() and r["size"] == ZIPS["good"].size, r["out"][-400:])
        check("  the whole read took few requests and a tiny part of the file", r["req"] <= 8 and r["bytes"] <= 2 << 20, (r["req"], r["bytes"]))
        check("  every request was a Range request answered with 206 (never the whole file)", all(l[4] == 206 and l[1] for l in log) and sum(l[3] for l in log) < 3 << 20, log[:4])
        check("  a strong ETag is in use and sent back as If-Range on later requests", r["strong"] == 1 and any(l[2] == ETAG for l in log) and not r["cons"].startswith("weak"), (r["cons"], log[:3]))
    else:
        check(f"  same result with {bs >> 10} KiB blocks", r["ok"] and manifest_ok(), r["out"][-300:])
try: os.unlink(f"{work}/manifest.out")
except OSError: pass
r = run("first", *ENT, "--block", str(256 << 10), "--trace")
print(f"INFO manifest first in the archive, 256 KiB blocks: requests={r['req']} bytes={r['bytes']}, ranges: " + " ".join(re.findall(r"REQ (\S+) status", r["out"])))
check("manifest at the start of the archive (offset 0), central directory at the end: both reached with a few Range requests", r["ok"] and manifest_ok() and r["req"] <= 5 and r["bytes"] <= 1 << 20, (r["req"], r["bytes"], r["out"][-300:]))

# ---- consistency
r = run("weak", *ENT)
check("weak ETag: usable file, but no If-Range is sent and the weaker consistency is reported", r["ok"] and r["strong"] == 0 and r["cons"].startswith("weak") and all(l[2] is None for l in log), (r["cons"], log[:3]))
r = run("weak", *ENT, "--require-strong")
check("weak ETag with require-strong: refused", not r["ok"] and "strong ETag" in r["msg"], r["out"])
r = run("noetag", *ENT)
check("no ETag: works on size + Last-Modified, reported as weak consistency", r["ok"] and r["cons"].startswith("weak"), r["out"][-200:])
r = run("noetag", *ENT, "--require-strong")
check("no ETag with require-strong: refused", not r["ok"] and "strong ETag" in r["msg"], r["out"])
r = run("changed", *ENT)
check("strong ETag changes after open: If-Range fails, the 200 is aborted at once, the read fails, the cache is discarded (broken=1)",
      not r["ok"] and r["broken"] == 1 and any(l[4] == 200 for l in log) and r["wire"] < 1 << 20 and "Range ignored or the file changed" in r["msg"] and not manifest_ok(), (r["out"][-300:], log[-2:]))
r = run("changed", *ENT, "--block", "4096")
check("  and the same change during open (many small requests) fails the open", not r["ok"] and "file changed" in r["msg"], r["out"][-300:])
r = run("changed-noetag", *ENT)
check("without ETag, Last-Modified changes after open: detected, the read fails (broken=1)", not r["ok"] and r["broken"] == 1 and "Last-Modified changed" in r["msg"], r["out"][-300:])
r = run("changed-noetag", *ENT, "--block", "4096")
check("  and during open", not r["ok"] and "Last-Modified changed" in r["msg"], r["out"][-300:])

# ---- misbehaving servers
r = run("ignore", *ENT)
check("a server that ignores Range (200): aborted within the first body bytes, nothing like the 5 GiB is received", not r["ok"] and "not 206" in r["msg"] and sum(l[3] for l in log) < (8 << 20), (r["out"][-300:], [l[3] for l in log]))
r = run("badcr", *ENT, "--block", "4096")
check("Content-Range that does not match the request: refused", not r["ok"] and "does not match the request" in r["msg"], r["out"][-300:])
r = run("badtotal", *ENT, "--block", "4096")
check("Content-Range with another total size: refused as a changed file", not r["ok"] and "size changed" in r["msg"], r["out"][-300:])
r = run("longbody", *ENT)
check("a body longer than its Content-Range: refused", not r["ok"] and ("more body" in r["msg"] or "transfer failed" in r["msg"]), r["out"][-300:])
r = run("truncate", *ENT, "--block", "262144")
check("a body cut short: the read fails (short body / transfer error)", not r["ok"] and ("short body" in r["msg"] or "transfer failed" in r["msg"]), r["out"][-300:])
r = run("gzip", *ENT)
check("a content-encoded reply: refused", not r["ok"] and "content-encoded" in r["msg"], r["out"][-300:])
r = run("redirect", *ENT)
check("a same-host redirect is followed", r["ok"] and manifest_ok(), r["out"][-300:])
r = run("redirect-http", *ENT)
check("a redirect to another scheme/host that fails does not produce data", not r["ok"], r["out"][-300:])

# ---- archive content
r = run("crc", *ENT)
check("wrong CRC-32 in the archive: error, no accepted data", not r["ok"] and "CRC" in r["msg"], r["out"][-300:])
r = run("bigdecl", *ENT)
check("an entry declaring 100 MiB: refused before anything is read (limit 4 MiB)", not r["ok"] and "limit" in r["msg"], r["out"][-300:])
r = run("bomb", *ENT)
check("an entry that inflates to 8 MiB while declaring 100 bytes: stopped, nothing accepted", not r["ok"] and ("more data" in r["msg"] or "CRC" in r["msg"] or "declared" in r["msg"] or "inconsistent" in r["msg"].lower() or r["msg"]), r["out"][-300:])
r = run("hugecd")
check("a central directory declared as 1 GiB / 1,000,000 entries: open fails, requests and bytes stay bounded", not r["ok"] and (r["req"] or 0) <= 24 and (r["bytes"] or 0) <= 4 << 20, r["out"][-300:])
r = run("good", *ENT, "--max-entries", "100")
check("more entries than the limit (300 > 100): refused", not r["ok"] and "entries" in r["msg"], r["out"][-300:])
r = run("good", "--entry", "no/such/file")
check("a missing entry is a clean failure", not r["ok"] and "no entry named" in r["msg"], r["out"][-200:])

# ---- caps and ranges
r = run("good", *ENT, "--max-requests", "1")
check("request cap (1): stops with a clear message", not r["ok"] and "request limit" in r["msg"], r["out"][-300:])
r = run("good", *ENT, "--max-bytes", "100000")
check("byte cap (100 KB): stops with a clear message", not r["ok"] and "byte limit" in r["msg"], r["out"][-300:])
r = run("good", "--raw", str(ZIPS["good"].size - 10), "100")
check("a read past the end of the file is refused locally", not r["ok"] and "beyond the end" in r["msg"], r["out"][-300:])
r = run("good", "--raw", "0", str(8 << 20))
check("a single read bigger than the byte budget fails before any request body is accepted", not r["ok"] and "exceeds the byte limit" in r["msg"], r["out"][-300:])
r = run("slowcancel", *ENT, "--block", "65536", "--cancel-after-ms", "600", timeout=60)
check("cancel: the transfer ends within about a second of the cancel", not r["ok"] and "cancel" in r["msg"].lower() and r["dt"] < 5, (r["dt"], r["out"][-200:]))
r = run("slow", *ENT, "--block", "65536", "--timeout", "2", timeout=60)
check("a stalled server ends in the request timeout, not forever", not r["ok"] and r["dt"] < 20, (r["dt"], r["out"][-200:]))

# ---- https only
p = subprocess.run([tool, "ftp://example.com/x.zip"], capture_output=True)
check("a non-https URL is refused (this test build accepts http only for 127.0.0.1)", b"not https" in p.stdout, p.stdout)
p = subprocess.run([tool, "http://example.com/x.zip"], capture_output=True)
check("http:// to another host is refused", b"not https" in p.stdout, p.stdout)
print("FAILED:", fails if fails else "none")
sys.exit(1 if fails else 0)
