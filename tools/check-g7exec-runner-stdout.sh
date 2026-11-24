#!/bin/bash
# The G7 runner's stdout must be line-buffered before its first output (host/g7_runner.c main), because the app reads it through a pipe. Two checks:
#  1. source order: setvbuf(stdout, ..., _IOLBF, ...) comes before every printf/puts/fputs in main();
#  2. the mechanism on a plain child process: a line written WITHOUT fflush reaches the pipe while the child is still running, and the same child without setvbuf does not deliver it.
# Usage: tools/check-g7exec-runner-stdout.sh [work dir]
set -u
cd "$(dirname "$0")/.."
W=${1:-$(mktemp -d)}; mkdir -p "$W"
fail=0; ok() { echo "PASS $1"; }; bad() { echo "FAIL $1"; fail=$((fail+1)); }
m=$(grep -n '^int main(' host/g7_runner.c | head -1 | cut -d: -f1)
sv=$(awk -v m="$m" 'NR>m && /setvbuf\(stdout, NULL, _IOLBF, 0\)/ {print NR; exit}' host/g7_runner.c)
fp=$(awk -v m="$m" 'NR>m && /(printf|puts|fputs)\(/ && !/setvbuf/ && !/fprintf\(stderr/ {print NR; exit}' host/g7_runner.c)
{ [ -n "$sv" ] && [ -n "$fp" ] && [ "$sv" -lt "$fp" ]; } && ok "setvbuf(stdout, _IOLBF) precedes the first stdout output in main" || bad "setvbuf(stdout, _IOLBF) must precede the first stdout output in main (setvbuf line '$sv', first output line '$fp')"
grep -q 'setvbuf(stdout, NULL, _IOLBF, 0) != 0' host/g7_runner.c && ok "the result of setvbuf is checked" || bad "the result of setvbuf must be checked"
cat > "$W/child.c" <<'EOT'
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc > 1) setvbuf(stdout, NULL, _IOLBF, 0);
    printf("early line\n");   /* no fflush */
    sleep(2);
    return 0;
}
EOT
cc -o "$W/child" "$W/child.c" || { echo "FAIL build child"; exit 1; }
probe() { # $1 = args; prints "yes" if the line arrived within 1 s while the child still sleeps
python3 - "$W/child" "$@" <<'EOP'
import subprocess, sys, select, time
p = subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE)
r, _, _ = select.select([p.stdout], [], [], 1.0)
print("yes" if r else "no")
p.wait()
EOP
}
[ "$(probe line)" = yes ] && ok "line-buffered child: the line arrives while the child is still running" || bad "line-buffered child: the line did not arrive in time"
[ "$(probe)" = no ] && ok "default child (pipe): the same line is held back (why the setting is needed)" || bad "default child: expected the line to be held back"
[ "$fail" = 0 ] && echo "G7EXEC RUNNER STDOUT OK" || { echo "G7EXEC RUNNER STDOUT FAILED"; exit 1; }
