/* Keyboard report path of devices/xhci.c (hv_xhci_key / hv_xhci_key_linux) without a VM: the report sequence for key down/up,
   modifiers, rollover, queue-full coalescing and the Linux->HID table. Guest consumption is NOT tested here (needs a guest). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../devices/xhci.c"

void hv_pci_init(HvPciDev *d, uint16_t v, uint16_t dv, uint32_t c, uint16_t sv, uint16_t si) { (void)d; (void)v; (void)dv; (void)c; (void)sv; (void)si; }
void hv_pci_add_bar(HvPciDev *d, int b, uint64_t s, int i) { (void)d; (void)b; (void)s; (void)i; }
int hv_pcie_attach(HvPcie *p, HvPciDev *d) { (void)p; (void)d; return 0; }
void hv_pci_set_intx(HvPciDev *d, int l) { (void)d; (void)l; }
/* flat 1 MiB of "guest RAM" so the delivery path (transfer ring -> buffer -> event ring) really runs */
static uint8_t gmem[1 << 20];
void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t len) { (void)m; return pa + len <= sizeof gmem ? gmem + pa : NULL; }
int hv_phys_read(HvMachine *m, uint64_t pa, void *b, unsigned l) { (void)m; if (pa + l > sizeof gmem) return -1; memcpy(b, gmem + pa, l); return 0; }
int hv_phys_write(HvMachine *m, uint64_t pa, const void *b, unsigned l) { (void)m; if (pa + l > sizeof gmem) return -1; memcpy(gmem + pa, b, l); return 0; }
static int wakes;
void hv_machine_wake(HvMachine *m) { (void)m; wakes++; }

static int bad;
#define EXPECT(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) bad++; } while (0)

static HvXhci *mk(void) { HvXhci *x = calloc(1, sizeof *x); pthread_mutex_init(&x->mu, NULL); return x; }
static const uint8_t *rep(HvXhci *x, unsigned i) { return x->reports[(x->rhead + i) % HV_XHCI_REPORTS]; }
static unsigned pend(HvXhci *x) { return x->rtail - x->rhead; }

/* A device as the guest leaves it after enumeration: slot 1 = keyboard, endpoint IN running, ring at 0x2000, event ring (16 entries) at 0x4000. */
#define RING 0x2000u
#define EVR 0x4000u
#define BUFS 0x8000u
static HvXhci *mk_ready(void) {
    HvXhci *x = mk(); memset(gmem, 0, sizeof gmem);
    x->kbd_slot = 1; x->ep_running[KBD_DCI_IN] = 1; x->ep[KBD_DCI_IN].dequeue = RING; x->ep[KBD_DCI_IN].ccs = 1;
    x->intr[0].erstsz = 1; x->intr[0].erstba = 0x3000; x->intr[0].seg_base = EVR; x->intr[0].seg_size = 16; x->intr[0].pcs = 1;
    uint64_t ent[2] = {EVR, 16}; memcpy(gmem + 0x3000, ent, 16);
    return x;
}
/* the guest driver posts n Normal TRBs (8-byte buffers at BUFS + 16*i) starting at ring index `from`, cycle bit 1, IOC */
static void post(unsigned from, unsigned n) {
    for (unsigned i = from; i < from + n; i++) { uint32_t t[4] = {BUFS + 16 * i, 0, 8, (uint32_t)TRB_NORMAL << 10 | 1u | 1u << 5}; memcpy(gmem + RING + 16 * i, t, 16); }
}
static uint32_t ev_w(unsigned i, int w) { uint32_t v; memcpy(&v, gmem + EVR + 16 * i + 4 * w, 4); return v; }

int main(void) {
    HvXhci *x = mk_ready();
    hv_xhci_key_linux(x, 28, 1); hv_xhci_key_linux(x, 28, 0);
    hv_xhci_poll(x);
    EXPECT(x->delivered == 0 && pend(x) == 2 && ev_w(0, 3) == 0, "no transfer buffer posted: nothing delivered, both reports stay pending, no event (the 'guest never posts buffers' signature)");
    post(0, 3); hv_xhci_poll(x);
    EXPECT(x->delivered == 2 && pend(x) == 0 && x->ep[KBD_DCI_IN].dequeue == RING + 32, "guest posts 3 buffers: 2 reports delivered, ring dequeue advanced 2, third buffer left for the next key");
    EXPECT(gmem[BUFS + 2] == 0x28 && gmem[BUFS + 16 + 2] == 0 && gmem[BUFS + 16] == 0, "buffer 0 holds the Enter-down report (usage 0x28), buffer 1 the all-zero key-up report");
    EXPECT((ev_w(0, 3) >> 10 & 0x3f) == TRB_TRANSFER_EVENT && (ev_w(0, 3) & 1) == 1 && (ev_w(0, 2) >> 24) == CC_SUCCESS && (ev_w(0, 3) >> 24) == 1 && (ev_w(0, 3) >> 16 & 0x1f) == KBD_DCI_IN && ev_w(0, 0) == RING, "completion event 0: transfer event, success, slot 1, endpoint IN, points at TRB 0, producer cycle 1");
    EXPECT(ev_w(1, 0) == RING + 16 && (ev_w(2, 3) & 1) == 0, "event 1 points at TRB 1; no third event");
    EXPECT((x->intr[0].iman & 1) && (x->usbsts & 8), "interrupt pending (IP) and EINT set after the events");
    hv_xhci_key_linux(x, 57, 1); hv_xhci_poll(x);
    EXPECT(x->delivered == 3 && gmem[BUFS + 32 + 2] == 0x2c && ev_w(2, 0) == RING + 32, "a later key (Space) is delivered into the third buffer once it exists");
    free(x);
    /* event ring wrap flips the producer cycle bit */
    x = mk_ready(); post(0, 20);
    for (int i = 0; i < 17; i++) hv_xhci_key(x, 4, (i & 1) ^ 1);
    hv_xhci_poll(x);
    EXPECT(x->delivered == 17 && x->intr[0].pcs == 0 && (ev_w(0, 3) & 1) == 0 && (ev_w(1, 3) & 1) == 1 && (ev_w(15, 3) & 1) == 1, "17 events into a 16-entry ring: entries 1..15 keep cycle 1, the 17th overwrote entry 0 with cycle 0, producer cycle is now 0");
    free(x);
    /* queue full while the guest posts nothing, then buffers arrive: backlog drains, the last delivered report is the FINAL key state */
    x = mk_ready();
    for (unsigned i = 0; i < HV_XHCI_REPORTS + 6; i++) hv_xhci_key(x, 5, (int)(i & 1) ^ 1);   /* 262 changes, the last (i=261) is a key-up */
    post(0, 8); hv_xhci_poll(x);
    EXPECT(x->coalesced == 6 && pend(x) == HV_XHCI_REPORTS - 8 && x->delivered == 8, "256 queued + 6 coalesced; 8 buffers drain 8 reports");
    for (unsigned i = 8; i < HV_XHCI_REPORTS; i += 8) { post(i, 8); hv_xhci_poll(x); if (i + 8 >= 256) break; }
    EXPECT(x->delivered == 256 && !pend(x) && gmem[BUFS + 16 * 255 + 2] == 0, "after the guest posts enough buffers all 256 reports arrive and the last one is the key-up (no stuck key)");
    free(x);
    x = mk();

    hv_xhci_key_linux(x, 28, 1); hv_xhci_key_linux(x, 28, 0);
    EXPECT(pend(x) == 2 && rep(x, 0)[2] == 0x28 && !rep(x, 1)[2] && !rep(x, 1)[0], "Enter: down report carries usage 0x28, up report is all zero");
    free(x); x = mk();
    hv_xhci_key_linux(x, 42, 1); hv_xhci_key_linux(x, 15, 1); hv_xhci_key_linux(x, 15, 0); hv_xhci_key_linux(x, 42, 0);
    EXPECT(pend(x) == 4 && rep(x, 0)[0] == 0x02 && rep(x, 1)[0] == 0x02 && rep(x, 1)[2] == 0x2b && rep(x, 2)[0] == 0x02 && !rep(x, 2)[2] && !rep(x, 3)[0], "Shift+Tab: modifier bit 0x02 held across the Tab report, released last");
    free(x); x = mk();
    hv_xhci_key(x, 4, 1); hv_xhci_key(x, 5, 1); hv_xhci_key(x, 4, 0);
    EXPECT(rep(x, 1)[2] == 4 && rep(x, 1)[3] == 5 && rep(x, 2)[2] == 5 && !rep(x, 2)[3], "rollover: releasing the first of two keys closes the gap");
    hv_xhci_key(x, 5, 1);
    EXPECT(pend(x) == 4 && rep(x, 3)[2] == 5 && !rep(x, 3)[3], "repeated down of a held key does not duplicate it");
    free(x); x = mk();
    for (int i = 0; i < 7; i++) hv_xhci_key(x, (uint8_t)(4 + i), 1);
    EXPECT(rep(x, 6)[7] == 9 && rep(x, 6)[2] == 4, "seventh simultaneous key is ignored, the six held keys stay");
    free(x); x = mk();
    for (unsigned i = 0; i < HV_XHCI_REPORTS; i++) hv_xhci_key(x, 4, (int)(i & 1) ^ 1);   /* fills the queue, ends with an up */
    EXPECT(pend(x) == HV_XHCI_REPORTS && x->coalesced == 0, "queue filled exactly, nothing coalesced");
    hv_xhci_key(x, 4, 1); hv_xhci_key(x, 4, 0);
    EXPECT(pend(x) == HV_XHCI_REPORTS && x->coalesced == 2 && !rep(x, HV_XHCI_REPORTS - 1)[2], "full queue: final state (key up) survives in the newest report; 2 coalesced");
    free(x); x = mk();
    hv_xhci_key_linux(x, 111, 1);
    EXPECT(rep(x, 0)[2] == 0x4c, "KEY_DELETE (111) -> 0x4c (was unmapped)");
    hv_xhci_key_linux(x, 86, 1); hv_xhci_key_linux(x, 99, 1);
    EXPECT(rep(x, 2)[2] == 0x4c && rep(x, 2)[3] == 0x64 && rep(x, 2)[4] == 0x46, "KEY_102ND (86) -> 0x64, KEY_SYSRQ (99) -> 0x46");
    free(x); x = mk();
    unsigned bad_map = 0; const struct { unsigned c, h; } t[] = {{1,0x29},{14,0x2a},{15,0x2b},{28,0x28},{57,0x2c},{103,0x52},{108,0x51},{105,0x50},{106,0x4f},{29,0xe0},{42,0xe1},{56,0xe2},{125,0xe3},{97,0xe4},{54,0xe5},{100,0xe6},{126,0xe7},{110,0x49},{102,0x4a},{107,0x4d},{104,0x4b},{109,0x4e}};
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (linux_to_hid[t[i].c] != t[i].h) { printf("  map %u -> %02x want %02x\n", t[i].c, linux_to_hid[t[i].c], t[i].h); bad_map++; }
    EXPECT(!bad_map, "Linux->HID table: %zu common keys (Esc, Backspace, Tab, Enter, Space, arrows, modifiers, Home/End/PgUp/PgDn/Insert) correct", sizeof t / sizeof *t);
    hv_xhci_key_linux(x, 0, 1); hv_xhci_key_linux(x, 255, 1); hv_xhci_key_linux(x, 300, 1);
    EXPECT(pend(x) == 0, "unmapped / out-of-range codes produce no report");
    free(x);
    printf(bad ? "FAIL: %d\n" : "ALL PASS\n", bad);
    return bad != 0;
}
