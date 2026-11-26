#!/bin/bash
# The frame-file writer of host/vm.c (PNG from a BGRA frame, bounded count/size, worker thread, flush) on this machine: the region between FRAME-FILES-BEGIN and FRAME-FILES-END is cut out of the
# source and compiled with a test main; the PNGs it writes are decoded independently (python zlib) and compared with the pixels that were given. Usage: tools/g7-frames-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; rm -rf "$OUT"; mkdir -p "$OUT/png"
sed -n '/FRAME-FILES-BEGIN/,/FRAME-FILES-END/p' host/vm.c > "$OUT/frames_region.inc"
[ "$(wc -l < "$OUT/frames_region.inc")" -gt 40 ] || { echo "FAILED: the FRAME-FILES region is missing from host/vm.c"; exit 1; }
${CC:-gcc} -std=gnu11 -O1 -Wall -Wextra -Werror -I"$OUT" -o "$OUT/t" tests/test_g7_frames.c -lpthread 2> "$OUT/cc.log" || { echo "FAILED compile"; head -30 "$OUT/cc.log"; exit 1; }
"$OUT/t" "$OUT/png"; R1=$?
python3 -I - "$OUT/png" <<'P'
import sys, zlib, struct
d = sys.argv[1]
def png(path):
    b = open(path, "rb").read()
    assert b[:8] == b"\x89PNG\r\n\x1a\n", "signature"
    i = 8; idat = b""; ihdr = None; end = False
    while i < len(b):
        n, = struct.unpack(">I", b[i:i+4]); t = b[i+4:i+8]; data = b[i+8:i+8+n]; crc, = struct.unpack(">I", b[i+8+n:i+12+n])
        assert zlib.crc32(t + data) & 0xffffffff == crc, "crc of " + t.decode()
        if t == b"IHDR": ihdr = struct.unpack(">IIBBBBB", data)
        elif t == b"IDAT": idat += data
        elif t == b"IEND": end = True
        i += 12 + n
    assert end and i == len(b), "IEND last, no trailing bytes"
    w, h, depth, ctype, _, _, _ = ihdr; assert (depth, ctype) == (8, 2)
    raw = zlib.decompress(idat); assert len(raw) == h * (1 + 3 * w)
    return w, h, raw
ok = True
w, h, raw = png(d + "/g7_frame_small_37x23.png")
for y in range(h):
    assert raw[y * (1 + 3 * w)] == 0
    for x in range(w):
        o = y * (1 + 3 * w) + 1 + 3 * x
        assert tuple(raw[o:o+3]) == ((x + y) & 255, (y * 11) & 255, (x * 7) & 255), (x, y)
print("PASS small PNG: valid CRCs, zlib stream, pixels equal the given BGRA (as RGB), the writer's copy was not changed by the caller afterwards")
w, h, raw = png(d + "/g7_frame_big_1920x1080.png")
for i in (0, 12345, 1920 * 1080 - 1):
    y, x = divmod(i, w); o = y * (1 + 3 * w) + 1 + 3 * x
    assert tuple(raw[o:o+3]) == ((i >> 16) & 255, (i >> 8) & 255, i & 255), i
print("PASS 1920x1080 PNG: valid, sampled pixels equal")
P
R2=$?
[ $R1 = 0 ] && [ $R2 = 0 ] && echo "G7FRAMES TEST OK" || { echo "G7FRAMES TEST FAILED ($R1 $R2)"; exit 1; }
