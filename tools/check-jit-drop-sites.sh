#!/bin/bash
# g7-jit2: static checks for the translation cache's drop paths and the g7-jit2 counters (no arm64 needed).
#  1. the only places that request or perform a whole flush, reset the code region or bump a page generation are the expected ones,
#  2. no TLBI path (hv_tlbi_apply, the hv_tlb_flush*/hv_ijc_* family it calls) can reach any of them,
#  3. the old stats fields are still there (names kept) and the new ones are in the stats lines,
#  4. with aarch64-linux-gnu-gcc: core/*.c still compiles with -Wall -Wextra.
# Usage: tools/check-jit-drop-sites.sh
set -u
cd "$(dirname "$0")/.."
rc=0
fail() { echo "FAIL  $*"; rc=1; }
ok() { echo "ok    $*"; }

# 1. drop sites: file:text, normalized
pat='flush_pending *= *1|flush_pending, 1,|hv_flush_blocks\(m\)|hv_jit_reset\(&m->jit\)|__atomic_add_fetch\(&r->page_gen'
got=$(grep -rnE "$pat" core host --include=*.c | grep -v '^[^:]*:[0-9]*: *\(/\*\|//\|\*\)' | sed -E 's/^([^:]*):[0-9]+: */\1: /; s/ +/ /g' | sort)
want=$(cat <<'EOL' | sort
core/dispatch.c: __atomic_store_n(&m->flush_pending, 1, __ATOMIC_SEQ_CST);
core/dispatch.c: hv_flush_blocks(m);
core/dispatch.c: hv_jit_reset(&m->jit);
core/machine.c: __atomic_add_fetch(&r->page_gen[pg], 1, __ATOMIC_SEQ_CST);
core/machine.c: __atomic_store_n(&m->flush_pending, 1, __ATOMIC_SEQ_CST);
core/sysreg.c: cpu->m->flush_pending = 1;
host/main.c: hm->flush_pending = 1;
host/main.c: mm->flush_pending = 1;
EOL
)
if [ "$got" = "$want" ]; then ok "drop sites are exactly the expected 8 (lookup/flush in dispatch.c, hv_knob_set and hv_invalidate_code in machine.c, HV_IC_FLUSH in sysreg.c, the Mac tool in host/main.c)"; else fail "drop sites changed:"; diff <(echo "$want") <(echo "$got"); fi

# 2. function bodies that TLBI reaches
body() { awk -v f="$2" '$0 ~ "^[a-z].* \\**" f "\\(" && !/;$/ {on=1} on {print} on && /^}/ {exit}' "$1"; }
forbid='flush_pending|hv_flush_blocks|hv_jit_reset|page_gen|hv_invalidate_code|->hash\b|nblocks|jit\.used|jit_gen'
for spec in "core/sysreg.c hv_tlbi_apply" "core/mmu.c hv_tlb_flush" "core/mmu.c hv_tlb_flush_user" "core/mmu.c hv_tlb_flush_va_page" "core/mmu.c hv_tlb_flush_va_asid" \
            "core/mmu.c hv_tlb_flush_kernel_range" "core/mmu.c hv_tlb_flush_user_range" "core/mmu.c hv_tlb_stash_drop" "core/mmu.c hv_ijc_drop_asid" "core/mmu.c hv_ijc_drop_range" "core/mmu.c hv_ijc_reset"; do
  set -- $spec
  b=$(body "$1" "$2")
  if [ -z "$b" ]; then fail "$2 not found in $1"; continue; fi
  if echo "$b" | grep -En "$forbid" >/dev/null; then fail "$2 ($1) touches blocks/region/page generations:"; echo "$b" | grep -En "$forbid"; else ok "$2 touches no block, region or page generation ($(echo "$b" | wc -l) lines)"; fi
done

# 3. stats names
for f in flush_smc flush_ic flush_full flushed_blocks flushed_words flush_emit_fail flush_table_full translate_ticks translated_insns translated_words retrans_va retrans_vapa; do
  grep -q "\b$f\b" core/machine.h || fail "old field $f gone from machine.h"
done; ok "old stats fields present"
for f in flush_code_region_full flush_pending_at_lookup flush_pending_at_publish flush_req_other flush_requests flush_req_during_stw duplicate_translation_count duplicate_translation_words duplicate_translation_ticks \
         unpublished_count published_words first_translation_ticks retranslation_ticks exact_first_ticks exact_retrans_ticks live_words_at_flush_sum exact_again_same_gen exact_gap; do
  grep -q "\b$f\b" core/machine.h || fail "new field $f missing from machine.h"
  grep -q "\b$f\b" core/dispatch.c || fail "new field $f not used in dispatch.c (stats line)"
done; ok "new stats fields declared and printed"
# the request counters add up: every setter of a flush request goes through note_flush_request or bumps flush_requests itself
n=$(grep -c "note_flush_request(" core/dispatch.c); [ "$n" -ge 6 ] && ok "note_flush_request sites: $((n - 1)) (definition excluded)" || fail "note_flush_request sites fewer than expected ($n)"

# 3b. integ7b (T1): the emit-failure end path. A NULL from lookup() is acted on only through hv_lookup_null_action (explicit outcome), a give-up stops the WHOLE machine through the
#     existing hv_machine_halt, and HV_TR_E_GROUP cannot be a final failure (the last emit attempt is always the non-group one).
grep -q 'hv_lookup_null_action(li.outcome' core/dispatch.c && ok "run_loop decides a NULL lookup by the explicit outcome" || fail "run_loop does not use hv_lookup_null_action(li.outcome, ...)"
if grep -n 'if (m->flush_pending || __atomic_load_n(&m->stw_active, __ATOMIC_SEQ_CST) || m->stats.block_flushes != flushes) continue;' core/dispatch.c >/dev/null; then fail "the old inferred retry rule is back in run_loop"; else ok "the old inferred retry rule is gone from run_loop"; fi
n=$(grep -c 'return HV_EXIT_UNSUPPORTED' core/dispatch.c); u=$(grep -B3 'return HV_EXIT_UNSUPPORTED' core/dispatch.c | grep -c 'engine_fatal_stop')
[ "$n" -ge 1 ] && [ "$u" -ge 1 ] && ok "the cannot-translate return is preceded by engine_fatal_stop" || fail "a cannot-translate exit without engine_fatal_stop (returns $n, with stop $u)"
b=$(body core/dispatch.c engine_fatal_stop); echo "$b" | grep -q 'hv_machine_halt(m, HV_HALT_ENGINE)' && echo "$b" | grep -q 'hv_machine_kick_all' && ok "engine_fatal_stop halts every CPU with the existing hv_machine_halt and kicks them" || fail "engine_fatal_stop does not halt the machine"
grep -q 'engine-fatal' host/ui_runner_vm_clone.c && grep -q 'outcome = !confirmed ? "not-confirmed" : engine_fatal ? "engine-fatal"' host/ui_runner_vm_clone.c && grep -q 'engine_fatal ? 4 : 0' host/ui_runner_vm_clone.c && ok "the clone runner reports stop_kind/outcome engine-fatal and exit status 4" || fail "the clone runner does not report the engine-fatal stop"
# HV_TR_E_GROUP: returned only under !e->nogroup, and hv_translate_emit's last attempt is nogroup=1
eb=$(body core/translate.c hv_translate_emit)
if grep -q 'if (!e->nogroup && e->ngrp && emit_group_copies(e, code, b->va, i) != 0) return HV_TR_E_GROUP;' core/translate.c && echo "$eb" | tail -3 | grep -q 'return translate_emit(m, b, code, avail, 1);'; then ok "HV_TR_E_GROUP is never a final failure (guarded by !nogroup; the last attempt is the plain one)"; else fail "HV_TR_E_GROUP reachability changed: check hv_translate_emit"; fi

# 4. compile check
if command -v aarch64-linux-gnu-gcc >/dev/null; then
  for f in dispatch machine mmu sysreg translate memop; do
    out=$(aarch64-linux-gnu-gcc -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -fsyntax-only -Icore -Ihost -Idevices core/$f.c 2>&1)
    if [ -n "$out" ]; then fail "core/$f.c does not compile cleanly"; echo "$out" | head -5; fi
  done; ok "core/*.c compile for aarch64 (syntax only) with -Wall -Wextra"
else echo "skip  no aarch64-linux-gnu-gcc"; fi
exit $rc
