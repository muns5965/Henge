#!/bin/bash
# The G7 restore stack must be the restore tool WITHOUT any stop switch, and must reach nothing that ships. Usage: tools/check-restore-g7-build.sh [<restore g7 build dir>]
#  source: no -DHV_STOP_<name> in tools/restore-g7-build.sh or tools/g7-runner-build.sh; the G7 runner script uses the G7 stack, not the self-test stack
#  binary (when a build dir is given): no hv_stop_*_hook symbol in the archive (they exist only in a build with a stop switch; the self-test archive has them, which is the control)
cd "$(dirname "$0")/.." || exit 1
fail=0; bad() { echo "FAIL $*"; fail=1; }
for f in tools/restore-g7-build.sh tools/g7-runner-build.sh; do
  [ -f "$f" ] || { bad "missing $f"; continue; }
  grep -nE -- '-DHV_STOP_[A-Z]' "$f" | grep -v '^[0-9]*:#' | grep . > /dev/null && bad "$f sets a stop switch"
done
grep -q 'restore-selftest' tools/g7-runner-build.sh && bad "the G7 runner script refers to the self-test restore stack"
if [ -n "${1:-}" ]; then
  A="$1/rs/lib.a"
  [ -f "$A" ] || bad "no archive $A"
  if [ -f "$A" ]; then
    nm "$A" 2>/dev/null | grep -E 'hv_stop_[a-e]_hook' | grep . > /dev/null && bad "the G7 archive has a stop hook (a stop switch was compiled in)"
  fi
fi
[ $fail = 0 ] && echo "OK  G7 restore build: no stop switch in the sources${1:+ and none in the archive}"
exit $fail
