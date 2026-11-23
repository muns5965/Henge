#!/bin/bash
# g7_probe (host/g7_probe.c) as a real program on real files in a temporary directory: a valid request, every kind of refusal, bad arguments, the static symbol check, and strace
# (no file opened for writing, nothing removed/renamed/made, no network, one program start). No machine, no restore, no Apple request.
# Two builds: SHIPPED (the real constants: the stand-in ROM and a small temp disk are refused, which is itself checked) and TEST (the SHA-256 of the stand-in ROM and a small free-space floor
# are compiled in, so a valid request can reach the switch). Usage: [CC=cc] tools/g7-probe-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd); rc=0
ok() { echo "ok   $*"; }; no() { echo "FAIL $*"; rc=1; }
ROMSHA=$(python3 -c "import hashlib; print(hashlib.sha256(bytes((i*31+7)&255 for i in range(214904))).hexdigest())")
CF="-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -Icore -Ihost -Idevices"
SRC="host/g7_probe.c host/runner_g7_core.c host/g7_preflight.c host/disk_policy.c host/crypto_portable.c"
$CC $CF $SRC -Wl,--gc-sections -o "$OUT/g7_probe_ship" 2> "$OUT/ship.log" || { echo "FAILED build (shipped)"; head -20 "$OUT/ship.log"; exit 1; }
$CC $CF -DHV_G7_MIN_FREE_BYTES='(1ULL<<20)' -DHV_G7_ROM_SHA256_HEX=\"$ROMSHA\" $SRC -Wl,--gc-sections -o "$OUT/g7_probe_test" 2> "$OUT/test.log" || { echo "FAILED build (test)"; head -20 "$OUT/test.log"; exit 1; }
echo "=== static check"
for b in ship test; do tools/check-g7-probe-safe.sh "$OUT/g7_probe_$b" || rc=1; done
# the check must catch a binary that has the execution stages (control)
$CC -std=gnu11 -O1 -DHV_G7_MOCK_EXECUTION -Icore -Ihost -Idevices -x c - host/runner_g7_core.c host/g7_preflight.c host/disk_policy.c host/crypto_portable.c -o "$OUT/g7_control" 2>/dev/null <<'C'
#include "runner_g7_core.h"
int main(void){ HvG7Result r; HvG7Request q={0}; hv_g7_run(&q,0,&r); return 0; }
C
if tools/check-g7-probe-safe.sh "$OUT/g7_control" > "$OUT/control.out" 2>&1; then no "the static check passed a binary with the execution stages in it"; else ok "the static check refuses a binary with the execution stages ($(grep -c '^FAIL' "$OUT/control.out") findings)"; fi

W=$OUT/work; rm -rf "$W"; mkdir -p "$W"
mkset() { # <name>: data dir, ipsw dir, ecid dir, rom
  local d=$W/$1; rm -rf "$d"; mkdir -p "$d/data" "$d/ipsw" "$d/ecid"; chmod 700 "$d/data"
  truncate -s 16814137790 "$d/ipsw/UniversalMac_15.6.1_24G90_Restore.ipsw"
  printf 'ecid=0x13579bdf024681\n' > "$d/ecid/vm_ecid.txt"
  python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31+7)&255 for i in range(214904)))" > "$d/rom.bin"
}
args() { echo --dir "$W/$1/data" --ipsw-dir "$W/$1/ipsw" --ecid-dir "$W/$1/ecid" --rom "$W/$1/rom.bin" --ecid 13579bdf024681; }
snapdir() { (cd "$W/$1" && find . -printf '%p %y %s %T@\n' | sort); }
run() { local b=$1; shift; "$OUT/g7_probe_$b" "$@" > "$OUT/last.out" 2> "$OUT/last.err"; echo $?; }

echo "=== shipped constants"
mkset s1
r=$(run ship $(args s1)); [ "$r" = 2 ] && grep -q "BLOCK rom" "$OUT/last.out" && ok "the stand-in ROM is refused by the shipped constants (exit 2)" || no "shipped build accepted the stand-in ROM (exit $r)"

echo "=== valid request (test constants)"
mkset v1; before=$(snapdir v1)
r=$(run test $(args v1)); after=$(snapdir v1)
[ "$r" = 3 ] && grep -q "BLOCKED_BY_SWITCH" "$OUT/last.out" && ok "a valid request stops at the switch (exit 3)" || { no "valid request: exit $r"; cat "$OUT/last.out"; }
[ "$before" = "$after" ] && ok "nothing in the directories changed (names, types, sizes, mtimes)" || no "the directories changed"
grep -qiE "13579bdf024681|0x1357" "$OUT/last.out" "$OUT/last.err" && no "the ECID value appears in the output" || ok "the ECID value does not appear in the output"
[ ! -s "$OUT/last.err" ] && ok "nothing on stderr" || no "stderr: $(head -2 "$OUT/last.err")"

echo "=== refusals (each must exit 2, say which check, and change nothing)"
refuse() { # <label> <expect text> <setup...>
  local name=r$RANDOM; mkset $name; local lbl=$1 want=$2; shift 2
  ( cd "$W/$name"; eval "$@" ); before=$(snapdir $name)
  r=$(run test $(args $name)); after=$(snapdir $name)
  if [ "$r" = 2 ] && grep -q "$want" "$OUT/last.out" && [ "$before" = "$after" ]; then ok "$lbl"; else no "$lbl (exit $r, want '$want')"; tail -3 "$OUT/last.out"; fi
}
refuse "ECID differs from vm_ecid.txt" "BLOCK ecid" "printf 'ecid=0x2468ace13579bd\n' > ecid/vm_ecid.txt"
refuse "vm_ecid.txt missing" "BLOCK ecid" "rm ecid/vm_ecid.txt"
refuse "vm_ecid.txt corrupt" "BLOCK ecid" "echo junk > ecid/vm_ecid.txt"
refuse "IPSW missing" "BLOCK ipsw" "rm ipsw/*.ipsw"
refuse "IPSW one byte short" "BLOCK ipsw" "truncate -s 16814137789 ipsw/*.ipsw"
refuse "IPSW .part left" "BLOCK ipsw" "touch ipsw/UniversalMac_15.6.1_24G90_Restore.ipsw.part"
refuse "IPSW is a link" "BLOCK ipsw" "mv ipsw/*.ipsw rom_ipsw.bin; ln -s \$PWD/rom_ipsw.bin ipsw/UniversalMac_15.6.1_24G90_Restore.ipsw"
refuse "ROM with one flipped byte" "BLOCK rom" "printf '\\x00' | dd of=rom.bin bs=1 seek=1000 conv=notrunc 2>/dev/null"
refuse "ROM missing" "BLOCK rom" "rm rom.bin"
refuse "ROM is a link" "BLOCK rom" "mv rom.bin real_rom; ln -s \$PWD/real_rom rom.bin"
refuse "a disk already there" "BLOCK disk" "truncate -s 137438953472 data/macos_disk.img"
refuse "an AUX already there" "BLOCK aux" "truncate -s 33570816 data/macos_aux.img"
refuse "an interrupted pair (state file)" "BLOCK state" "echo state=in_progress > data/g7_state.txt"
refuse "a group/world-writable directory" "BLOCK directory" "chmod 777 data"

echo "=== arguments"
for t in "" "--bogus" "--dir" "--ecid 0" "--ecid zz" "--ecid -5" "--ram-mb 99999999 --dir a --ipsw-dir b --ecid-dir c --rom d --ecid 1"; do
  r=$(run test $t); [ "$r" = 64 ] && ok "bad arguments '$t' -> 64" || no "bad arguments '$t' -> $r"
done
mkset a2; r=$(run test $(args a2) --ram-mb 100); [ "$r" = 2 ] && grep -q "configuration refused" "$OUT/last.out" && ok "RAM out of range is refused at the configuration stage" || no "ram-mb 100 -> $r"

echo "=== strace on the valid run (real engine not involved: this program links none)"
if ! command -v strace >/dev/null 2>&1; then echo "strace is not installed"; [ "${REQUIRE_STRACE:-0}" = 1 ] && rc=1
else
  mkset t1
  strace -f -qq -s 200 -e trace=%file,%network,%process,%desc -o "$OUT/probe.trace" "$OUT/g7_probe_test" $(args t1) > /dev/null 2>&1; r=$?
  [ "$r" = 3 ] && ok "traced run exit 3" || no "traced run exit $r"
  w=$(grep -E '^[0-9]+ +(openat|open|creat)\(' "$OUT/probe.trace" | grep -E 'O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND' | grep -v '"/dev/' | head)
  [ -z "$w" ] && ok "no file opened for writing" || { no "a file was opened for writing:"; echo "$w"; }
  m=$(grep -E '^[0-9]+ +(unlink|unlinkat|rename|renameat|renameat2|truncate|ftruncate|mkdir|mkdirat|rmdir|symlink|symlinkat|link|linkat|pwrite64|write)\(' "$OUT/probe.trace" | grep -vE '^[0-9]+ +write\([12],' | head)
  [ -z "$m" ] && ok "nothing removed, renamed, truncated, made or written (but the two output descriptors)" || { no "file-changing calls:"; echo "$m"; }
  n=$(grep -cE '^[0-9]+ +(socket|socketpair|connect|bind|listen|accept|accept4|sendto|sendmsg|recvfrom|recvmsg)\(' "$OUT/probe.trace")
  [ "$n" = 0 ] && ok "no network call" || no "$n network calls"
  e=$(grep -cE '^[0-9]+ +execve\(' "$OUT/probe.trace")
  [ "$e" = 1 ] && ok "one program start" || no "$e program starts"
  # control: the tracer must see writes (a blind tracer proves nothing)
  strace -f -qq -e trace=%file -o "$OUT/ctl.trace" sh -c "echo x > $W/ctl.txt" > /dev/null 2>&1
  grep -qE 'O_WRONLY|O_CREAT' "$OUT/ctl.trace" && ok "control: the tracer does see a file opened for writing" || no "control: the tracer is blind"
fi
[ $rc = 0 ] && echo "G7-PROBE OK" || echo "G7-PROBE FAILED"
exit $rc
