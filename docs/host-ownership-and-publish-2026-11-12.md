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

## Addendum: run() entry serialized with abort_start (reviewer 907ae3ca / 761b06f8 / 9f1186ad)
- hv_restore_session_run(): `owner AND registry ACTIVE AND hv_net_guard_engine_enter()` are ONE g_own_mu section; the engine runs outside the lock. abort_start() takes the same lock, so either the engine is already counted (abort refused) or the abort came first (run refused). New lock edge g_own_mu -> net_guard mutex; net_guard never takes g_own_mu. engine_enter is called only from run().
- abort_start() order kept (abandon, then uninstall). uninstall fails only while an engine is registered; entry is serialized, so after the count check it cannot. The return value is checked as a defensive assertion (counter hv_restore_session_abort_uninstall_failures, must stay 0); abandon is NOT undone if it ever fired, so the invariant, not the check, is the protection.
- Contract: only the owner (starter, or the restore thread after claim) may call run(). install.c already claimed first; restore_preflight.c now claims in its restore thread and that thread ends the session; tests were moved to the contract (claim before run) without removing the thread race.
- Observation counters: hv_restore_session_run_denied_owner_state()/..._engine().
- Tests added: abort_race (400 rounds abort_start vs claim+run, never a leftover policy/engine/non-IDLE), pubrights (publish right: none early, not for other threads, once, not carried to the next generation), fdr-session stranger run refused, 400-round claim+run vs old owner's end().
- Note: end()/abort_start() answer 0 when the generation is already gone (no-op), so tests tell the winner by whether the claim happened.
- hv_reg_cleanup_resolve() has no host call site yet (nothing resolves cleanup bits today), so "resolve only after real release" is a rule for the I3~I6 callers, not yet exercised.
- Not rerun: registry ASan/TSan (registry code unchanged this round). ASan leaks / TSan races in urlasset_tests are vendored restore.c, as before.
