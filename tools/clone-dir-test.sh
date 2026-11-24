#!/bin/bash
# host/clone_dir.h (the clone checks of host/ui_runner_vm_clone.c) in temporary directories on this machine, plain and with ASan+UBSan. Usage: tools/clone-dir-test.sh
set -u
cd "$(dirname "$0")/.."
for san in "" "-fsanitize=address,undefined"; do
  cc -std=gnu11 -Wall -Wextra -Werror -Wno-format-truncation -Wno-unused-result -O1 -g $san -Ihost tests/test_clone_dir.c -o /tmp/test_clone_dir_$$ 2>&1 | head -20
  out=$(/tmp/test_clone_dir_$$); rc=$?
  echo "$out" | grep -E "FAIL|CLONEDIR"
  [ $rc = 0 ] && echo "$out" | grep -q "^CLONEDIR OK" || { echo "FAILED (${san:-plain})"; exit 1; }
done
rm -f /tmp/test_clone_dir_$$
