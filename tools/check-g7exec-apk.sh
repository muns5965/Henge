#!/bin/bash
# What an APK may contain of the G7 execution build. Usage: tools/check-g7exec-apk.sh general|exec <apk>
#   general (assembleDebug, the app everyone installs): NO libg7runner.so, no G7ExecActivity/G7ExecLauncher class.
#   exec    (assembleG7exec): libg7runner.so is there as AArch64 (READELF), G7ExecActivity and G7ExecLauncher are there; and nothing else of the restore stack's test programs is required.
set -u
M=${1:?general|exec}; APK=${2:?apk}; READELF=${READELF:-readelf}
[ -f "$APK" ] || { echo "FAIL no such file: $APK"; exit 1; }
fail=0; bad() { echo "FAIL $*"; fail=1; }
L=$(unzip -l "$APK" 2>/dev/null)
echo "$L" | grep -q "AndroidManifest.xml" || { echo "FAIL not an APK: $APK"; exit 1; }
has_lib=$(echo "$L" | grep -c "lib/arm64-v8a/libg7runner.so")
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
unzip -q -o "$APK" 'classes*.dex' -d "$D" 2>/dev/null
has_cls=$(cat "$D"/classes*.dex 2>/dev/null | grep -a -c "G7ExecLauncher\|G7ExecActivity")
case $M in
  general)
    [ "$has_lib" = 0 ] || bad "the general APK contains libg7runner.so"
    [ "$has_cls" = 0 ] || bad "the general APK contains the G7 execution launcher or activity"
    ;;
  exec)
    [ "$has_lib" = 1 ] || bad "the execution APK does not contain libg7runner.so"
    [ "$has_cls" != 0 ] || bad "the execution APK does not contain the launcher/activity"
    if [ "$has_lib" = 1 ]; then
      unzip -q -o "$APK" lib/arm64-v8a/libg7runner.so -d "$D"
      "$READELF" -h "$D/lib/arm64-v8a/libg7runner.so" 2>/dev/null | grep -q "AArch64" || bad "libg7runner.so is not AArch64"
      grep -a -q "G7RUN RESULT" "$D/lib/arm64-v8a/libg7runner.so" || bad "libg7runner.so is not the G7 runner"
      grep -a -q "g7-result:" "$D/lib/arm64-v8a/libg7runner.so" || bad "libg7runner.so lacks the failure-summary line (not the execution build)"
    fi
    ;;
  *) echo "usage: $0 general|exec <apk>"; exit 64;;
esac
[ $fail = 0 ] && echo "OK  $M APK: G7 execution build content as required"
exit $fail
