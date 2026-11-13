#!/bin/bash
# G2-b isolation: the mock orchestrator is not connected to anything real, and stays that way.
#  1. host/install_plan.{h,c} include only standard headers (no project header, so no network, USB, disk or install code can be reached through them);
#  2. they name none of the real things;
#  3. nothing but tests/test_install_plan.c and tools/install-plan-test.sh refers to it (no build file, no app, no runner).
set -u
rc=0
bad() { echo "ISOLATION: $*"; rc=1; }
for f in host/install_plan.h host/install_plan.c; do
  [ -f "$f" ] || { bad "$f missing"; continue; }
  grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' "$f" | grep -v '"install_plan.h"' && bad "$f includes a project header"
  grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<' "$f" | grep -vE '<(stddef|stdint|stdio|string)\.h>' && bad "$f includes a non-basic system header"
  grep -nEi 'curl|irecv|libusb|tss|apple|ipsw|disk|socket|\bconnect\(|fopen|open\(|system\(|exec[lv]|fork|pthread|install\.h|usb_host|restore_' "$f" | grep -vE '^[0-9]+:[[:space:]]*(/\*|\*)' | grep -vE 'no connection|not connected|nothing real' && bad "$f names something real"
done
refs=$(grep -rlE 'install_plan' --include='*' . 2>/dev/null | grep -vE '^\./(\.git/|host/install_plan\.[ch]$|tests/test_install_plan\.c$|tools/install-plan-test\.sh$|tools/check-install-plan-isolation\.sh$|\.github/workflows/android-engine\.yml$)')
[ -z "$refs" ] || bad "referenced from outside the test: $refs"
[ $rc = 0 ] && echo "PASS install-plan isolation"
exit $rc
