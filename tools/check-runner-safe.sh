#!/bin/sh
# C1-1: the runner binary must not be able to reach a network, a virtual machine's files, Apple's servers or the restore tool. Checks a BUILT binary (plain build, not a sanitizer one):
#   - imports: no socket, connect, bind, listen, accept, send*, recv*, name resolution, no dlopen, no exec*, fork, system, popen (nothing it could use to reach out or start something else)
#   - defined or imported symbols: nothing of the restore stack, TSS, USB, libirecovery, install, curl, TLS, the virtual machine (hv_vm_*, hv_machine_*, hv_disk_*)
#   - the file it opens: only its --shm file and standard input (a strings check for ROM / disk option names)
# Usage: tools/check-runner-safe.sh <runner binary>
B=${1:?runner binary}
[ -f "$B" ] || { echo "FAIL no such file: $B"; exit 1; }
fail=0
NM=${NM:-nm}
SYMS=$($NM -D "$B" 2>/dev/null; $NM "$B" 2>/dev/null)
[ -n "$SYMS" ] || { echo "FAIL no symbols could be read from $B with $NM"; exit 1; }
bad=$(echo "$SYMS" | grep -E ' [UuTtWwVv] (socket|connect|bind|listen|accept|accept4|send|sendto|sendmsg|recv|recvfrom|recvmsg|getaddrinfo|gethostbyname|gethostbyname2|getnameinfo|dlopen|execv|execve|execvp|execl|execlp|fork|vfork|system|popen|posix_spawn|socketpair)(@.*)?$')
[ -z "$bad" ] || { echo "FAIL the runner has network / process-start symbols:"; echo "$bad"; fail=1; }
bad=$(echo "$SYMS" | grep -Ei ' [A-Za-z] [A-Za-z_0-9]*(tss_|irecv|restore_|idevice|libusb|usb_host|hv_install|hv_g0|hv_g1|curl_|mbedtls|hv_vm_|hv_machine_|hv_disk_|hv_pvg|slirp|hv_net)' )
[ -z "$bad" ] || { echo "FAIL the runner contains restore / TSS / USB / VM / network code:"; echo "$bad" | head; fail=1; }
if strings "$B" | grep -Eq -- '--(rom|disk|aux|installer|net|ipsw|ecid)\b'; then echo "FAIL the runner names an option for a ROM, disk, installer or network"; fail=1; fi
[ $fail = 0 ] && echo "OK  runner: no network or process-start imports, no VM / restore / TSS / USB code, no option for a ROM, disk or network"
exit $fail
