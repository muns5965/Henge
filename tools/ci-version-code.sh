#!/bin/bash
# The ONE versionCode rule of every CI-built APK (the general APK and the G7 execution APK): seconds since 2026-01-01 00:00:00 UTC.
# Both workflows call this script and pass the value to gradle as HENGE_VERSION_CODE, so the two APKs share one scale (the run number is per workflow and cannot be compared).
# A build made later always gets a higher code (one second apart is enough; two builds in the very same second get EQUAL codes, which Android still installs over each other:
# only a LOWER code is refused). Installing an OLDER build after a newer one is a downgrade and Android refuses it; tools/check-apk-update.sh tells that before installing.
# Optional argument: a UTC epoch second to convert (tests); default is now. Prints one integer; exit 1 if it is not in 1..2147483647.
set -u
BASE=1767225600
NOW=${1:-$(date -u +%s)}
case "$NOW" in ''|*[!0-9]*) echo "ci-version-code: not an epoch second: $NOW" >&2; exit 1;; esac
V=$((NOW - BASE))
if [ "$V" -lt 1 ] || [ "$V" -gt 2147483647 ]; then echo "ci-version-code: $V is outside 1..2147483647" >&2; exit 1; fi
echo "$V"
