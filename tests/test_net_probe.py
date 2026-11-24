#!/usr/bin/env python3
"""hv_curl_probe against local servers only: nothing leaves this machine. Usage: test_net_probe.py <test_net_probe binary> <workdir>"""
import os, shutil, socket, ssl, subprocess, sys, threading
exe, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
fails = 0
def check(name, cond, info=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails += 1; print("   ", info)
def mkcert(name, cn):
    key, crt, cfg = f"{work}/{name}.key", f"{work}/{name}.crt", f"{work}/{name}.cnf"
    open(cfg, "w").write(f"[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nCN={cn}\n[v3]\nsubjectAltName=DNS:{cn}\nbasicConstraints=critical,CA:TRUE\n")
    subprocess.check_call(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt, "-days", "2", "-config", cfg], stderr=subprocess.DEVNULL)
    return key, crt
seen_data = []
def serve(tls):
    c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c.load_cert_chain(tls[1], tls[0])
    s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(8)
    def loop():
        while True:
            try:
                cl, _ = s.accept()
                try:
                    t = c.wrap_socket(cl, server_side=True); t.settimeout(1)
                    try:
                        d = t.recv(100)
                        if d: seen_data.append(d)
                    except Exception: pass
                    t.close()
                except Exception: cl.close()
            except Exception: return
    threading.Thread(target=loop, daemon=True).start()
    return s.getsockname()[1]
def probe(host, port, ca):
    env = dict(os.environ); env["HV_CA_PATH"] = ca
    out = subprocess.run([exe, host, str(port)], capture_output=True, env=env, timeout=60).stdout.decode()
    return dict(kv.split("=", 1) for kv in out.split())
good = mkcert("pgood", "localhost")
ca = f"{work}/pca"; os.makedirs(ca, exist_ok=True); shutil.copy(good[1], ca + "/c.pem")
port = serve(good)
r = probe("localhost", port, ca)
check("trusted certificate: handshake done, curl 0, no verify flags", r["curl"] == "0" and r["tls"] == "1" and r["conn"] == "1" and r["vflags"] == "0x0" and r["tlsrc"] == "0", r)
check("the lookup and connect and handshake times are recorded and small", all(int(r[k]) >= 0 for k in ("dns_ms", "tcp_ms", "tls_ms")) and int(r["v4"]) + int(r["v6"]) >= 1, r)
check("nothing is written: no request sent, no status", r["sent"] == "0" and r["status"] == "0" and not seen_data, (r, seen_data))
empty = f"{work}/pempty"; os.makedirs(empty, exist_ok=True); shutil.copy(mkcert("pother", "other.example")[1], empty + "/o.pem")
r = probe("localhost", port, empty)
check("untrusted issuer: curl 60, mbedtls verify-failed code, flag 0x8 (issuer not trusted)", r["curl"] == "60" and r["tlsrc"] == "-9984" and int(r["vflags"], 16) & 0x8 and r["conn"] == "1" and r["tls"] == "0", r)
mism = mkcert("pmism", "other.example"); mca = f"{work}/pmca"; os.makedirs(mca, exist_ok=True); shutil.copy(mism[1], mca + "/m.pem")
port2 = serve(mism)
r = probe("localhost", port2, mca)
check("trusted certificate for another name: curl 60, flag 0x4 (name mismatch), connection was made", r["curl"] == "60" and int(r["vflags"], 16) & 0x4 and r["conn"] == "1", r)
s = socket.socket(); s.bind(("127.0.0.1", 0)); closed = s.getsockname()[1]; s.close()
r = probe("localhost", closed, ca)
check("closed port: curl 7, connect errno 111, one address tried, no TLS", r["curl"] == "7" and r["connerr"] == "111" and r["tried"] == "1" and r["conn"] == "0" and r["tls_ms"] == "0", r)
r = probe("nonexistent.invalid", 443, ca)
check("a name that does not exist: curl 6, a lookup code, nothing tried", r["curl"] == "6" and r["gai"] != "0" and r["tried"] == "0", r)
print("NETPROBE FAILED (%d)" % fails if fails else "NETPROBE OK (0 failed)")
sys.exit(1 if fails else 0)
