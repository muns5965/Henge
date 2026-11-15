/* C1-2 (B-track addition): did a picture the REAL engine's guest code drew ever reach the shared file?
   c1vm-test.sh's existing real-engine runs use a stand-in ROM that is one instruction (`b .`, branch to itself) and so never draws
   anything; the strace checks only say the process stayed inside its safety rules, not that a frame ever crossed
   host/display_buf.h -> host/vm.c's hv_vm_boot_display -> host/runner_vm_core.c's fetch_cb -> hv_shm_publish.
   tests/roms/frame_probe.S (built by tools/build-frame-probe-rom.sh) is still a stand-in, not Apple firmware: real ARM64
   instructions that write one synthetic "boot_args" block (the same shape iBoot hands the kernel; see the field offsets in
   host/vm.c's hv_vm_boot_display) into guest RAM, naming a small framebuffer, fill that framebuffer with one repeated 32-bit
   word, then loop forever. Nothing it does touches a file, the network, or any Apple data.
   This program is the other half: open the shared file ui_runner_vm already publishes to, wait for a picture, and check its
   size and every pixel against exactly what frame_probe.S wrote. A pass means the whole path carried real engine-drawn pixels
   byte for byte; this says nothing about a real OS or a real Apple ROM.
   Usage: frame_probe_check <shm path> <width> <height> <pattern hex, e.g. aabbccdd> [timeout seconds, default 20] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "../host/frame_shm.h"

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s <shm path> <width> <height> <pattern hex> [timeout s]\n", argv[0]); return 2; }
    const char *path = argv[1];
    uint32_t want_w = (uint32_t)strtoul(argv[2], NULL, 10);
    uint32_t want_h = (uint32_t)strtoul(argv[3], NULL, 10);
    uint32_t pattern = (uint32_t)strtoul(argv[4], NULL, 16);
    double timeout_s = argc > 5 ? atof(argv[5]) : 20.0;
    uint8_t want_bytes[4] = {
        (uint8_t)(pattern & 0xff), (uint8_t)((pattern >> 8) & 0xff),
        (uint8_t)((pattern >> 16) & 0xff), (uint8_t)((pattern >> 24) & 0xff)
    }; /* a 32-bit little-endian store leaves these four bytes in memory, in this order, per pixel */

    static uint8_t buf[HV_SHM_PIXEL_BYTES];
    HvShm *s = NULL;
    double waited = 0; const double step = 0.1;
    int opened = 0;
    while (waited < timeout_s) {
        if (!opened && hv_shm_open(path, &s) == 0) opened = 1;
        if (opened) {
            uint64_t seen = 0; uint32_t w = 0, h = 0;
            int r = hv_shm_read_frame(s, &seen, buf, sizeof buf, &w, &h);
            if (r == 1) {
                printf("frame_probe_check: got a frame, %ux%u (wanted %ux%u)\n", w, h, want_w, want_h);
                if (w != want_w || h != want_h) {
                    fprintf(stderr, "FAIL size mismatch: got %ux%u, want %ux%u\n", w, h, want_w, want_h);
                    hv_shm_close(s); return 1;
                }
                size_t n = (size_t)w * h * 4;
                size_t bad = 0, first_bad = (size_t)-1;
                for (size_t i = 0; i < n; i += 4) {
                    if (buf[i] != want_bytes[0] || buf[i+1] != want_bytes[1] || buf[i+2] != want_bytes[2] || buf[i+3] != want_bytes[3]) {
                        if (first_bad == (size_t)-1) first_bad = i / 4;
                        bad++;
                    }
                }
                if (bad) {
                    fprintf(stderr, "FAIL %zu of %zu pixels did not match 0x%02x%02x%02x%02x (first mismatch: pixel %zu = %02x %02x %02x %02x)\n",
                        bad, n / 4, want_bytes[3], want_bytes[2], want_bytes[1], want_bytes[0], first_bad,
                        buf[first_bad*4], buf[first_bad*4+1], buf[first_bad*4+2], buf[first_bad*4+3]);
                    hv_shm_close(s); return 1;
                }
                printf("PASS all %zu pixels match the pattern the guest code wrote; a real engine-drawn frame reached the shared file intact\n", n / 4);
                hv_shm_close(s); return 0;
            }
        }
        usleep((unsigned)(step * 1e6)); waited += step;
    }
    fprintf(stderr, "FAIL no matching frame within %.1fs (opened=%d)\n", timeout_s, opened);
    if (s) hv_shm_close(s);
    return 1;
}
