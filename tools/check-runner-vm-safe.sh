#!/bin/bash
# C1-2: the STATIC half of the VM runner's safety contract (the dynamic half is tools/c1vm-test.sh: strace over a real run). Checks a BUILT host/ui_runner_vm and the sources.
# The runner links the virtual machine, so the symbol check of tools/check-runner-safe.sh (no hv_vm_* at all) cannot apply. What can still be required:
#   binary  1. no network or process-start import (socket, connect, ..., fork, exec*, system, popen, dlopen)
#           2. nothing of the restore stack, TSS, libirecovery, libusb, install, curl, TLS, user-mode network (slirp) or the connection policy in the symbol tables
#           3. none of the test-only fake machine (tests/vm_fake.c) and no option string that would loosen the settings
#           4. shared libraries: only the C library, the math library and the thread / loader libraries
#   source  5. the four settings are assigned in ONE place (hv_runner_make_config, host/runner_vm_core.c), as 1 / 1 / 0 / 0, and nowhere in host/ui_runner_vm.c
#           6. the settings are checked (hv_runner_config_ok) before the machine is created, there is one hv_vm_create call, and its argument is the checked structure
#           7. the options the program accepts are exactly the allowed list; it sets no environment variable (it only removes HV_* ones)
# What this cannot show: that the engine does not reach the network or write a file by some other path. That is what the strace run is for.
# Usage: [NM=nm] [READELF=readelf] tools/check-runner-vm-safe.sh <ui_runner_vm binary>
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
# (restore_ only at the start of a name: enter.S has a local label Lrestore_host that is not the restore stack)
x=$(echo "$SYMS" | grep -Ei -e ' [A-Za-z] [A-Za-z_0-9]*(tss_|irecv|idevice|libusb|hv_install|hv_g0|hv_g1|curl_|mbedtls|slirp|hv_net_guard|hv_dnload|hv_urlasset)' -e ' [A-Za-z] (hv_)?restore_')
[ -z "$x" ] || { bad "restore / TSS / USB-host / network code in the symbol table:"; echo "$x" | head; }
# 3
if strings "$B" | grep -q 'HV-TEST-FAKE-MACHINE'; then bad "the test-only fake machine is in this binary"; fi
if strings "$B" | grep -Eq -- '--(installer|net|ipsw|ecid|seed|readonly|no-readonly|write|rw|dfu)\b'; then bad "the binary names an option that would loosen the settings"; fi
# 4
if command -v "$READELF" >/dev/null 2>&1; then
  need=$($READELF -d "$B" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
  for l in $need; do
    case $l in libc.so|libc.so.6|libm.so|libm.so.6|libpthread.so.0|libdl.so|libdl.so.2|ld-linux-aarch64.so.1|ld-linux-x86-64.so.2) ;; *) bad "unexpected shared library: $l";; esac
  done
else echo "note: no readelf; shared libraries not checked"; fi

# 5
C=host/runner_vm_core.c; R=host/ui_runner_vm.c
for f in $C $R; do [ -f "$f" ] || { echo "FAIL missing source $f"; exit 1; }; done
asg='(->|\.)(readonly_storage|no_net|installer|ecid|seed|jit_rx|jit_rw)[[:space:]]*=[^=]'
n=$(grep -cE "$asg" $R); [ "$n" = 0 ] || bad "$R assigns a safety field ($n line(s))"
for want in 'c->readonly_storage = 1;' 'c->no_net = 1;' 'c->installer = 0;' 'c->ecid = 0; c->seed = 0;'; do
  n=$(grep -cF "$want" $C); [ "$n" = 1 ] || bad "$C: '$want' must appear exactly once (found $n)"
done
n=$(grep -E "$asg" $C | grep -vF -e 'c->readonly_storage = 1;' -e 'c->no_net = 1;' -e 'c->installer = 0;' -e 'c->ecid = 0; c->seed = 0;' | wc -l)
[ "$n" = 0 ] || { bad "$C assigns a safety field somewhere else:"; grep -E "$asg" $C | grep -vF -e 'c->readonly_storage = 1;' -e 'c->no_net = 1;' -e 'c->installer = 0;' -e 'c->ecid = 0; c->seed = 0;'; }
n=$(grep -c 'memset(c, 0, sizeof \*c);' $C); [ "$n" = 1 ] || bad "$C: the structure must be zeroed first, once (found $n)"
# 6
n=$(grep -c 'hv_vm_create(' $R); [ "$n" = 1 ] || bad "$R: exactly one hv_vm_create call expected (found $n)"
grep -q 'hv_vm_create(&cfg, ' $R || bad "$R: hv_vm_create must be given &cfg"
n=$(grep -cE '^[[:space:]]*HvVmConfig ' $R); [ "$n" = 1 ] || bad "$R: exactly one HvVmConfig variable expected (found $n)"
l1=$(grep -n 'hv_runner_make_config(&cfg' $R | head -1 | cut -d: -f1); l2=$(grep -n 'hv_runner_config_ok(&cfg' $R | head -1 | cut -d: -f1); l3=$(grep -n 'hv_vm_create(&cfg' $R | head -1 | cut -d: -f1)
{ [ -n "$l1" ] && [ -n "$l2" ] && [ -n "$l3" ] && [ "$l1" -lt "$l2" ] && [ "$l2" -lt "$l3" ]; } || bad "$R: make_config, then config_ok, then hv_vm_create is the order required (lines '$l1' '$l2' '$l3')"
grep -q 'hv_runner_scrub_env' $R || bad "$R: the HV_* scrub is missing"
l4=$(grep -n 'hv_runner_scrub_env(' $R | head -1 | cut -d: -f1); { [ -n "$l4" ] && [ "$l4" -lt "$l3" ]; } || bad "$R: the scrub must come before hv_vm_create"
grep -q 'bd->vusb' $R || bad "$R: the check that the machine has no USB device port is missing"
# 7
opts=$(grep -o '"--[a-z-]*"' $R | sort -u | tr -d '"' | tr '\n' ' ')
[ "$opts" = "--aux --cpus --disk --jit-mb --ram-mb --rom --seconds --shm --stop-wait-ms " ] || bad "$R: the accepted options are '$opts'"
if grep -nE '(^|[^a-z_])(setenv|putenv)[[:space:]]*\(' $R $C | grep -v '^[^:]*:[0-9]*:[[:space:]]*/[*]' > /dev/null; then bad "$R / $C set an environment variable"; fi
if grep -nE '\b(system|popen|fork|exec[a-z]*|socket|connect|dlopen)[[:space:]]*\(' $R $C | grep -v '^[^:]*:[0-9]*:[[:space:]]*/[*]' | grep -v '^[^:]*:[0-9]*:.*\*/' > /dev/null; then bad "$R / $C call a process-start or network function"; fi

[ $fail = 0 ] && echo "OK  VM runner (static): no network or process-start imports, no restore / TSS / USB-host / slirp code, safety fields set once in hv_runner_make_config and checked before hv_vm_create, accepted options as listed, no fake machine"
exit $fail
