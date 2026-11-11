# Host: ownership hand-over, publication, cleanup history (2026-11-12)

Scope: host-only (restore_registry, restore_session, install.c Android branch). No vendored changes; I3~I6 not approved, I10 deferred. D1~D8, C1~C19 unchanged.

## Ownership
- begin() caller = owner. One hand-over to the restore thread: approve_successor() issues a token (owner only, once); claim(token) swaps owner and consumes the token.
- claim() and close()'s (owner check + close_begin) both run inside one g_own_mu section, so once a transfer is claimable no other thread can enter CLOSING between the ACTIVE check and the owner swap (reviewer f0cefe6a invariant). The wait for quiescence runs outside g_own_mu.
- Lock order: g_own_mu -> registry mutex only. The registry never takes g_own_mu.
- abort_start(): owner && not transferred && no engine/registered work && hv_reg_abandon() ok; only after abandon succeeds are token/owner/gen released. On failure the starter keeps ownership.
- cancel() is not owner-gated; refused only inside the publish callback (TLS t_in_publish).

## Publication
finalize (once per generation, records the result, grants a one-time publish right) -> publish(fn,arg) (TLS in_publish around the callback, consumes right, PUBLISHING->PUBLISHED) -> release_idle().

## Cleanup history vs current
Bits HOST_THREAD=1, POLICY=2, OBJECT=4, OTHER=8. History (OR, diagnosis only) vs current (cleared by resolve).
release_idle requires: PUBLISHED AND current bits 0 AND objects released AND final result != CLEANUP_FAILED.
A confirmed CLEANUP_FAILED never returns to IDLE until app restart, even if resources are freed late; the final result is never changed.

## Verification (this machine, Linux, fake devices)
- registry tests (plain/hooks, ASan, TSan), session-reg tests (14 cases incl. xfer_race 300+300 rounds), install-start tests (7 cases): all pass in plain, ASan, TSan.
- fdr-session 400-round run/end race: pass (ran 187, refused 213), no test call changed.
- netguard, preflight, check-no-test-flags: pass.
- fake-device regression equals baseline: same return values; known vendored findings still reproduce (ASan t5a_free, TSan t5a_free/t13; t5s_bb and t12 hang = baseline T5/T12).

## Not verified / remaining risks
- Token proves the right, not the caller identity.
- Publish callback has no time limit (hang keeps PUBLISHING, refuses next session).
- install.c Android branch only syntax-checked and tested via Linux -D__ANDROID__ wraps; not run on a device.
- Time constants (close 15000 ms, mux stop 5000 ms) are provisional, unmeasured.
- usbmuxd is not in the CLOSED scope; recheck against C13 when I3~I6 register it.
- CI is build-only.
