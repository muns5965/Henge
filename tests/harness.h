// Shared by the block-level tests: assembler macros for test programs (when
// included from a .s file) and the C side of the native-vs-JIT comparison.
#ifndef HENGEVM_TEST_HARNESS_H
#define HENGEVM_TEST_HARNESS_H

#ifdef __ASSEMBLER__

// Mach-O prefixes C symbols with an underscore and uses @PAGE/@PAGEOFF; ELF
// (Linux, Android) does neither and uses :lo12:.
#if defined(__APPLE__)
#define SYMPFX _
#define SYM(x) _##x
#define PAGE(x) x@PAGE
#define PAGEOFF(x) x@PAGEOFF
#else
#define SYMPFX
#define SYM(x) x
#define PAGE(x) x
#define PAGEOFF(x) :lo12:x
#endif

// PROGRAM name ... END name. The JIT runs from _name to _name_end; natively
// _name_native loads the registers first and the epilogue saves them after.
.macro PROGRAM name
    .p2align 2
    .globl SYMPFX\name\()_native
    .globl SYMPFX\name
    .globl SYMPFX\name\()_end
SYMPFX\name\()_native:
    ldp q0, q1, [x28, #320]
    ldp q2, q3, [x28, #352]
    ldp q4, q5, [x28, #384]
    ldp q6, q7, [x28, #416]
    ldp q8, q9, [x28, #448]
    ldp q10, q11, [x28, #480]
    ldp q12, q13, [x28, #512]
    ldp q14, q15, [x28, #544]
    ldp q16, q17, [x28, #576]
    ldp q18, q19, [x28, #608]
    ldp q20, q21, [x28, #640]
    ldp q22, q23, [x28, #672]
    ldp q24, q25, [x28, #704]
    ldp q26, q27, [x28, #736]
    ldp q28, q29, [x28, #768]
    ldp q30, q31, [x28, #800]
    ldr x0, [x28, #0x340]
    msr fpcr, x0
    ldr x0, [x28, #0x348]
    msr fpsr, x0
    ldr x0, [x28, #0xf8]
    mov sp, x0
    ldr x0, [x28, #0x108]
    msr nzcv, x0
    ldp x0, x1, [x28, #0]
    ldp x2, x3, [x28, #16]
    ldp x4, x5, [x28, #32]
    ldp x6, x7, [x28, #48]
    ldp x8, x9, [x28, #64]
    ldp x10, x11, [x28, #80]
    ldp x12, x13, [x28, #96]
    ldp x14, x15, [x28, #112]
    ldp x16, x17, [x28, #128]
    ldp x18, x19, [x28, #144]
    ldp x20, x21, [x28, #160]
    ldp x22, x23, [x28, #176]
    ldp x24, x25, [x28, #192]
    ldp x26, x27, [x28, #208]
    ldp x29, x30, [x28, #232]
    ldr x28, [x28, #224]
SYMPFX\name\():
.endm

.macro END name
    b SYMPFX\name\()_end
SYMPFX\name\()_end:
    stp x0, x1, [sp, #-16]!
    adrp x0, PAGE(SYM(hv_native_cpu))
    ldr x0, [x0, PAGEOFF(SYM(hv_native_cpu))]
    stp x2, x3, [x0, #16]
    stp x4, x5, [x0, #32]
    stp x6, x7, [x0, #48]
    stp x8, x9, [x0, #64]
    stp x10, x11, [x0, #80]
    stp x12, x13, [x0, #96]
    stp x14, x15, [x0, #112]
    stp x16, x17, [x0, #128]
    stp x18, x19, [x0, #144]
    stp x20, x21, [x0, #160]
    stp x22, x23, [x0, #176]
    stp x24, x25, [x0, #192]
    stp x26, x27, [x0, #208]
    stp x28, x29, [x0, #224]
    str x30, [x0, #240]
    mrs x2, nzcv
    str x2, [x0, #0x108]
    ldp x2, x3, [sp], #16
    stp x2, x3, [x0, #0]
    mov x2, sp
    str x2, [x0, #0xf8]
    mrs x2, fpcr
    str x2, [x0, #0x340]
    mrs x2, fpsr
    str x2, [x0, #0x348]
    stp q0, q1, [x0, #320]
    stp q2, q3, [x0, #352]
    stp q4, q5, [x0, #384]
    stp q6, q7, [x0, #416]
    stp q8, q9, [x0, #448]
    stp q10, q11, [x0, #480]
    stp q12, q13, [x0, #512]
    stp q14, q15, [x0, #544]
    stp q16, q17, [x0, #576]
    stp q18, q19, [x0, #608]
    stp q20, q21, [x0, #640]
    stp q22, q23, [x0, #672]
    stp q24, q25, [x0, #704]
    stp q26, q27, [x0, #736]
    stp q28, q29, [x0, #768]
    stp q30, q31, [x0, #800]
    b SYM(hv_native_return)
.endm

#else

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "dispatch.h"
#include "machine.h"

/* Where a crash happened. Tests set the phase and program name as they go; on Linux and Android a SIGSEGV/SIGBUS/
   SIGILL handler prints them with the fault address, the registers and the memory map entries around them, so a
   crash on a phone leaves something to read. Other hosts do nothing. */
static const char *hv_test_phase = "start", *hv_test_program = "";
#if defined(__linux__) && defined(__aarch64__)
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
static void hv_test_map_line(const char *what, uint64_t addr) {
    char buf[512];
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    while (fgets(buf, sizeof buf, f)) {
        unsigned long lo, hi;
        if (sscanf(buf, "%lx-%lx", &lo, &hi) == 2 && addr >= lo && addr < hi) {
            dprintf(2, "  %s in map: %s", what, buf);
            fclose(f);
            return;
        }
    }
    dprintf(2, "  %s not in any map\n", what);
    fclose(f);
}
static char hv_test_scs[4096] __attribute__((aligned(16)));
static void hv_test_early_write(const char *msg) {
    size_t n = 0;
    while (msg[n]) n++;
    if (write(2, msg, n) < 0) {}
}
static void hv_test_crash(int sig, siginfo_t *si, void *ctx) {
    /* The program under test may have left anything in x18, which Android uses as the shadow call stack pointer
       in libc: give this handler its own before it calls anything. */
    __asm__ volatile("mov x18, %0" ::"r"(hv_test_scs));
    hv_test_early_write("CRASH (handler entered)\n");
    ucontext_t *uc = ctx;
    dprintf(2, "CRASH signal %d code %d fault addr %llx phase %s program %s\n", sig, si->si_code,
            (unsigned long long)(uintptr_t)si->si_addr, hv_test_phase, hv_test_program);
    dprintf(2, "  pc %llx sp %llx lr %llx x18 %llx x28 %llx\n", (unsigned long long)uc->uc_mcontext.pc,
            (unsigned long long)uc->uc_mcontext.sp, (unsigned long long)uc->uc_mcontext.regs[30],
            (unsigned long long)uc->uc_mcontext.regs[18], (unsigned long long)uc->uc_mcontext.regs[28]);
    hv_test_map_line("pc", uc->uc_mcontext.pc);
    hv_test_map_line("fault addr", (uint64_t)(uintptr_t)si->si_addr);
    hv_test_map_line("sp", uc->uc_mcontext.sp);
    _exit(128 + sig);
}
/* Runs before main: if this line never shows, the process died while being loaded. */
__attribute__((constructor, unused)) static void hv_test_before_main(void) { hv_test_early_write("test: before main\n"); }

/* A short trail of what ran last, written at once (only the first few steps, to keep the output short). */
static inline void hv_test_step(const char *what, const char *name) {
    static int n;
    if (n++ >= 8) return;
    hv_test_early_write("step: ");
    hv_test_early_write(what);
    hv_test_early_write(" ");
    hv_test_early_write(name);
    hv_test_early_write("\n");
}

static inline void hv_test_report_crashes(void) {
    hv_test_early_write("test: main entered\n");
    static char alt[65536];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {.sa_flags = SA_SIGINFO | SA_ONSTACK};
    sa.sa_sigaction = hv_test_crash;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
#else
static inline void hv_test_report_crashes(void) {}
static inline void hv_test_step(const char *what, const char *name) { (void)what; (void)name; }
#endif

void hv_native_run(HvCpu *cpu, const void *entry);

typedef struct TestProgram {
    const char *name;
    const void *native, *start, *end;
} TestProgram;

#define TEST_PROGRAM(n) {#n, (const void *)n##_native, (const void *)n, (const void *)n##_end}
#define DECLARE_PROGRAM(n) extern const char n##_native[], n[], n##_end[]

/* Compare everything a program can change. Returns the number of differences. */
static inline int compare_cpus(const char *name, const HvCpu *want, const HvCpu *got) {
    int diffs = 0;
    for (int i = 0; i < 31; i++)
        if (want->x[i] != got->x[i] && i != 18) { /* the host's x18 is the platform's: macOS may clear it when it preempts the native run */
            printf("  %s: x%d native %016llx jit %016llx\n", name, i, (unsigned long long)want->x[i],
                   (unsigned long long)got->x[i]);
            diffs++;
        }
    if (want->sp != got->sp) {
        printf("  %s: sp native %016llx jit %016llx\n", name, (unsigned long long)want->sp, (unsigned long long)got->sp);
        diffs++;
    }
    if ((want->nzcv ^ got->nzcv) & 0xf0000000) {
        printf("  %s: nzcv native %08llx jit %08llx\n", name, (unsigned long long)want->nzcv,
               (unsigned long long)got->nzcv);
        diffs++;
    }
    for (int i = 0; i < 32; i++)
        if (want->v[i] != got->v[i]) {
            printf("  %s: q%d native %016llx%016llx jit %016llx%016llx\n", name, i,
                   (unsigned long long)(want->v[i] >> 64), (unsigned long long)want->v[i],
                   (unsigned long long)(got->v[i] >> 64), (unsigned long long)got->v[i]);
            diffs++;
        }
    if (want->fpsr != got->fpsr || want->fpcr != got->fpcr) {
        printf("  %s: fpsr/fpcr native %llx/%llx jit %llx/%llx\n", name, (unsigned long long)want->fpsr,
               (unsigned long long)want->fpcr, (unsigned long long)got->fpsr, (unsigned long long)got->fpcr);
        diffs++;
    }
    return diffs;
}

/* Deterministic starting registers, different per test. */
static inline void seed_cpu(HvCpu *cpu, uint64_t seed, uint64_t sp) {
    uint64_t s = seed * 0x9e3779b97f4a7c15ull + 1;
    for (int i = 0; i < 31; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        cpu->x[i] = s;
    }
    for (int i = 0; i < 32; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        cpu->v[i] = (__uint128_t)s << 64 | (s * 31);
    }
    cpu->sp = sp;
    cpu->nzcv = 0;
    cpu->fpcr = 0;
    cpu->fpsr = 0;
}

/* Native reference. Darwin may clear x18 on a context switch, so run until two
   runs agree. reset (may be NULL) puts guest memory back before each run. */
static inline void run_native(const TestProgram *p, const HvCpu *init, HvCpu *out, void (*reset)(void)) {
    HvCpu a, b;
    hv_test_phase = "native";
    hv_test_program = p->name;
    for (int tries = 0; tries < 10; tries++) {
        if (reset) reset();
        a = *init;
        hv_test_step("native run begin", p->name);
        hv_native_run(&a, p->native);
        hv_test_step("native run end", p->name);
        if (reset) reset();
        b = *init;
        hv_native_run(&b, p->native);
        if (memcmp(a.x, b.x, sizeof a.x) == 0 && a.v[0] == b.v[0]) break;
    }
    *out = a;
}

/* Through the JIT: code runs in place, guest PA = host address. */
static inline int run_jit(HvMachine *m, const TestProgram *p, const HvCpu *init, HvCpu *out,
                          void (*setup)(HvCpu *)) {
    HvCpu *cpu = out;
    hv_test_phase = "jit";
    hv_test_program = p->name;
    hv_cpu_reset(cpu, m);
    memcpy(cpu->x, init->x, sizeof cpu->x);
    memcpy(cpu->v, init->v, sizeof cpu->v);
    cpu->sp = init->sp;
    cpu->nzcv = init->nzcv;
    cpu->fpcr = init->fpcr;
    cpu->fpsr = init->fpsr;
    cpu->cpacr_el1 = 3u << 20; /* FP enabled */
    if (setup) setup(cpu);
    cpu->pc = (uint64_t)(uintptr_t)p->start;
    m->stop_pc = (uint64_t)(uintptr_t)p->end;
    hv_test_step("jit run begin", p->name);
    int r = hv_run(cpu);
    hv_test_step("jit run end", p->name);
    if (r != HV_EXIT_HALT) {
        printf("  %s: dispatcher returned %d at pc %llx (insn %08x) esr %llx far %llx elr %llx\n", p->name, r,
               (unsigned long long)cpu->pc, cpu->exit_insn, (unsigned long long)cpu->esr_el1,
               (unsigned long long)cpu->far_el1, (unsigned long long)cpu->elr_el1);
        return 1;
    }
    return 0;
}

#endif
#endif
