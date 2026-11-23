#!/bin/bash
# Test of tools/ci-version-code.sh (one shared rule, monotonic, bounds) and tools/check-apk-update.sh (refuses a lower versionCode, accepts equal/higher). A fake aapt2 stands in for the SDK tool.
set -u
cd "$(dirname "$0")/.."
W=${1:-build-apkversion}; rm -rf "$W"; mkdir -p "$W"
F=0; ok(){ echo "PASS $1"; }; bad(){ echo "FAIL $1"; F=$((F+1)); }
BASE=1767225600
[ "$(tools/ci-version-code.sh $((BASE+1)))" = 1 ] && ok "first second is 1" || bad "first second"
a=$(tools/ci-version-code.sh $((BASE+1000))); b=$(tools/ci-version-code.sh $((BASE+1001)))
[ "$b" -gt "$a" ] && ok "later build is higher (second resolution)" || bad "monotonic"
[ "$(tools/ci-version-code.sh $((BASE+60)))" = "$(tools/ci-version-code.sh $((BASE+60)))" ] && ok "same second -> equal (installable)" || bad "equal"
tools/ci-version-code.sh "$BASE" >/dev/null 2>&1 && bad "zero accepted" || ok "zero refused"
tools/ci-version-code.sh $((BASE+2147483648)) >/dev/null 2>&1 && bad "too large accepted" || ok "above int max refused"
tools/ci-version-code.sh x >/dev/null 2>&1 && bad "non-number accepted" || ok "non-number refused"
c=$(tools/ci-version-code.sh); [ "$c" -gt 20000000 ] && [ "$c" -lt 2147483647 ] && ok "now is a plausible code ($c)" || bad "now"
# workflows: both use the one script and gradle has no run-number fallback
for w in android-apk android-g7exec; do grep -q "tools/ci-version-code.sh" .github/workflows/$w.yml && ok "$w uses the shared rule" || bad "$w does not use tools/ci-version-code.sh"; done
grep -q "GITHUB_RUN_NUMBER" android/app/build.gradle && bad "gradle still uses the run number" || ok "gradle has no run-number code"
# update check with a fake aapt2
cat > "$W/aapt2" <<'EOS'
#!/bin/bash
echo "package: name='dev.muns.hengeprobe' versionCode='${FAKE_CODE}' versionName='0.2+x' compileSdkVersion='34'"
EOS
chmod +x "$W/aapt2"; : > "$W/x.apk"
run(){ AAPT2="$PWD/$W/aapt2" FAKE_CODE=$1 tools/check-apk-update.sh $2 "$W/x.apk" >"$W/out" 2>&1; echo $?; }
[ "$(run 500 400)" = 0 ] && ok "higher -> may install" || bad "higher"
[ "$(run 400 400)" = 0 ] && ok "equal -> may install" || bad "equal install"
[ "$(run 300 400)" = 2 ] && grep -q "DO NOT INSTALL" "$W/out" && ok "lower -> refused with a clear message" || bad "lower"
grep -q "uninstall" "$W/out" && ok "message names the data-loss consequence" || bad "message"
[ "$(run 500 abc)" = 1 ] && ok "bad installed code -> usage error" || bad "bad arg"
AAPT2=/nonexistent tools/check-apk-update.sh 1 "$W/x.apk" >/dev/null 2>&1; [ $? = 1 ] && ok "unreadable apk -> error, not install" || bad "unreadable"
echo "failures: $F"; [ "$F" = 0 ]
