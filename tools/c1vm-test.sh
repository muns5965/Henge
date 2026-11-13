#!/bin/bash
# C1-2: the VM runner (host/ui_runner_vm.c) and its core (host/runner_vm_core.c). Two parts.
#
# PART A, any Linux, no engine: the core with a recorder in place of the machine (tests/test_c1vm.c), and the whole runner as a real process on a FAKE machine
#   (tests/vm_fake.c: no guest, nothing of the engine) driven by tests/test_c1vm_proc.c. Plain, ASan + UBSan, TSan. This tests our code (the command rules, the picture and
#   pointer path, the settings, the HV_* scrub, the loop, every way of stopping, the exit codes). It says NOTHING about the engine.
#
# PART B, only when an engine build dir is given and the machine is arm64 (the engine runs arm64 code natively): the runner built against the REAL engine
#   (tools/ui-runner-vm-build.sh, with the static safety check), the process test against it with a stand-in ROM, and the dynamic safety check: the same runner under strace,
#   which must show no network call, one program start, the disk and the auxiliary storage opened read-only, and no file opened for writing but the shared file.
#   The sanitizers are NOT used for Part B (the JIT and the sanitizer runtimes are not known to coexist); that limit is reported, not hidden.
#   A control program that does exactly the forbidden things is traced and analysed first; if the analysis does not flag it, Part B fails (a blind tracer proves nothing).
#
# Usage: [CC=cc] [ENGINE_DIR=<dir after engine-build.sh + devices-build.sh>] [EXTRA_LIBS=...] [REQUIRE_STRACE=1] tools/c1vm-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; export CC
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rc=0
INC="-Icore -Ihost -Idevices"
W="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-result -g"

echo "######## PART A: core with a recorder; runner on a fake machine"
for mode in ${MODES:-plain asan tsan}; do
  case $mode in
    plain) F="-O1";;
    asan)  F="-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined";;
    tsan)  F="-O1 -fsanitize=thread";;
  esac
  echo "=== c1vm $mode"
  if ! $CC $W $F $INC tests/test_c1vm.c host/runner_vm_core.c host/frame_shm.c host/display_buf.c host/disk.c -o "$OUT/c1vm_$mode" -lm -lpthread 2> "$OUT/c1vm_$mode.build.log"; then
    echo "FAILED build of the core test ($mode)"; head -20 "$OUT/c1vm_$mode.build.log"; rc=1; continue
  fi
  if ! $CC $W $F $INC host/ui_runner_vm.c host/runner_vm_core.c host/frame_shm.c host/display_buf.c tests/vm_fake.c -o "$OUT/runner_fake_$mode" -lm -lpthread 2> "$OUT/runner_fake_$mode.build.log"; then
    echo "FAILED build of the runner on the fake machine ($mode)"; head -20 "$OUT/runner_fake_$mode.build.log"; rc=1; continue
  fi
  if ! $CC $W $F -Ihost tests/test_c1vm_proc.c host/frame_shm.c -o "$OUT/c1vm_proc_$mode" -lpthread 2> "$OUT/c1vm_proc_$mode.build.log"; then
    echo "FAILED build of the process test ($mode)"; head -20 "$OUT/c1vm_proc_$mode.build.log"; rc=1; continue
  fi
  if ! timeout 300 "$OUT/c1vm_$mode" > "$OUT/c1vm_$mode.out" 2> "$OUT/c1vm_$mode.err"; then
    echo "FAILED core test ($mode)"; grep -E "^FAIL" "$OUT/c1vm_$mode.out" | head -30; head -30 "$OUT/c1vm_$mode.err"; rc=1; continue
  fi
  tail -1 "$OUT/c1vm_$mode.out"
  [ -s "$OUT/c1vm_$mode.err" ] && { echo "unexpected stderr output (core test, $mode)"; head -10 "$OUT/c1vm_$mode.err"; rc=1; }
  if ! timeout 600 "$OUT/c1vm_proc_$mode" "$OUT/runner_fake_$mode" > "$OUT/c1vm_proc_$mode.out" 2> "$OUT/c1vm_proc_$mode.err"; then
    echo "FAILED process test on the fake machine ($mode)"; grep -E "^(FAIL|--)" "$OUT/c1vm_proc_$mode.out" | head -40; head -30 "$OUT/c1vm_proc_$mode.err"; rc=1; continue
  fi
  tail -1 "$OUT/c1vm_proc_$mode.out"
  [ -s "$OUT/c1vm_proc_$mode.err" ] && { echo "unexpected stderr output (process test, $mode)"; head -10 "$OUT/c1vm_proc_$mode.err"; rc=1; }
done

# the static check on the fake build must FAIL, and only because of the marker (the check works and the fake cannot be shipped by mistake)
if [ -x "$OUT/runner_fake_plain" ]; then
  if tools/check-runner-vm-safe.sh "$OUT/runner_fake_plain" > "$OUT/static_on_fake.out" 2>&1; then echo "FAILED the static check passed a binary with the fake machine in it"; rc=1
  elif grep -q "fake machine is in this binary" "$OUT/static_on_fake.out"; then echo "ok  the static check refuses a binary with the fake machine in it"
  else echo "FAILED the static check failed for another reason:"; cat "$OUT/static_on_fake.out"; rc=1; fi
fi

# ---------------------------------------------------------------- strace helpers
# analyse <trace> <shm path> <disk> <aux>   prints one line per finding group and returns the number of VIOLATIONS
analyse() {
  local t=$1 shm=$2 disk=$3 aux=$4 v=0 n
  n=$(grep -cE '^[0-9]+ +(socket|socketpair|connect|bind|listen|accept|accept4|sendto|sendmsg|sendmmsg|recvfrom|recvmsg|recvmmsg)\(' "$t")
  echo "   network calls: $n"; [ "$n" = 0 ] || v=$((v + n))
  n=$(grep -cE '^[0-9]+ +execve\(' "$t" | tr -d ' ')
  echo "   program starts (execve): $n"; [ "$n" -le 1 ] || v=$((v + n - 1))
  local wr; wr=$(grep -E '^[0-9]+ +(openat|open|creat)\(' "$t" | grep -E 'O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND|[0-9]+ +creat\(' | sed -n 's/^[0-9]* *[a-z]*(\(AT_FDCWD, \)\{0,1\}"\([^"]*\)".*/\2/p' | sort -u)
  local other; other=$(echo "$wr" | grep -vxF "$shm" | grep -v '^$')
  echo "   paths opened for writing: $(echo "$wr" | grep -vc '^$') ($(echo "$wr" | tr '\n' ' '| sed "s|$(dirname "$shm")/||g"))"
  [ -z "$other" ] || { echo "   VIOLATION: writable open of: $other"; v=$((v + $(echo "$other" | wc -l))); }
  local p bad
  for p in "$disk" "$aux"; do
    n=$(grep -E '^[0-9]+ +(openat|open)\(' "$t" | grep -cF "\"$p\"")
    bad=$(grep -E '^[0-9]+ +(openat|open)\(' "$t" | grep -F "\"$p\"" | grep -cE 'O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND')
    echo "   $(basename "$p"): opened $n time(s), with write access $bad"
    [ "$n" -ge 1 ] || { echo "   VIOLATION: $(basename "$p") never opened (the run did not reach the machine?)"; v=$((v + 1)); }
    [ "$bad" = 0 ] || v=$((v + bad))
  done
  local ch; ch=$(grep -E '^[0-9]+ +(unlink|unlinkat|rename|renameat|renameat2|truncate|mkdir|mkdirat|rmdir|symlink|symlinkat|link|linkat)\(' "$t" | sed -n 's/^[0-9]* *[a-z0-9]*(\(AT_FDCWD, \)\{0,1\}"\([^"]*\)".*/\2/p' | sort -u | grep -vxF "$shm" | grep -v '^$')
  [ -z "$ch" ] || { echo "   VIOLATION: file removed, renamed or made: $ch"; v=$((v + $(echo "$ch" | wc -l))); }
  return $v
}
strace_class() { if strace -e trace=%network true >/dev/null 2>&1; then echo "%network"; else echo "network"; fi; }

echo "######## PART B: the real engine (arm64 only)"
if [ -z "${ENGINE_DIR:-}" ]; then echo "not run: no ENGINE_DIR given"
elif [ "$(uname -m)" != "aarch64" ] && [ "$(uname -m)" != "arm64" ]; then echo "not run: this machine is $(uname -m), the engine runs arm64 code natively"
else
  E=$(cd "$ENGINE_DIR" && pwd)
  if ! tools/ui-runner-vm-build.sh "$E" "$OUT"; then echo "FAILED build / static check of the runner against the engine"; rc=1
  else
    echo "=== process test against the real engine (stand-in ROM, blank disks; plain build, no sanitizer)"
    if ! timeout 900 "$OUT/c1vm_proc_plain" "$OUT/ui_runner_vm" > "$OUT/c1vm_proc_real.out" 2> "$OUT/c1vm_proc_real.err"; then
      echo "FAILED process test against the real engine"; grep -E "^(FAIL|--)" "$OUT/c1vm_proc_real.out" | head -40; head -30 "$OUT/c1vm_proc_real.err"; rc=1
    else tail -1 "$OUT/c1vm_proc_real.out"; [ -s "$OUT/c1vm_proc_real.err" ] && { echo "unexpected stderr output"; head -10 "$OUT/c1vm_proc_real.err"; rc=1; }; fi

    echo "=== strace: what the real runner does to the network and to files"
    if ! command -v strace >/dev/null 2>&1; then
      echo "strace is not installed"; [ "${REQUIRE_STRACE:-0}" = 1 ] && rc=1
    else
      T=$OUT/strace; rm -rf "$T"; mkdir -p "$T"
      NETCLS=$(strace_class)
      TRACE="-f -qq -s 300 -e trace=$NETCLS,execve,open,openat,creat,unlink,unlinkat,rename,renameat,renameat2,truncate,mkdir,mkdirat,rmdir,symlink,symlinkat,link,linkat"
      $CC -O1 -o "$T/control" tests/strace_control.c 2> "$T/control.build.log" || { echo "FAILED build of the strace control"; head "$T/control.build.log"; rc=1; }
      if [ -x "$T/control" ]; then
        # the control: must be flagged (network, a writable path that is not the allowed one, an unlink, a second program)
        strace $TRACE -o "$T/control.trace" "$T/control" "$T/control_file" > /dev/null 2>&1
        echo "-- control program (must be flagged):"
        analyse "$T/control.trace" "$T/not_the_shm" "$T/nodisk" "$T/noaux" > "$T/control.analysis" 2>&1; cr=$?
        grep -v "never opened" "$T/control.analysis" | head -12
        if [ $cr -ge 3 ] && grep -qE 'network calls: [1-9]' "$T/control.analysis" && grep -q 'VIOLATION: writable open' "$T/control.analysis"; then echo "ok  the analysis sees network calls and a writable open (control flagged: $cr)"
        else echo "FAILED the control was not flagged: the tracer or the analysis is blind"; rc=1; fi
        # the runner
        printf '%s\n' "make files"
        D=$T/files; mkdir -p "$D"
        python3 -c "import struct,sys; sys.stdout.buffer.write(struct.pack('<I',0x14000000)*16384)" > "$D/rom.bin"
        truncate -s 16M "$D/blank_disk.img"
        python3 - "$D/blank_aux.img" <<'PY'
import sys
hd = bytes([0xa5,0xd5,0xb3,0xfe,0x97,0xad,0x1d,0x27,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0])
with open(sys.argv[1],'wb') as f:
    f.write(hd); f.truncate(33570816)
PY
        sha256sum "$D/blank_disk.img" "$D/blank_aux.img" > "$D/before.sha"
        ( sleep 7 | strace $TRACE -o "$T/runner.trace" "$OUT/ui_runner_vm" --shm "$D/ui.shm" --rom "$D/rom.bin" --disk "$D/blank_disk.img" --aux "$D/blank_aux.img" --ram-mb 256 --jit-mb 64 --cpus 2 --seconds 4 > "$T/runner.out" 2>&1 )
        grep -E "RESULT" "$T/runner.out"
        grep -q "RESULT: stopped, confirmed" "$T/runner.out" || { echo "FAILED the traced run did not stop confirmed"; head -20 "$T/runner.out"; rc=1; }
        echo "-- the real runner under strace:"
        analyse "$T/runner.trace" "$D/ui.shm" "$D/blank_disk.img" "$D/blank_aux.img" > "$T/runner.analysis" 2>&1; ar=$?
        cat "$T/runner.analysis"
        if [ $ar = 0 ]; then echo "ok  no network call, one program start, disk and auxiliary storage opened read-only, nothing but the shared file opened for writing"
        else echo "FAILED the dynamic safety check found $ar violation(s)"; rc=1; fi
        sha256sum -c "$D/before.sha" > "$T/after.out" 2>&1 && echo "ok  the disk image and the auxiliary storage still have the checksums they had before the run" || { echo "FAILED the disk files changed:"; cat "$T/after.out"; rc=1; }
      fi
    fi
  fi
fi

[ $rc = 0 ] && echo "C1-2 OK" || echo "C1-2 FAILED"
exit $rc
