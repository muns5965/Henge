#!/usr/bin/env python3
"""G1: a SYNTHETIC signing server on 127.0.0.1 (TLS). It signs nothing and is nothing like Apple's server: it answers with fixed, made-up bodies so that the restore tool's own
request path can be exercised offline. Usage: g1_tss_server.py DIR   -- writes DIR/cadir/ca.pem (the trusted test CA = the server certificate), DIR/cadir_other/ca.pem (an
unrelated certificate), DIR/port, then DIR/ready; appends one line per request to DIR/requests.log ("<path>"); runs until it is killed.
Paths: /ok  /refuse (STATUS=8)  /refuse2 (the first reply is fine, then STATUS=8)  /bad (no STATUS)  /slow (silent for 30 s)  /close (TLS handshake, then closes)
       /redirect (302 to plain http)"""
import os, re, shutil, socket, ssl, subprocess, sys, threading, time

d = sys.argv[1]
os.makedirs(d, exist_ok=True)

def mkcert(name):
    key, crt, cfg = f"{d}/{name}.key", f"{d}/{name}.crt", f"{d}/{name}.cnf"
    open(cfg, "w").write("[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nCN=127.0.0.1\n[v3]\nsubjectAltName=IP:127.0.0.1\nbasicConstraints=critical,CA:TRUE\n"
                         "keyUsage=critical,keyCertSign,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
    subprocess.check_call(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt, "-days", "2", "-config", cfg], stderr=subprocess.DEVNULL)
    return key, crt

good, other = mkcert("good"), mkcert("other")
for name, c in (("cadir", good), ("cadir_other", other)):
    os.makedirs(f"{d}/{name}", exist_ok=True)
    shutil.copy(c[1], f"{d}/{name}/ca.pem")

BODY = (b"STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING=<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        b"\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist version=\"1.0\"><dict><key>ApImg4Ticket</key><data>c3ludGhldGljLXRpY2tldA==</data></dict></plist>")

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(good[1], good[0])
sock = socket.socket()
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", 0))
sock.listen(64)
port = sock.getsockname()[1]
counter = {}
lock = threading.Lock()

def reply(c, status, body, extra=b"", reason=b"OK"):
    c.sendall(b"HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Length: %d\r\n%s\r\n" % (status, reason, len(body), extra) + body)

def serve(c):
    try:
        c.settimeout(40)
        c = ctx.wrap_socket(c, server_side=True)
        buf = b""
        while b"\r\n\r\n" not in buf:
            x = c.recv(4096)
            if not x: return
            buf += x
        head, _, rest = buf.partition(b"\r\n\r\n")
        path = head.split(b" ")[1].decode().split("?")[0]
        m = re.search(rb"(?i)content-length:\s*(\d+)", head); n = int(m.group(1)) if m else 0
        while len(rest) < n:
            x = c.recv(4096)
            if not x: break
            rest += x
        with lock:
            counter[path] = counter.get(path, 0) + 1
            no = counter[path]
            with open(f"{d}/requests.log", "a") as f: f.write(path + "\n")
        if path == "/ok": reply(c, 200, BODY)
        elif path == "/refuse": reply(c, 200, b"STATUS=8&MESSAGE=refused")
        elif path == "/refuse2": reply(c, 200, BODY if no == 1 else b"STATUS=8&MESSAGE=refused")
        elif path == "/bad": reply(c, 200, b"not a signing reply")
        elif path == "/slow": time.sleep(30)
        elif path == "/close": pass
        elif path == "/redirect": reply(c, 302, b"", b"Location: http://127.0.0.1:%d/ok\r\n" % port, reason=b"Found")
        else: reply(c, 404, b"")
    except Exception:
        pass
    finally:
        try: c.close()
        except Exception: pass

open(f"{d}/requests.log", "w").close()
open(f"{d}/port", "w").write(str(port))
open(f"{d}/ready", "w").write("1")
while True:
    try: c, _ = sock.accept()
    except OSError: break
    threading.Thread(target=serve, args=(c,), daemon=True).start()
