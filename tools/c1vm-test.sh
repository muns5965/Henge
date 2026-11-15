#!/bin/bash
# C1-2: the VM runner (host/ui_runner_vm.c) and its core (host/runner_vm_core.c). Two parts.
#
# PART A, any Linux, no engine: the core with a recorder in place of the machine (tests/test_c1vm.c), and the whole runner as a real process on a FAKE machine
#   (tests/vm_fake.c: no guest, nothing of the engine) driven by tests/test_c1vm_proc.c. Plain, ASan + UBSan, TSan. This tests our code (the command rules, the picture and
#   pointer path, the settings, the HV_* scrub, the loop, every way of stopping, the exit codes). It says NOTHING about the engine.
#
# PART B, only when an engine build dir is given and the machine is arm64 (the engine runs arm64 code natively): the runner built against the REAL engine
#   (tools/ui-runner-vm-build.sh, with the static safety check: forbidden symbols and strings, a shared-library allow-list and source rules; NOT a complete import allow-list), the process test against it with a stand-in ROM, and the dynamic safety check: the same runner under strace,
#   which must show no network call, one program start, the disk and the auxiliary storage opened read-only, and no file opened for writing but the shared file. Once for a timed
#   run with no input and once for the process test's scenario with input commands pushed through the ring. Both only describe the paths those runs took.
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

FILTER_BASE="execve,open,openat,creat,unlink,unlinkat,rename,renameat,renameat2,truncate,mkdir,mkdirat,rmdir,symlink,symlinkat,link,linkat"
traced_input_run() {   # <runner> <label>: the process test's life scenario (13 input commands through the ring) with the runner under strace, then the analysis
  local R=$1 L=$2; local D=$T/driven_$L
  rm -rf "$D"; mkdir -p "$D"
  if ! C1VM_DIR="$D" C1VM_TRACE_OUT="$T/driven_$L.trace" C1VM_TRACE_FILTER="$(strace_class),$FILTER_BASE" "$OUT/c1vm_proc_plain" "$R" > "$T/driven_$L.out" 2>&1; then
    echo "FAILED the traced scenario with input ($L)"; grep -E "^(FAIL|--)" "$T/driven_$L.out" | head -20; return 1
  fi
  tail -1 "$T/driven_$L.out"
  analyse "$T/driven_$L.trace" "$D/ui.shm" "$D/blank_disk.img" "$D/blank_aux.img" > "$T/driven_$L.analysis" 2>&1; local ar=$?
  cat "$T/driven_$L.analysis"
  if [ $ar = 0 ]; then echo "ok  [$L, with input] no network call, one program start, disk and auxiliary storage opened read-only, nothing but the shared file opened for writing"; return 0
  else echo "FAILED [$L, with input] the dynamic safety check found $ar violation(s)"; return 1; fi
}
T=$OUT/strace; mkdir -p "$T"
if [ -x "$OUT/runner_fake_plain" ] && command -v strace >/dev/null 2>&1; then
  echo "=== strace over the fake-machine runner with input (checks the analysis and the traced scenario; says nothing about the engine)"
  traced_input_run "$OUT/runner_fake_plain" fake || rc=1
fi

# the static check on the fake build must FAIL, and only because of the marker (the check works and the fake cannot be shipped by mistake)
if [ -x "$OUT/runner_fake_plain" ]; then
  if tools/check-runner-vm-safe.sh "$OUT/runner_fake_plain" > "$OUT/static_on_fake.out" 2>&1; then echo "FAILED the static check passed a binary with the fake machine in it"; rc=1
  elif grep -q "fake machine is in this binary" "$OUT/static_on_fake.out"; then echo "ok  the static check refuses a binary with the fake machine in it"
  else echo "FAILED the static check failed for another reason:"; cat "$OUT/static_on_fake.out"; rc=1; fi
fi

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
      NETCLS=$(strace_class)
      TRACE="-f -qq -s 300 -e trace=$NETCLS,$FILTER_BASE"
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
        echo "-- the real runner under strace, with input commands:"
        traced_input_run "$OUT/ui_runner_vm" real || rc=1
        sha256sum -c "$D/before.sha" > "$T/after.out" 2>&1 && echo "ok  the disk image and the auxiliary storage still have the checksums they had before the run" || { echo "FAILED the disk files changed:"; cat "$T/after.out"; rc=1; }

        echo "=== frame probe: a picture the real engine actually drew, carried through to the shared file"
        if ! tools/build-frame-probe-rom.sh "$T/frame_probe.bin" > "$T/frame_probe_build.out" 2>&1; then
          echo "FAILED building the frame-probe stand-in ROM"; cat "$T/frame_probe_build.out"; rc=1
        elif ! $CC $W -Ihost -o "$T/frame_probe_check" tests/frame_probe_check.c host/frame_shm.c 2> "$T/frame_probe_check.build.log"; then
          echo "FAILED building frame_probe_check"; head -20 "$T/frame_probe_check.build.log"; rc=1
        else
          FP=$T/frameprobe; rm -rf "$FP"; mkdir -p "$FP"
          truncate -s 16M "$FP/blank_disk.img"
          python3 - "$FP/blank_aux.img" <<'PY'
import sys
hd = bytes([0xa5,0xd5,0xb3,0xfe,0x97,0xad,0x1d,0x27,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0])
with open(sys.argv[1],'wb') as f:
    f.write(hd); f.truncate(33570816)
PY
          # stdin must stay open past the run, or ui_runner_vm reads EOF as "input closed" and stops before pumping a
          # single frame (the --seconds timer is what should end this run, the same way the strace helpers above feed it
          # `sleep N |` rather than leaving stdin at whatever this script's own stdin happens to be).
          ( sleep 20 | "$OUT/ui_runner_vm" --shm "$FP/ui.shm" --rom "$T/frame_probe.bin" --disk "$FP/blank_disk.img" --aux "$FP/blank_aux.img" --ram-mb 256 --jit-mb 64 --cpus 2 --seconds 15 > "$FP/runner.out" 2>&1 ) &
          RUNNER_PID=$!
          if "$T/frame_probe_check" "$FP/ui.shm" 320 200 aabbccdd 20 > "$FP/check.out" 2>&1; then
            cat "$FP/check.out"; echo "ok  a 320x200 picture the guest code (tests/roms/frame_probe.S) drew reached the shared file with every pixel intact"
          else
            echo "FAILED the frame-probe check:"; cat "$FP/check.out"; echo "-- runner output:"; cat "$FP/runner.out"; rc=1
          fi
          wait "$RUNNER_PID" 2>/dev/null || true
        fi

        echo "=== psci probe: SYSTEM_OFF and SYSTEM_RESET captured at the call, never confused with each other (core/exception.c's psci_halt_* record, docs/boot-diagnostic-contract-2026-11-15.md's stage-2 known gap C-2)"
        if ! tools/build-asm-rom.sh tests/roms/psci_off_probe.S "$T/psci_off.bin" > "$T/psci_off_build.out" 2>&1; then
          echo "FAILED building the psci-off-probe stand-in ROM"; cat "$T/psci_off_build.out"; rc=1
        elif ! tools/build-asm-rom.sh tests/roms/psci_reset_probe.S "$T/psci_reset.bin" > "$T/psci_reset_build.out" 2>&1; then
          echo "FAILED building the psci-reset-probe stand-in ROM"; cat "$T/psci_reset_build.out"; rc=1
        else
          PP=$T/psciprobe; rm -rf "$PP"; mkdir -p "$PP"
          pok=1
          for kind in off reset; do
            truncate -s 16M "$PP/${kind}_disk.img"
            python3 - "$PP/${kind}_aux.img" <<'PY'
import sys
hd = bytes([0xa5,0xd5,0xb3,0xfe,0x97,0xad,0x1d,0x27,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0])
with open(sys.argv[1],'wb') as f:
    f.write(hd); f.truncate(33570816)
PY
            # same reason as the frame-probe run above: stdin must stay open (sleep N |), or ui_runner_vm reads
            # EOF as "input closed" before the guest's one hvc instruction is even reached.
            ( sleep 10 | "$OUT/ui_runner_vm" --shm "$PP/$kind.shm" --rom "$T/psci_$kind.bin" --disk "$PP/${kind}_disk.img" --aux "$PP/${kind}_aux.img" --ram-mb 256 --jit-mb 64 --cpus 2 --seconds 5 > "$PP/$kind.out" 2>&1 )
          done
          OFF_LINE=$(grep RESULT "$PP/off.out"); RESET_LINE=$(grep RESULT "$PP/reset.out")
          echo "off:   $OFF_LINE"; echo "reset: $RESET_LINE"
          echo "$OFF_LINE" | grep -q 'SYSTEM_OFF (fn=0x84000008)' || { echo "FAILED SYSTEM_OFF probe did not report fn=0x84000008/SYSTEM_OFF"; pok=0; }
          echo "$OFF_LINE" | grep -q 'cpu0 ' || { echo "FAILED SYSTEM_OFF probe did not report cpu0"; pok=0; }
          echo "$OFF_LINE" | grep -q 'SYSTEM_RESET\|0x84000009' && { echo "FAILED SYSTEM_OFF probe's RESULT also mentions SYSTEM_RESET/0x84000009"; pok=0; }
          echo "$RESET_LINE" | grep -q 'SYSTEM_RESET (fn=0x84000009)' || { echo "FAILED SYSTEM_RESET probe did not report fn=0x84000009/SYSTEM_RESET"; pok=0; }
          echo "$RESET_LINE" | grep -q 'cpu0 ' || { echo "FAILED SYSTEM_RESET probe did not report cpu0"; pok=0; }
          echo "$RESET_LINE" | grep -qE 'SYSTEM_OFF|0x84000008' && { echo "FAILED SYSTEM_RESET probe's RESULT also mentions SYSTEM_OFF/0x84000008"; pok=0; }
          # pc must land inside this tiny ROM's own code (VMAPPLE_FIRMWARE=0x100000, the hvc is a few
          # instructions in -- well under +0x1000), proving it is the HVC's own address, not some unrelated value.
          for kind in off reset; do
            line=$(grep RESULT "$PP/$kind.out")
            pc=$(echo "$line" | grep -oE 'pc=0x[0-9a-f]+' | cut -d= -f2)
            if [ -z "$pc" ] || [ $(( pc )) -lt $(( 0x100000 )) ] || [ $(( pc )) -ge $(( 0x101000 )) ]; then
              echo "FAILED $kind probe: pc '$pc' is not inside the probe ROM's own firmware range"; pok=0
            fi
          done
          if [ $pok = 1 ]; then echo "ok  SYSTEM_OFF and SYSTEM_RESET are each reported with their own function number, cpu and a pc inside the probe ROM, and never with each other's"
          else echo "-- off runner output:"; cat "$PP/off.out"; echo "-- reset runner output:"; cat "$PP/reset.out"; rc=1; fi
        fi
      fi
    fi
  fi
fi

[ $rc = 0 ] && echo "C1-2 OK" || echo "C1-2 FAILED"
exit $rc
