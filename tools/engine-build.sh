#!/bin/sh
# Build the engine core for a non-Apple target and link tests/test_mmu.c.
# Usage: CC=<compiler> [EXTRA_LIBS=...] [EXTRA_LDFLAGS=...] tools/engine-build.sh <output dir>
# Every file is compiled on its own and failures are collected, so one run
# shows all the files that do not build, not only the first.
set -u
OUT=${1:?output dir}
: "${CC:?CC}"
mkdir -p "$OUT"
CFLAGS="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -O2 -g -Icore -Ihost -Idevices -Wno-unused-parameter"
fail=0
objs=""
for f in core/classify.c core/operands.c core/translate.c core/dispatch.c core/mmu.c core/memop.c \
         core/exception.c core/sysreg.c core/machine.c host/jit_region.c host/disk.c; do
  o="$OUT/$(basename "$f" .c).o"
  extra=""
  [ "$f" = core/mmu.c ] && extra="-mgeneral-regs-only"
  if $CC $CFLAGS $extra -c "$f" -o "$o" 2> "$o.log"; then
    echo "ok      $f"; cat "$o.log"; objs="$objs $o"
  else
    echo "FAILED  $f"; cat "$o.log"; fail=1
  fi
done
if $CC $CFLAGS -c host/enter.S -o "$OUT/enter.o" 2> "$OUT/enter.log"; then
  echo "ok      host/enter.S"; objs="$objs $OUT/enter.o"
else
  echo "FAILED  host/enter.S"; cat "$OUT/enter.log"; fail=1
fi
[ $fail = 0 ] || { echo "compile step failed; not linking"; exit 1; }
# Test programs are assembly run both natively and through the engine; their
# instructions need an Armv8.5 assembler target (dit, pauth, ...).
$CC $CFLAGS -march=armv8.5-a -Itests -c tests/harness.S -o "$OUT/harness.o" || { echo "FAILED  tests/harness.S"; exit 1; }
tfail=0
for t in mmu jit mem exc; do
  if [ $t = mmu ]; then
    srcs="tests/test_mmu.c"
  else
    $CC $CFLAGS -march=armv8.5-a -Itests -c "tests/${t}_programs.S" -o "$OUT/${t}_programs.o" 2> "$OUT/${t}_programs.log" || { echo "FAILED  tests/${t}_programs.S"; cat "$OUT/${t}_programs.log"; tfail=1; continue; }
    srcs="tests/test_$t.c $OUT/${t}_programs.o $OUT/harness.o"
  fi
  if $CC $CFLAGS -Itests $srcs $objs ${EXTRA_LDFLAGS:-} ${EXTRA_LIBS:-} -o "$OUT/test_$t" 2> "$OUT/test_$t.log"; then
    echo "linked  $OUT/test_$t"
  else
    echo "FAILED  link test_$t"; cat "$OUT/test_$t.log"; tfail=1
  fi
done
[ $tfail = 0 ] || exit 1
