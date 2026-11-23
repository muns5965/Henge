#!/bin/bash
# The static half of g7_probe's safety contract: what the FINISHED binary must not contain. Usage: tools/check-g7-probe-safe.sh <binary>
# Symbols (nm, defined or undefined) that would mean a machine, the restore tool, the network, a process start, a disk write path or the disk creator is in it; and the
# strings of the execution stages, which exist only in a build with HV_G7_MOCK_EXECUTION. A search of symbols and strings is a help, not a proof; the dynamic half is the
# strace run of tools/g7-probe-test.sh.
set -u
B=${1:?binary}; NM=${NM:-nm}; READELF=${READELF:-readelf}; fail=0
bad() { echo "FAIL $*"; fail=1; }
syms=$($NM -D "$B" 2>/dev/null; $NM "$B" 2>/dev/null)
for s in hv_vm_create hv_vm_start hv_vm_board hv_install_start hv_install_disk_create hv_install_disk_reset hv_g7_conclude hv_g7_restore_run hv_g7_collect_evidence hv_install_result hv_install_cancel hv_vm_stop hv_vm_flush_disks hv_usb_wait_online hv_g7_pair_begin hv_g7_pair_finish hv_g7_pair_quarantine hv_disk_open hv_disk_write \
         idevicerestore_start restored_start_restore hv_restore_session_run_client hv_net_guard_install socket connect bind listen accept sendto sendmsg recvfrom fork vfork execve execv execvp system popen posix_spawn dlopen \
         unlink unlinkat rename renameat rmdir mkdir mkdirat symlink truncate ftruncate pwrite write_all; do
  echo "$syms" | grep -qE "[[:space:]]$s(@.*)?\$" && bad "binary has the symbol $s"
done
for str in "configuration refused before the machine" "the machine was not created" "the restore did not finish" "run through the recorder"; do
  strings -a "$B" | grep -qF "$str" && bad "binary contains the execution-stage text: $str"
done
if command -v "$READELF" >/dev/null 2>&1; then
  libs=$($READELF -d "$B" 2>/dev/null | grep NEEDED | sed 's/.*\[\(.*\)\]/\1/' | grep -vE '^(libc\.so|libm\.so|libdl\.so|ld-linux|libpthread\.so)' )
  [ -z "$libs" ] || bad "unexpected shared libraries: $libs"
fi
[ $fail = 0 ] && echo "OK  g7_probe static check: no machine, restore, network, process-start, write or disk-creation symbol, no execution-stage text, only the C library"
exit $fail
