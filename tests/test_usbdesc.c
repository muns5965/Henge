/* host/usb_desc.c on synthetic configuration descriptors (no device). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../host/usb_desc.c"

static int fails;
static void check(const char *name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) fails++; }

int main(void) {
    /* configuration (9) + DFU interface alt 0 (9) + DFU functional (9) = 27 bytes, the shape USB DFU 1.1 requires */
    uint8_t dfu[] = {9, 2, 27, 0, 1, 1, 0, 0x80, 50,
                     9, 4, 0, 0, 0, 0xFE, 1, 2, 5,
                     9, 0x21, 0x0B, 0xFF, 0x00, 0x00, 0x08, 0x1A, 0x01};
    HvUsbConfigInfo c;
    check("DFU configuration parses", hv_usb_describe_config(dfu, sizeof dfu, &c, stdout) == 0 && c.valid);
    check("one interface, no endpoints", c.interfaces == 1 && c.endpoints == 0 && c.num_interfaces == 1 && c.total_length == 27);
    check("DFU interface found: number 0, protocol 2 (DFU mode), iInterface 5", c.dfu_interface == 0 && c.dfu_protocol == 2 && c.dfu_istring == 5);
    check("DFU functional decoded: attributes 0x0B, detach 255 ms, transfer 2048, bcd 0x011A",
          c.dfu_func_seen && c.dfu_attributes == 0x0B && c.dfu_detach_timeout == 255 && c.dfu_transfer_size == 2048 && c.dfu_bcd_version == 0x011A);

    /* Apple's shape seen on the device: 9 + 9 + 7 = 25 bytes, DFU interface with protocol 0, 7-byte functional descriptor */
    uint8_t apple[] = {9, 2, 25, 0, 1, 1, 5, 0x80, 0xFA, 9, 4, 0, 0, 0, 0xFE, 1, 0, 0, 7, 0x21, 0x0D, 0x10, 0x27, 0x00, 0x10};
    check("7-byte DFU functional descriptor (Apple form) decoded, no bcd", hv_usb_describe_config(apple, sizeof apple, &c, stdout) == 0 && c.dfu_func_seen && c.dfu_func_length == 7 &&
          c.dfu_attributes == 0x0D && c.dfu_detach_timeout == 10000 && c.dfu_transfer_size == 4096 && c.dfu_bcd_version == -1 && c.dfu_protocol == 0 && c.dfu_interface == 0);
    check("9-byte form still reports length 9 and bcd", hv_usb_describe_config(dfu, sizeof dfu, &c, NULL) == 0 && c.dfu_func_length == 9 && c.dfu_bcd_version == 0x011A);
    uint8_t cut9[] = {9, 2, 25, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 0xFE, 1, 2, 0, 9, 0x21, 0x0B, 0xFF, 0x00, 0x00, 0x08};
    check("functional descriptor declaring 9 bytes with only 7 left is rejected, nothing read past the end", hv_usb_describe_config(cut9, sizeof cut9, &c, NULL) == -1 && !c.dfu_func_seen);
    uint8_t cut7[] = {9, 2, 24, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 0xFE, 1, 0, 0, 7, 0x21, 0x0D, 0x10, 0x27, 0x00};
    check("functional descriptor declaring 7 bytes with 6 left is rejected", hv_usb_describe_config(cut7, sizeof cut7, &c, NULL) == -1 && !c.dfu_func_seen);
    uint8_t odd8[] = {9, 2, 26, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 0xFE, 1, 0, 0, 8, 0x21, 0x0D, 0x10, 0x27, 0x00, 0x10, 0};
    check("an 8-byte 0x21 descriptor is not decoded as DFU functional", hv_usb_describe_config(odd8, sizeof odd8, &c, NULL) == 0 && !c.dfu_func_seen);
    check("hexdump prints 16 bytes per line", ({ char *bp; size_t bl; FILE *f = open_memstream(&bp, &bl); hv_usb_hexdump(f, "x", apple, sizeof apple); fclose(f); int ok = strstr(bp, "x (25 bytes):\n  09 02 19 00") && strchr(strchr(bp, '\n') + 1, '\n') && strstr(bp, "\n  21 0d") == NULL ? 1 : 0; free(bp); ok; }));

    uint8_t two[] = {9, 2, 34, 0, 1, 1, 0, 0x80, 50,
                     9, 4, 0, 0, 1, 0x08, 6, 0x50, 0,          /* mass storage, not DFU */
                     7, 5, 0x81, 2, 0x00, 0x02, 0,
                     9, 4, 1, 0, 0, 0xFE, 1, 1, 0};             /* DFU runtime, no functional descriptor */
    check("non-DFU interface and endpoint, then a DFU one without functional descriptor", hv_usb_describe_config(two, sizeof two, &c, NULL) == 0 && c.endpoints == 1 && c.interfaces == 2 &&
          c.dfu_interface == 1 && c.dfu_protocol == 1 && !c.dfu_func_seen);

    uint8_t none[] = {9, 2, 9, 0, 0, 1, 0, 0x80, 50};
    check("configuration with nothing in it parses, no DFU", hv_usb_describe_config(none, sizeof none, &c, NULL) == 0 && c.dfu_interface == -1 && c.dfu_protocol == -1);

    uint8_t shortbuf[] = {9, 2, 27, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0};
    check("wTotalLength larger than what was read is rejected", hv_usb_describe_config(shortbuf, sizeof shortbuf, &c, NULL) == -1 && !c.valid);
    uint8_t runoff[] = {9, 2, 18, 0, 1, 1, 0, 0x80, 50, 20, 4, 0, 0, 0, 0xFE, 1, 2, 0};
    check("a descriptor whose length runs past the end is rejected", hv_usb_describe_config(runoff, sizeof runoff, &c, NULL) == -1);
    uint8_t zerolen[] = {9, 2, 12, 0, 1, 1, 0, 0x80, 50, 0, 4, 0};
    check("a zero-length descriptor cannot loop forever", hv_usb_describe_config(zerolen, sizeof zerolen, &c, NULL) == -1);
    uint8_t wrongtype[] = {9, 1, 9, 0, 0, 1, 0, 0x80, 50};
    check("a device descriptor is not a configuration", hv_usb_describe_config(wrongtype, sizeof wrongtype, &c, NULL) == -1);
    check("fewer than 9 bytes is rejected", hv_usb_describe_config(dfu, 5, &c, NULL) == -1);
    check("a 0x21 descriptor outside the DFU interface is ignored", ({ uint8_t o[] = {9, 2, 27, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 0x03, 1, 1, 0, 9, 0x21, 1, 2, 3, 4, 5, 6, 7};
          hv_usb_describe_config(o, sizeof o, &c, NULL) == 0 && c.dfu_interface == -1 && !c.dfu_func_seen; }));
    check("state names", !strcmp(hv_dfu_state_name(2), "dfuIDLE") && !strcmp(hv_dfu_state_name(10), "dfuERROR") && !strcmp(hv_dfu_state_name(99), "?"));
    const char *why = NULL;
    check("GET_STATUS is sent from dfuIDLE with a 1-byte GET_STATE", hv_dfu_should_get_status(1, 2, &why) == 1);
    check("not sent from dfuDNLOAD-SYNC (3)", hv_dfu_should_get_status(1, 3, &why) == 0 && strstr(why, "not in dfuIDLE"));
    check("not sent from dfuMANIFEST-SYNC (6) or dfuERROR (10) or appIDLE (0)", !hv_dfu_should_get_status(1, 6, &why) && !hv_dfu_should_get_status(1, 10, &why) && !hv_dfu_should_get_status(1, 0, &why));
    check("not sent when GET_STATE failed (negative)", hv_dfu_should_get_status(-1, 2, &why) == 0 && strstr(why, "failed"));
    check("not sent when GET_STATE returned 0 or 2 bytes even if the byte says dfuIDLE", !hv_dfu_should_get_status(0, 2, &why) && !hv_dfu_should_get_status(2, 2, &why) && strstr(why, "exactly 1 byte"));
    HvDfuStatus st;
    uint8_t ok6[6] = {0, 0x64, 0, 0, 2, 0};
    check("GET_STATUS answer of 6 bytes is parsed", hv_dfu_parse_status(6, ok6, &st) == 0 && st.status == 0 && st.poll_timeout_ms == 100 && st.state == 2 && st.istring == 0);
    uint8_t big[6] = {0, 0xFF, 0xFF, 0xFF, 5, 1};
    check("3-byte poll timeout is little endian", hv_dfu_parse_status(6, big, &st) == 0 && st.poll_timeout_ms == 0xFFFFFF && st.state == 5);
    check("an answer that is not 6 bytes (5, 7, 0, negative) is refused", hv_dfu_parse_status(5, ok6, &st) < 0 && hv_dfu_parse_status(7, ok6, &st) < 0 && hv_dfu_parse_status(0, ok6, &st) < 0 && hv_dfu_parse_status(-2, ok6, &st) < 0);
    printf(fails ? "FAILED: %d\n" : "ALL PASS\n", fails);
    return fails != 0;
}
