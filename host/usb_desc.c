/* Included by host/usb_host.c (and by tests/test_usbdesc.c); not a separate translation unit, so no build file has to list it. */
#include "usb_desc.h"

#include <string.h>

const char *hv_dfu_state_name(int s) {
    static const char *n[] = {"appIDLE", "appDETACH", "dfuIDLE", "dfuDNLOAD-SYNC", "dfuDNBUSY", "dfuDNLOAD-IDLE",
                              "dfuMANIFEST-SYNC", "dfuMANIFEST", "dfuMANIFEST-WAIT-RESET", "dfuUPLOAD-IDLE", "dfuERROR"};
    return s >= 0 && s <= 10 ? n[s] : "?";
}

const char *hv_dfu_status_name(int s) {
    static const char *n[] = {"OK", "errTARGET", "errFILE", "errWRITE", "errERASE", "errCHECK_ERASED", "errPROG", "errVERIFY",
                              "errADDRESS", "errNOTDONE", "errFIRMWARE", "errVENDOR", "errUSBR", "errPOR", "errUNKNOWN", "errSTALLEDPKT"};
    return s >= 0 && s <= 15 ? n[s] : "?";
}

void hv_usb_hexdump(FILE *out, const char *label, const uint8_t *b, int len) {
    if (!out) return;
    fprintf(out, "%s (%d bytes):", label, len);
    for (int i = 0; i < len; i++) fprintf(out, "%s%02x", i % 16 == 0 ? "\n  " : " ", b[i]);
    fprintf(out, "\n");
}

int hv_usb_describe_config(const uint8_t *b, int len, HvUsbConfigInfo *info, FILE *out) {
    HvUsbConfigInfo z;
    memset(&z, 0, sizeof z);
    z.dfu_interface = z.dfu_protocol = z.dfu_alt = -1;
    *info = z;
    if (len < 9 || b[0] != 9 || b[1] != 2) {
        if (out) fprintf(out, "config: not a configuration descriptor header\n");
        return -1;
    }
    info->total_length = b[2] | (b[3] << 8);
    info->num_interfaces = b[4];
    info->configuration_value = b[5];
    if (out)
        fprintf(out, "config: wTotalLength=%d bNumInterfaces=%d bConfigurationValue=%d iConfiguration=%d bmAttributes=0x%02x bMaxPower=%d mA\n", info->total_length,
                info->num_interfaces, info->configuration_value, b[6], b[7], b[8] * 2);
    if (info->total_length > len || info->total_length < 9) {
        if (out) fprintf(out, "config: wTotalLength %d does not fit the %d bytes read\n", info->total_length, len);
        return -1;
    }
    int in_dfu = 0;
    for (int p = b[0]; p < info->total_length;) {
        int dl = b[p];
        if (dl < 2 || p + dl > info->total_length) {
            if (out) fprintf(out, "config: descriptor at %d has length %d and runs off the end\n", p, dl);
            return -1;
        }
        int t = b[p + 1];
        if (t == 4 && dl >= 9) {
            info->interfaces++;
            in_dfu = 0;
            if (out)
                fprintf(out, "  interface %d alt %d: %d endpoints, class 0x%02x subclass 0x%02x protocol 0x%02x iInterface=%d\n", b[p + 2], b[p + 3], b[p + 4],
                        b[p + 5], b[p + 6], b[p + 7], b[p + 8]);
            if (b[p + 5] == 0xFE && b[p + 6] == 1 && info->dfu_interface < 0) {
                info->dfu_interface = b[p + 2], info->dfu_alt = b[p + 3], info->dfu_protocol = b[p + 7], info->dfu_istring = b[p + 8];
                in_dfu = 1;
            }
        } else if (t == 5 && dl >= 7) {
            info->endpoints++;
            if (out) fprintf(out, "    endpoint 0x%02x attributes 0x%02x maxpacket %d interval %d\n", b[p + 2], b[p + 3], b[p + 4] | (b[p + 5] << 8), b[p + 6]);
        } else if (t == 0x21 && (dl == 7 || dl == 9) && in_dfu) {
            /* DFU 1.0 has the 7-byte form (no bcdDFUVersion; Apple's boot ROMs use it), DFU 1.1 the 9-byte one. The length is checked against the
               descriptor's own bLength, and the loop above already made sure p+dl is inside the blob. */
            info->dfu_func_seen = 1;
            info->dfu_func_length = dl;
            info->dfu_attributes = b[p + 2];
            info->dfu_detach_timeout = b[p + 3] | (b[p + 4] << 8);
            info->dfu_transfer_size = b[p + 5] | (b[p + 6] << 8);
            info->dfu_bcd_version = dl == 9 ? b[p + 7] | (b[p + 8] << 8) : -1;
            if (out) {
                fprintf(out, "    DFU functional (%d bytes): attributes 0x%02x (canDnload=%d canUpload=%d manifestTolerant=%d willDetach=%d) detachTimeout=%d transferSize=%d", dl,
                        info->dfu_attributes, info->dfu_attributes & 1, (info->dfu_attributes >> 1) & 1, (info->dfu_attributes >> 2) & 1, (info->dfu_attributes >> 3) & 1,
                        info->dfu_detach_timeout, info->dfu_transfer_size);
                if (dl == 9) fprintf(out, " bcdDFU=0x%04x\n", info->dfu_bcd_version);
                else fprintf(out, " (no bcdDFUVersion in this form; the 7-byte form alone does not say which DFU revision the device implements)\n");
            }
        } else if (out) {
            fprintf(out, "    other descriptor type 0x%02x length %d\n", t, dl);
        }
        p += dl;
    }
    if (info->interfaces != info->num_interfaces && out)
        fprintf(out, "config: note: %d interface descriptors (alternate settings included) for bNumInterfaces=%d\n", info->interfaces, info->num_interfaces);
    info->valid = 1;
    return 0;
}

int hv_dfu_should_get_status(int state_rc, int state, const char **why) {
    const char *w;
    int go = 0;
    if (state_rc != 1) w = state_rc < 0 ? "GET_STATE failed" : "GET_STATE did not return exactly 1 byte";
    else if (state != 2) w = "not in dfuIDLE; GET_STATUS could move this state on, so it is not sent";
    else w = "dfuIDLE", go = 1;
    if (why) *why = w;
    return go;
}

int hv_dfu_parse_status(int rc, const uint8_t b[6], HvDfuStatus *o) {
    if (rc != 6) return -1;
    o->status = b[0];
    o->poll_timeout_ms = b[1] | (b[2] << 8) | (b[3] << 16);
    o->state = b[4];
    o->istring = b[5];
    return 0;
}
