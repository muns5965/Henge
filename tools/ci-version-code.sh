#!/bin/bash
# The ONE versionCode rule of every CI-built APK (the general APK and the G7 execution APK): seconds since 2026-01-01 00:00:00 UTC of the COMMIT time (git committer date of HEAD).
# Both workflows call this script and pass the value to gradle as HENGE_VERSION_CODE. The same commit therefore gets the SAME code in both APKs, whichever workflow finishes first
# (a build-time rule would give the later-finishing APK the higher code and break the update in the other direction); a newer commit gets a higher code. Equal codes install over each other;
# only a LOWER code is refused. A commit with an older committer date (rebase, cherry-pick) can get a lower code: tools/check-apk-update.sh tells that before installing.
# Optional argument: an epoch second to convert (tests); default is the committer date of HEAD. Prints one integer; exit 1 if it is not in 1..2147483647.
set -u
BASE=1767225600
NOW=${1:-$(git log -1 --format=%ct 2>/dev/null)}
case "$NOW" in ''|*[!0-9]*) echo "ci-version-code: not an epoch second: $NOW" >&2; exit 1;; esac
V=$((NOW - BASE))
if [ "$V" -lt 1 ] || [ "$V" -gt 2147483647 ]; then echo "ci-version-code: $V is outside 1..2147483647" >&2; exit 1; fi
echo "$V"
