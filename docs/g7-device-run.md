# G7 on the phone: how the execution build is delivered, started, stopped and read back

**Nothing here starts automatically.** The start switch comes from the build type (`BuildConfig.G7_START_ENABLED`): `false` in `g7exec` (the APK CI publishes on every push: it cannot start the runner), `true` only in `g7start`.
`g7start` has the same files, package and signing key; CI builds and checks it on every run (the dex carries the marker `G7-BUILD-KIND:START-ON`, `g7exec` carries `G7-BUILD-KIND:EXEC-OFF`, the general APK neither), but publishes it only from a MANUAL run of the workflow
"Android G7 execution APK" with `publish_start=true` (artifact `henge-g7-start-apk`). That run is an explicit approval step. Even the switch-ON APK starts nothing by itself: it needs the consent box checked (nothing is typed), the preflight must pass, the IPSW SHA-256 is the built-in reference for macOS 15.6.1 (24G90),
and the runner re-hashes the IPSW before it creates anything (a mismatch prints the expected and the calculated digest and refuses: no disk, no AUX, no Apple request).

## Delivery (separate from the general APK)
- `assembleDebug` (workflow android-apk.yml) is the app everyone installs. It has no `libg7runner.so`, no `G7ExecLauncher`, no `G7ExecActivity` (`tools/check-g7exec-apk.sh general`).
- `assembleG7exec` (workflow android-g7exec.yml, artifact `henge-g7-exec-apk`) is the same package and key, so it installs OVER the general app and keeps its private files
  (ROM, ECID, IPSW, an existing pair). It adds `libg7runner.so` (the execution build of host/g7_runner.c) and one extra launcher icon, "Henge G7 실행".
  To go back, install the next general APK over it (its version number is newer).
- The binary can only be started from the APK's native library directory (Android does not run files from app storage), which is why it travels in an APK.

## What the screen does without approval
- Preflight reads files only: binary present, ROM genuine, ECID readable (never shown), IPSW present with the right size, no install files yet. It never runs the binary.

## Approval boundary (all three, in order)
1. A reviewed change that turns `START_ENABLED` on (its own commit, its own build, its own approval). Until then every start is refused before anything is touched.
2. The consent box on the screen, checked by the person (it lists what will happen: install disk and AUX, writes to storage, Apple requests). Nothing is typed. The IPSW SHA-256 is the built-in reference; the runner hashes the file and refuses on a mismatch.
3. Preflight is clean: if any install file exists (disk, AUX, state), start is refused: no automatic resume, no overwrite. A failed run is kept as it is; a new try needs a new pair
   (quarantine by rename, a separate step).
What a start does: creates the 128 GiB sparse disk and the AUX, runs the machine with writable storage, and contacts gs.apple.com (the three approved TSS hosts) for the restore.

## Stop and result
- 취소 (cancel): SIGTERM to the runner, which asks the restore to stop; after 90 s the process is killed. The pair stays `in_progress`.
- The output is shown on the screen and written to one new file per run, `files/g7_run_<ms>.txt` (never appended to or overwritten; at most 4 MiB, ECID masked). The virtual USB device's `bdif usb diag:` lines (about a dozen per 2 KiB chunk) are thinned: the first 300 are kept, later ones are only counted in `[usb diag: N line(s) not kept; last: …]` summary lines. Every other line is kept. The last line of a run is `g7-result: outcome=… last_step=… pair=… refused_destinations=N`, then `g7-exit: <code>`.
- The top of the screen shows the state (running / finished with the meaning of the exit code / no `g7-exit` in the log = the app was ended and the outcome is unconfirmed), the log name and size, and the last `g7-result:` and `g7-exit:` lines. `로그 파일 공유(전체)` sends the whole newest log file through the share sheet (read-only, through a non-exported provider that only serves `g7_run_<13 digits>.txt`); `로그 끝부분 복사` copies the last 100 KB to the clipboard. The log text on screen is selectable.
- Exit codes: 0 pair finished (evidence only) / 2 refused or failed / 5 the restore did not end (process ended) / 64 bad arguments / 137, 143 ended by a signal.

## Not covered
- Android may end the app (low memory, user swipe). The runner then dies with it; the pair stays `in_progress`, but the machine's last writes are whatever the disk had: an unfinished pair is never reused.
  There is no foreground service yet: keep the screen on and the app in front.
- A new destination the restore asks for is refused and printed; adding it is a separate approval (docs/g7-new-destinations.md).
