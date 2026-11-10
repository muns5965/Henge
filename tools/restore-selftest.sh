#!/bin/bash
# Runs a built restore_selftest on three bad IPSW inputs; each must fail cleanly (RESTORE_RESULT=-1), usbmuxd must be listening, exit 0.
# Usage: tools/restore-selftest.sh path/to/restore_selftest workdir
set -u
BIN=${1:?restore_selftest}; W=${2:?workdir}
rm -rf "$W"; mkdir -p "$W"
head -c 4096 /dev/urandom > "$W/bad.ipsw"
python3 -c "import zipfile;z=zipfile.ZipFile('$W/nomanifest.ipsw','w');z.writestr('readme.txt','x');z.close()"
rc=0
for f in "$W/missing.ipsw" "$W/bad.ipsw" "$W/nomanifest.ipsw"; do
  mkdir -p "$W/w"
  out=$(timeout 120 "$BIN" "$f" "$W/w" 2>&1); st=$?
  echo "$out" | grep -E "RESTORE_RESULT|MUX_LISTENING" | tr '\n' ' '; echo " exit=$st  ($(basename $f))"
  echo "$out" | grep -q "RESTORE_RESULT=-1" && echo "$out" | grep -q "MUX_LISTENING=1" && [ $st -eq 0 ] || { echo "FAIL: $f"; echo "$out" | tail -15; rc=1; }
done
exit $rc
