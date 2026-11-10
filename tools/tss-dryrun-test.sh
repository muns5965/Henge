#!/bin/sh
# Builds tests/tss_dryrun.c with the real libtatsu request builders and libplist, WITHOUT the send path (tss.c -DHV_TSS_NO_SEND) and without curl or TLS,
# then checks: structure OK; the binary has no HTTP/TLS/send symbols; the output shows no ECID, nonce or other value; the mirrored parameter keys are the
# ones idevicerestore.c's get_tss_response sets; a missing parameter is reported. Usage: [CC=..] tools/tss-dryrun-test.sh [outdir]
set -e
cd "$(dirname "$0")/.."
O=${1:-build-dryrun}; CC=${CC:-cc}; TP=third_party
mkdir -p "$O"
FLAGS="-O2 -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libplist/include -I$TP/libplist/libcnary/include -I$TP/libtatsu/include"
for f in $TP/libplist/src/*.c $TP/libplist/libcnary/node.c $TP/libplist/libcnary/node_list.c; do
  $CC $FLAGS -I"$(dirname "$f")" -DPACKAGE_VERSION=\"2.8.0-henge\" -c "$f" -o "$O/$(basename "$f" .c).o"
done
$CC $FLAGS -I$TP/libtatsu/src -DPACKAGE_VERSION=\"1.0.5-henge\" -DHV_TSS_NO_SEND -c $TP/libtatsu/src/tss.c -o "$O/dr_tss.o"
$CC $FLAGS -c tests/tss_dryrun.c -o "$O/dr_main.o"
$CC -o "$O/tss_dryrun" "$O/dr_main.o" "$O/dr_tss.o" $(ls $O/*.o | grep -v 'dr_') -lm
fails=0
ok() { echo "PASS $1"; }
no() { echo "FAIL $1"; fails=$((fails+1)); }

"$O/tss_dryrun" > "$O/out.txt" && ok "dry run on the synthetic identity: structure complete" || no "dry run failed: $(tail -3 "$O/out.txt")"
grep -q "SYNTHETIC" "$O/out.txt" && grep -q "NOT CHECKED: whether Apple signs" "$O/out.txt" && ok "output states it is synthetic and what it does not check" || no "output lacks the synthetic/limits statements"

# no send path, no HTTP client, no TLS in the binary
if nm "$O/tss_dryrun" | grep -Eiq 'curl|mbedtls|tss_request_send|ssl_|getaddrinfo|socket|connect$'; then no "binary references network symbols: $(nm "$O/tss_dryrun" | grep -Ei 'curl|mbedtls|tss_request_send|getaddrinfo|socket|connect$' | head -3 | tr '\n' ' ')"; else ok "binary has no curl/mbedtls/send/socket symbols"; fi
if nm "$O/dr_tss.o" | grep -q ' T tss_request_send'; then no "dr_tss.o defines tss_request_send"; else ok "tss.c built with HV_TSS_NO_SEND does not define tss_request_send"; fi
if ldd "$O/tss_dryrun" 2>/dev/null | grep -Eiq 'curl|ssl|mbedtls'; then no "binary links a network library"; else ok "binary links no network library"; fi

# no values: the ECID (dec/hex), nonce fill bytes as hex or base64, and the invented strings never appear
if grep -Eiq '1234605616436508552|1122334455667788|3333333333|MzMzMzMz|4444444444|RERERERE|synthetic/path' "$O/out.txt"; then no "output contains a value"; else ok "output contains no ECID, nonce or string value"; fi
# every line is "path  type" with no '=' or value column
if grep -Eq '=|0x[0-9a-f]{6,}' "$O/out.txt"; then no "output has value-looking text"; else ok "output lines carry only path/type/length"; fi

# a missing parameter is detected
if "$O/tss_dryrun" --omit ApNonce > "$O/out_omit.txt" 2>&1; then no "request without ApNonce was accepted"; else grep -Eq 'failed|MISSING' "$O/out_omit.txt" && ok "missing ApNonce is reported and exits non-zero" || no "no report for the missing ApNonce"; fi
if "$O/tss_dryrun" --omit ApECID > "$O/out_omit2.txt" 2>&1; then no "request without ApECID was accepted"; else grep -q 'MISSING-OR-WRONG-TYPE ApECID' "$O/out_omit2.txt" && ok "missing ApECID is reported" || no "no report for the missing ApECID"; fi

# the keys this tool sets in `parameters` are the ones get_tss_response() sets in idevicerestore.c
fn=$(awk '/^int get_tss_response\(/{f=1} f{print} f&&/^}/{exit}' third_party/idevicerestore/src/idevicerestore.c)
miss=""
for k in ApECID ApNonce ApSepNonce ApProductionMode ApSecurityMode ApSupportsImg4; do echo "$fn" | grep -q "\"$k\"" || miss="$miss $k"; done
[ -z "$fn" ] && no "get_tss_response not found in idevicerestore.c" || { [ -z "$miss" ] && ok "mirrored parameter keys all appear in get_tss_response()" || no "keys not in get_tss_response():$miss"; }

# a BuildManifest file path: chip/board comparison works on a tiny local manifest (invented here, only for the code path)
python3 - "$O/bm.plist" <<'PY'
import plistlib, sys
ident = {"UniqueBuildID": b"\x01"*20, "ApChipID": "0xfe00", "ApBoardID": "0x20", "ApSecurityDomain": "0x01",
         "Info": {}, "Manifest": {"LLB": {"Digest": b"\x02"*48, "Trusted": True, "Info": {"Path": "x"}}}}
plistlib.dump({"BuildIdentities": [ident, dict(ident, ApBoardID="0x22")]}, open(sys.argv[1], "wb"))
PY
"$O/tss_dryrun" "$O/bm.plist" 0 > "$O/out_file0.txt" || true
grep -q "chip match, board match" "$O/out_file0.txt" && ok "file mode: matching chip/board reported" || no "file mode: $(head -3 "$O/out_file0.txt")"
"$O/tss_dryrun" "$O/bm.plist" 1 > "$O/out_file1.txt" || true
grep -q "board DIFFERENT" "$O/out_file1.txt" && ok "file mode: a different board is reported as DIFFERENT" || no "file mode did not flag a different board"
echo "FAILED: $fails"
[ $fails = 0 ]
