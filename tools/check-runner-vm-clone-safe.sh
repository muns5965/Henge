#!/bin/bash
# CLONE BOOT: the STATIC half of host/ui_runner_vm_clone.c's safety contract, the counterpart of tools/check-runner-vm-safe.sh
# (host/ui_runner_vm.c) and tools/check-g4ui-launcher-source.sh's own source-side checks (host/ui_runner_vm_g4.c). It is a check against
# FORBIDDEN symbols and strings, an allow-list of shared LIBRARIES, and rules on the SOURCE. Checks a BUILT host/ui_runner_vm_clone
# and the sources.
# This binary's whole point is a NONZERO ecid/seed, so unlike tools/check-runner-vm-safe.sh's check 3 (which forbids the strings --ecid/
# --seed from appearing in host/ui_runner_vm at all), this script instead REQUIRES the binary to actually name both --ecid and --seed
# (so the capability is really wired to the command line, not merely present in a dead code path) while still forbidding every OTHER
# string that would loosen a setting this binary does not touch (--net, --no-readonly, --write, --rw, --dfu, --installer, --ipsw).
#   binary  1. no network or process-start import (socket, connect, ..., fork, exec*, system, popen, dlopen)
#           2. nothing of the restore stack, TSS, libirecovery, libusb, install, curl, TLS, user-mode network (slirp) or the connection policy in the symbol tables
#           3. none of the test-only fake machine (tests/vm_fake.c); --ecid and --seed ARE named (this binary's whole point); no OTHER
#              option string that would loosen a setting this binary does not touch
#           4. shared libraries: only the C library, the math library and the thread / loader libraries (allow-list); with EXPECT_ARCH set (AArch64) the ELF machine type
#   source  5. readonly_storage/no_net/installer are never assigned in this file (they come from hv_runner_make_config, untouched); ecid
#              and seed ARE assigned here, exactly once each, and nowhere else safety-relevant is touched
#           6. the settings are checked (installed_boot_config_ok, NOT hv_runner_config_ok) before the machine is created, there is one
#              hv_vm_create call, and its argument is the checked structure; the usb device port presence is checked against the requested usb mode (installer stays 0)
#           7. the options the program accepts are exactly the allowed list; it sets no environment variable (it only removes HV_* ones)
# What this cannot show: that the engine does not reach the network or write a file by some other path; that is what a strace-based run
# (mirroring tools/c1vm-test.sh, not yet extended to this binary -- tracked as a follow-up) would be for.
# Usage: [NM=nm] [READELF=readelf] [EXPECT_ARCH=AArch64] tools/check-runner-vm-installed-safe.sh <ui_runner_vm_clone binary>
cd "$(dirname "$0")/.." || exit 1
B=${1:?runner binary}
[ -f "$B" ] || { echo "FAIL no such file: $B"; exit 1; }
fail=0
NM=${NM:-nm}; READELF=${READELF:-readelf}
bad() { echo "FAIL $*"; fail=1; }
SYMS=$($NM -D "$B" 2>/dev/null; $NM "$B" 2>/dev/null)
[ -n "$SYMS" ] || { echo "FAIL no symbols could be read from $B with $NM"; exit 1; }

# 1
x=$(echo "$SYMS" | grep -E ' [UuTtWwVv] (socket|connect|bind|listen|accept|accept4|send|sendto|sendmsg|recv|recvfrom|recvmsg|getaddrinfo|gethostbyname|gethostbyname2|getnameinfo|dlopen|execv|execve|execvp|execl|execlp|fork|vfork|system|popen|posix_spawn|socketpair)(@.*)?$')
[ -z "$x" ] || { bad "network / process-start symbols:"; echo "$x"; }
# 2
x=$(echo "$SYMS" | grep -Ei -e ' [A-Za-z] [A-Za-z_0-9]*(tss_|irecv|idevice|libusb|hv_install|hv_g0|hv_g1|curl_|mbedtls|slirp|hv_net_guard|hv_dnload|hv_urlasset)' -e ' [A-Za-z] (hv_)?restore_')
[ -z "$x" ] || { bad "restore / TSS / libusb-passthrough / network code in the symbol table:"; echo "$x" | head; }
# 3
if strings "$B" | grep -q 'HV-TEST-FAKE-MACHINE'; then bad "the test-only fake machine is in this binary"; fi
for w in clone-dir ecid-dir; do strings "$B" | grep -q -- "--$w" || bad "this binary must name --$w (its whole point); it does not"; done
if strings "$B" | grep -Eq -- '--(ecid|seed|disk|aux)($|[^-a-z])'; then bad "the binary names --ecid/--seed/--disk/--aux: the ECID and the storage paths must not be options"; fi
if strings "$B" | grep -Eq -- '--(installer|net|ipsw|readonly|no-readonly|write|rw|dfu)\b'; then bad "the binary names an option that would loosen a setting this binary does not touch"; fi
# 4
if command -v "$READELF" >/dev/null 2>&1; then
  need=$($READELF -d "$B" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
  for l in $need; do
    case $l in
      libc.so|libc.so.6|libm.so|libm.so.6|libpthread.so.0|libdl.so|libdl.so.2|ld-linux-aarch64.so.1|ld-linux-x86-64.so.2|liblog.so) ;;
      *) bad "unexpected shared library: $l";;
    esac
  done
  if [ -n "${EXPECT_ARCH:-}" ]; then
    m=$($READELF -h "$B" 2>/dev/null | sed -n 's/^ *Machine: *//p')
    case "$m" in *"$EXPECT_ARCH"*) ;; *) bad "ELF machine is '$m', expected $EXPECT_ARCH";; esac
  fi
else echo "note: no readelf; shared libraries not checked"; fi

# 5
C=host/runner_vm_core.c; R=host/ui_runner_vm_clone.c; H=host/clone_dir.h
for f in $C $R $H; do [ -f "$f" ] || { echo "FAIL missing source $f"; exit 1; }; done
asg_locked='(->|\.)(no_net|installer|jit_rx|jit_rw)[[:space:]]*=[^=]'
n=$(grep -cE "$asg_locked" $R); [ "$n" = 0 ] || bad "$R assigns a locked safety field ($n line(s)) -- those must come only from hv_runner_make_config"
n=$(grep -cE '(->|\.)readonly_storage[[:space:]]*=[^=]' $R); [ "$n" = 1 ] || bad "$R: readonly_storage may be assigned exactly once, after the clone checks (found $n)"
n=$(grep -cF 'cfg.readonly_storage = 0;' $R); [ "$n" = 1 ] || bad "$R: 'cfg.readonly_storage = 0;' must appear exactly once (found $n)"
n=$(grep -cF 'cfg.ecid = ecid;' $R); [ "$n" = 1 ] || bad "$R: 'cfg.ecid = ecid;' must appear exactly once (found $n)"
n=$(grep -cF 'cfg.seed = 0;' $R); [ "$n" = 1 ] || bad "$R: 'cfg.seed = 0;' must appear exactly once (found $n)"
for want in 'c->readonly_storage = 1;' 'c->no_net = 1;' 'c->installer = 0;' 'c->ecid = 0; c->seed = 0;'; do
  n=$(grep -cF "$want" $C); [ "$n" = 1 ] || bad "$C: '$want' must appear exactly once (found $n) -- the shared runner's own guarantee must stay untouched"
done
# no write path of its own: the only writable thing is the disk handed to the VM
if grep -nE '\b(rename|renameat|unlink|unlinkat|remove|truncate|ftruncate|mkdir|symlink|link)[[:space:]]*\(|O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|"w"|"a"' $R $H | grep -v '^[^:]*:[0-9]*:[[:space:]]*/[*]' | grep -v '^[^:]*:[0-9]*:.*\*/' > /dev/null; then bad "$R / $H create, rename, remove or write a file itself"; fi
# 6
n=$(grep -c 'hv_vm_create(' $R); [ "$n" = 1 ] || bad "$R: exactly one hv_vm_create call expected (found $n)"
grep -q 'hv_vm_create(&cfg, ' $R || bad "$R: hv_vm_create must be given &cfg"
n=$(grep -cE '^[[:space:]]*HvVmConfig ' $R); [ "$n" = 1 ] || bad "$R: exactly one HvVmConfig variable expected (found $n)"
grep -q 'clone_boot_config_ok(&cfg, ecid, 1, why, sizeof why)' $R || bad "$R: clone_boot_config_ok must be called with &cfg, ecid, 1, why, sizeof why"
grep -q 'hv_runner_config_ok(&cfg' $R && bad "$R: must not call the shared hv_runner_config_ok (it hard-requires ecid=0/seed=0); use clone_boot_config_ok instead"
l0=$(grep -n 'hv_clone_open(clone_dir' $R | head -1 | cut -d: -f1); l1=$(grep -n 'hv_runner_make_config(&cfg' $R | head -1 | cut -d: -f1); lr=$(grep -n 'cfg.readonly_storage = 0;' $R | head -1 | cut -d: -f1)
l2=$(grep -n 'clone_boot_config_ok(&cfg' $R | head -1 | cut -d: -f1); l3=$(grep -n 'hv_vm_create(&cfg' $R | head -1 | cut -d: -f1)
{ [ -n "$l0" ] && [ -n "$l1" ] && [ -n "$lr" ] && [ -n "$l2" ] && [ -n "$l3" ] && [ "$l0" -lt "$l1" ] && [ "$l1" -lt "$lr" ] && [ "$lr" -lt "$l2" ] && [ "$l2" -lt "$l3" ]; } || bad "$R: clone check, make_config, writable, config check, hv_vm_create is the order required (lines '$l0' '$l1' '$lr' '$l2' '$l3')"
grep -q 'hv_runner_scrub_env' $R || bad "$R: the HV_* scrub is missing"
grep -q 'scrubbed < 0' $R || bad "$R: a failed scrub (result < 0) must stop the run"
n=$(grep -cE '(^|[^a-z_])unsetenv[[:space:]]*\(' $R); [ "$n" = 0 ] || bad "$R calls unsetenv itself (the scrub is in $C)"
l5=$(grep -n 'scrubbed < 0' $R | head -1 | cut -d: -f1); { [ -n "$l5" ] && [ "$l5" -lt "$l3" ]; } || bad "$R: the scrub result must be checked before hv_vm_create"
l4=$(grep -n 'hv_runner_scrub_env(' $R | head -1 | cut -d: -f1); { [ -n "$l4" ] && [ "$l4" -lt "$l3" ]; } || bad "$R: the scrub must come before hv_vm_create"
grep -q 'bd->vusb' $R || bad "$R: the check that the machine has no USB device port is missing"
grep -q 'usb_present != cfg.usb_on_normal_boot' $R || bad "$R: must refuse to run if the usb device port presence differs from the requested usb mode"
grep -q 'cfg.usb_probe_on_normal_boot = usb_mode >= 2' $R && grep -q 'cfg.usb_on_normal_boot = usb_mode >= 1' $R || bad "$R: the usb mode must map to exactly the two host-only usb flags"
grep -q 'c->installer) BAD' $R || bad "$R: installer must stay refused in every usb mode"
grep -q 'hv_runner_make_config(&cfg, rom, cl.disk, cl.aux,' $R || bad "$R: the machine must be given the descriptor paths of the files that were checked (cl.disk, cl.aux)"
grep -q 'flock(c->dirfd, LOCK_EX | LOCK_NB)' $H || bad "$H: the clone folder must be locked"
grep -q '/proc/self/fd/%d' $H || bad "$H: the checked files must be handed over as descriptor paths"
grep -q 'hv_clone_ecid_tag(ecid, tag)' $H || bad "$H: the ECID tag must be computed here"
[ "$(grep -c 'char reason_txt\[160\];' $R)" = 1 ] && [ "$(grep -n 'char reason_txt\[160\];' $R | cut -d: -f1)" -lt "$(grep -n 'for (;;) {' $R | tail -1 | cut -d: -f1)" ] || bad "$R: the stop-reason text must outlive the loop (declared before it)"
l6=$(grep -n 'hv_vm_start(1' $R | head -1 | cut -d: -f1); l7=$(grep -n 'hv_vm_start_failed()' $R | head -1 | cut -d: -f1); l8=$(grep -n 'hv_shm_set_state(g_shm, HV_SHM_RUNNING)' $R | head -1 | cut -d: -f1)
{ [ -n "$l6" ] && [ -n "$l7" ] && [ -n "$l8" ] && [ "$l6" -lt "$l7" ] && [ "$l7" -lt "$l8" ]; } || bad "$R: hv_vm_start_failed() must be checked right after hv_vm_start and before the state becomes RUNNING"
grep -q 'hv_vm_flush_disks_checked()' $R || bad "$R: the disk flush result must be checked (hv_vm_flush_disks_checked)"
# 7
opts=$(grep -o '"--[a-z-]*"' $R | sort -u | tr -d '"' | tr '\n' ' ')
[ "$opts" = "--clone-dir --cpus --ecid-dir --jit-mb --ram-mb --rom --seconds --shm --stop-wait-ms --usb-mode " ] || bad "$R: the accepted options are '$opts'"
if grep -nE '(^|[^a-z_])(setenv|putenv)[[:space:]]*\(' $R $C | grep -v '^[^:]*:[0-9]*:[[:space:]]*/[*]' > /dev/null; then bad "$R / $C set an environment variable"; fi
if grep -nE '\b(system|popen|fork|exec[a-z]*|socket|connect|dlopen)[[:space:]]*\(' $R $C | grep -v '^[^:]*:[0-9]*:[[:space:]]*/[*]' | grep -v '^[^:]*:[0-9]*:.*\*/' > /dev/null; then bad "$R / $C call a process-start or network function"; fi

[ $fail = 0 ] && echo "OK  VM runner clone-boot mode (static): no network or process-start imports, no restore / TSS / libusb-passthrough / slirp code, only the clone folder and the ECID folder are options, writable storage set once and only after the clone checks, no write path of its own, the shared runner's own 0/0 guarantee stays untouched, clone_boot_config_ok checked before hv_vm_create, accepted options as listed, usb port required absent, no fake machine"
exit $fail
