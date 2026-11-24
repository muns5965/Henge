#!/bin/bash
# tools/check-g7exec-apk.sh against synthetic APKs (zip files): the general one must not contain the G7 execution build, the execution one must. Usage: tools/g7exec-apk-check-test.sh <dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?dir}; rm -rf "$OUT"; mkdir -p "$OUT"; fail=0
python3 - "$OUT" <<'P'
import sys, zipfile, struct
o = sys.argv[1]
def elf(machine, tail=b""):
    h = bytearray(64); h[0:4] = b"\x7fELF"; h[4] = 2; h[5] = 1; h[6] = 1
    struct.pack_into("<HHIQQQIHHHHHH", h, 16, 3, machine, 1, 0, 0, 0, 0, 64, 0, 0, 0, 0, 0)
    return bytes(h) + tail
def apk(name, files):
    with zipfile.ZipFile(f"{o}/{name}.apk", "w") as z:
        z.writestr("AndroidManifest.xml", "x")
        for k, v in files.items(): z.writestr(k, v)
good = elf(183, b"G7RUN RESULT g7-result: ")
cb = elf(183, b"ui_runner_vm_clone")
CB = "lib/arm64-v8a/libg7cloneboot.so"
apk("general", {"classes.dex": b"dex\nMainActivity G7ProbeLauncher", "lib/arm64-v8a/libg7probe.so": elf(183)})
apk("general_lib", {"classes.dex": b"dex", "lib/arm64-v8a/libg7runner.so": good})
apk("general_cls", {"classes.dex": b"dex G7ExecLauncher"})
OFF = b" G7-BUILD-KIND:EXEC-OFF"; ON = b" G7-BUILD-KIND:START-ON"
apk("exec", {"classes.dex": b"dex G7ExecLauncher G7ExecActivity" + OFF, "lib/arm64-v8a/libg7runner.so": good, CB: cb})
apk("start", {"classes.dex": b"dex G7ExecLauncher G7ExecActivity" + ON, "lib/arm64-v8a/libg7runner.so": good, CB: cb})
apk("exec_with_on", {"classes.dex": b"dex G7ExecLauncher G7ExecActivity" + ON, "lib/arm64-v8a/libg7runner.so": good, CB: cb})
apk("exec_nomarker", {"classes.dex": b"dex G7ExecLauncher G7ExecActivity", "lib/arm64-v8a/libg7runner.so": good, CB: cb})
apk("start_with_off", {"classes.dex": b"dex G7ExecLauncher G7ExecActivity" + OFF, "lib/arm64-v8a/libg7runner.so": good, CB: cb})
apk("start_both", {"classes.dex": b"dex G7ExecLauncher G7ExecActivity" + OFF + ON, "lib/arm64-v8a/libg7runner.so": good, CB: cb})
apk("general_cb", {"classes.dex": b"dex", CB: cb})
apk("exec_nocb", {"classes.dex": b"dex G7ExecLauncher G7ExecActivity" + OFF, "lib/arm64-v8a/libg7runner.so": good})
apk("exec_cb_x86", {"classes.dex": b"dex G7ExecLauncher G7ExecActivity" + OFF, "lib/arm64-v8a/libg7runner.so": good, CB: elf(62, b"ui_runner_vm_clone")})
apk("exec_cb_wrong", {"classes.dex": b"dex G7ExecLauncher G7ExecActivity" + OFF, "lib/arm64-v8a/libg7runner.so": good, CB: elf(183, b"hello")})
apk("general_marker", {"classes.dex": b"dex" + ON})
apk("start_nolib", {"classes.dex": b"dex G7ExecLauncher" + ON})
apk("exec_nolib", {"classes.dex": b"dex G7ExecLauncher" + OFF})
apk("exec_x86", {"classes.dex": b"dex G7ExecLauncher" + OFF, "lib/arm64-v8a/libg7runner.so": elf(62, b"G7RUN RESULT g7-result: ")})
apk("exec_notrunner", {"classes.dex": b"dex G7ExecLauncher" + OFF, "lib/arm64-v8a/libg7runner.so": elf(183, b"hello")})
apk("exec_nocls", {"lib/arm64-v8a/libg7runner.so": good, "classes.dex": b"dex" + OFF})
P
t() { local want=$1 mode=$2 f=$3; bash tools/check-g7exec-apk.sh "$mode" "$OUT/$f.apk" > "$OUT/$f.out" 2>&1; local rc=$?; if { [ "$want" = ok ] && [ $rc = 0 ]; } || { [ "$want" = bad ] && [ $rc != 0 ]; }; then echo "PASS $mode $f -> $want"; else echo "FAIL $mode $f expected $want, rc=$rc"; cat "$OUT/$f.out"; fail=1; fi; }
t ok general general
t bad general general_lib
t bad general general_cls
t ok exec exec
t ok start start
t bad exec exec_with_on
t bad exec exec_nomarker
t bad exec start
t bad start exec
t bad start start_with_off
t bad start start_both
t bad start start_nolib
t bad general general_marker
t bad general general_cb
t bad exec exec_nocb
t bad exec exec_cb_x86
t bad exec exec_cb_wrong
t bad exec exec_nolib
t bad exec exec_x86
t bad exec exec_notrunner
t bad exec exec_nocls
bash tools/check-g7exec-apk.sh bogus "$OUT/exec.apk" > /dev/null 2>&1; [ $? = 64 ] && echo "PASS bad mode -> 64" || { echo "FAIL bad mode"; fail=1; }
[ $fail = 0 ] && echo "G7EXEC-APK-CHECK OK" || { echo "G7EXEC-APK-CHECK FAILED"; exit 1; }
