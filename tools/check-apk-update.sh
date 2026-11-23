#!/bin/bash
# Before installing an APK over an installed one: compare versionCodes. Android refuses a LOWER code (downgrade); equal or higher updates in place and keeps the app data.
# usage: tools/check-apk-update.sh <installed-versionCode> <apk>      (installed code: adb shell dumpsys package dev.muns.hengeprobe | grep versionCode)
# AAPT2 may name the aapt2 binary. Exit 0 = may install over it; exit 2 = do NOT install (lower); exit 1 = usage/read error. Nothing is installed, deleted or reset.
set -u
INST=${1:-}; APK=${2:-}
case "$INST" in ''|*[!0-9]*) echo "usage: $0 <installed-versionCode> <apk>" >&2; exit 1;; esac
[ -f "$APK" ] || { echo "no such apk: $APK" >&2; exit 1; }
AAPT2=${AAPT2:-aapt2}
NEW=$("$AAPT2" dump badging "$APK" 2>/dev/null | sed -n "s/^package:.* versionCode='\([0-9]*\)'.*/\1/p" | head -1)
case "$NEW" in ''|*[!0-9]*) echo "could not read the versionCode of $APK" >&2; exit 1;; esac
if [ "$NEW" -lt "$INST" ]; then
  echo "DO NOT INSTALL: this APK has versionCode $NEW, lower than the installed $INST. Android refuses a downgrade; installing it would need an uninstall, which deletes the app data (ROM, IPSW, ECID). Use a newer build."
  exit 2
fi
if [ "$NEW" -eq "$INST" ]; then echo "same versionCode ($NEW): installs over the installed app (reinstall)."; else echo "OK: $NEW is higher than the installed $INST; installs over it and keeps the app data."; fi
exit 0
