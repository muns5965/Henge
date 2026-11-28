/* Mouse/keyboard virtio-input event queue (devices/virtio_input.c) without a VM: order of events, SYN framing, no-buffer behaviour, ring overflow, short buffers.
   The virtqueue is replaced by a recorder: `bufs` = how many guest event buffers are posted, `bufsize` = their writable size. Guest consumption is NOT tested. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../devices/virtio_input.c"

void hv_virtio_init(HvVirtio *v, HvMachine *m, int id, uint32_t cc, int nq, uint64_t f) { (void)id; (void)cc; (void)nq; (void)f; v->m = m; }
int hv_pcie_attach(HvPcie *p, HvPciDev *d) { (void)p; (void)d; return 0; }
void hv_machine_wake(HvMachine *m) { (void)m; }
static int bufs, bufsize = 8, irqs;
static uint8_t got[8192][8]; static int ngot;
int hv_virtq_pop(HvVirtio *v, int q, HvVirtqElem *e) { (void)v; (void)q; (void)e; if (!bufs) return 0; bufs--; return 1; }
uint32_t hv_virtq_write(HvVirtio *v, const HvVirtqElem *e, uint32_t off, const void *b, uint32_t len) { (void)v; (void)e; (void)off; uint32_t n = len < (uint32_t)bufsize ? len : (uint32_t)bufsize; if (n == 8 && ngot < 8192) memcpy(got[ngot++], b, 8); return n; }
void hv_virtq_push(HvVirtio *v, int q, const HvVirtqElem *e, uint32_t w) { (void)v; (void)q; (void)e; (void)w; }
void hv_virtio_interrupt(HvVirtio *v) { (void)v; irqs++; }

static int bad;
#define EXPECT(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) bad++; } while (0)
static HvVirtioInput *mk(void) { HvVirtioInput *d = calloc(1, sizeof *d); pthread_mutex_init(&d->mu, NULL); d->vio.status = 4; d->vio.q[EVENTQ].enabled = 1; bufs = 0; bufsize = 8; irqs = 0; ngot = 0; return d; }
static uint16_t T(int i) { uint16_t v; memcpy(&v, got[i], 2); return v; }
static uint16_t C(int i) { uint16_t v; memcpy(&v, got[i] + 2, 2); return v; }
static int32_t V(int i) { int32_t v; memcpy(&v, got[i] + 4, 4); return v; }

int main(void) {
    HvVirtioInput *d = mk();
    /* what hv_vm_mouse_move / hv_vm_mouse_button send */
    hv_virtio_input_send(d, HV_EV_REL, HV_REL_X, 5); hv_virtio_input_send(d, HV_EV_REL, HV_REL_Y, -3); hv_virtio_input_send(d, HV_EV_SYN, 0, 0);
    hv_virtio_input_send(d, HV_EV_KEY, HV_BTN_LEFT, 1); hv_virtio_input_send(d, HV_EV_SYN, 0, 0);
    hv_virtio_input_send(d, HV_EV_KEY, HV_BTN_LEFT, 0); hv_virtio_input_send(d, HV_EV_SYN, 0, 0);
    hv_virtio_input_poll(d);
    EXPECT(d->delivered == 0 && ngot == 0 && d->tail - d->head == 7 && irqs == 0, "no guest buffer posted: nothing delivered, 7 events kept in order, no interrupt");
    bufs = 100; hv_virtio_input_poll(d);
    EXPECT(d->delivered == 7 && ngot == 7 && irqs == 1, "buffers arrive: 7 events delivered in one batch, one interrupt");
    EXPECT(T(0) == HV_EV_REL && C(0) == HV_REL_X && V(0) == 5 && T(1) == HV_EV_REL && C(1) == HV_REL_Y && V(1) == -3 && T(2) == HV_EV_SYN, "move: REL_X, REL_Y (negative value kept), then SYN");
    EXPECT(T(3) == HV_EV_KEY && C(3) == HV_BTN_LEFT && V(3) == 1 && T(4) == HV_EV_SYN && T(5) == HV_EV_KEY && C(5) == HV_BTN_LEFT && V(5) == 0 && T(6) == HV_EV_SYN, "button: left down + SYN, left up + SYN, in that order");
    free(d); d = mk(); bufs = 10; bufsize = 4;
    hv_virtio_input_send(d, HV_EV_REL, HV_REL_X, 1); hv_virtio_input_poll(d);
    EXPECT(d->delivered == 0 && d->short_writes == 1 && bufs == 9, "a guest buffer smaller than 8 bytes is a short write, not a delivery (and the event is consumed)");
    free(d); d = mk();
    for (int i = 0; i < HV_VINPUT_RING + 3; i++) hv_virtio_input_send(d, HV_EV_REL, HV_REL_X, i);
    EXPECT(d->dropped == 3 && d->tail - d->head == HV_VINPUT_RING, "ring full (4096) while the guest takes nothing: the 3 OLDEST events are dropped and counted");
    bufs = 5000; hv_virtio_input_poll(d);
    EXPECT(ngot == HV_VINPUT_RING && V(0) == 3, "after the drop the first delivered event is #3 (oldest 0..2 lost)");
    printf("note: dropping the oldest event can drop a SYN or a button edge; this only happens when the guest has not posted buffers for 4096 events.\n");
    free(d);
    printf(bad ? "FAIL: %d\n" : "ALL PASS\n", bad);
    return bad != 0;
}
