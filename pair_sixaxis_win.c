// pair_sixaxis_win.c  — Windows native HID (no hidapi), DS3/DS4 pairing MAC tool.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _MSC_VER
#  pragma comment(lib, "setupapi.lib")
#  pragma comment(lib, "hid.lib")
#endif

// ---------- PID classification ----------
static int is_ds3_pid(USHORT pid) {
    return (pid == 0x0268 || pid == 0x042F); // Sixaxis/Move
}
static int is_ds4_controller_pid(USHORT pid) {
    switch (pid) {
        case 0x05C4: // DS4 (old)
        case 0x09CC: // DS4 (new)
        case 0x0CE6: // DS4 Slim (some fw)
        case 0x0CDA: // variant seen in wild
            return 1;
        default: return 0;
    }
}
static int is_ds4_dongle_pid(USHORT pid) {
    return (pid == 0x0BA0); // DUALSHOCK4 USB Wireless Adaptor
}
static UCHAR pick_report_id(USHORT pid) {
    return is_ds3_pid(pid) ? 0xF5 : 0x12; // DS3 uses 0xF5, DS4 family uses 0x12
}

// ---------- small utils ----------
static int char_to_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int mac_to_bytes(const char* in, size_t in_len, UCHAR out6[6]) {
    size_t i = 0;
    for (size_t p = 0; p + 1 < in_len && i < 6; ) {
        if (in[p] == ':') { p++; continue; }
        int hi = char_to_nibble(in[p]);
        int lo = char_to_nibble(in[p+1]);
        if (hi < 0 || lo < 0) return 0;
        out6[i++] = (UCHAR)((hi << 4) | lo);
        p += 2;
    }
    return i == 6;
}
static void print_mac_forward(const UCHAR *b) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x\n", b[0], b[1], b[2], b[3], b[4], b[5]);
}
static void print_mac_reverse(const UCHAR *b) { // for DS4 report 0x12 over USB
    printf("%02x:%02x:%02x:%02x:%02x:%02x\n", b[5], b[4], b[3], b[2], b[1], b[0]);
}

// ---------- device open (prefer controller) ----------
static HANDLE open_sony_hid(USHORT *out_pid) {
    GUID g; HidD_GetHidGuid(&g);
    HDEVINFO devs = SetupDiGetClassDevs(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
    DWORD idx = 0;

    HANDLE best_ds4 = INVALID_HANDLE_VALUE;  USHORT best_ds4_pid = 0;
    HANDLE best_ds3 = INVALID_HANDLE_VALUE;  USHORT best_ds3_pid = 0;
    HANDLE best_dgl = INVALID_HANDLE_VALUE;  USHORT best_dgl_pid = 0;
    HANDLE any_sony = INVALID_HANDLE_VALUE;  USHORT any_pid     = 0;

    while (SetupDiEnumDeviceInterfaces(devs, NULL, &g, idx++, &ifd)) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetail(devs, &ifd, NULL, 0, &need, NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA det = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(need);
        if (!det) continue;
        det->cbSize = sizeof(*det);

        if (SetupDiGetDeviceInterfaceDetail(devs, &ifd, det, need, NULL, NULL)) {
            HANDLE h = CreateFile(det->DevicePath, GENERIC_READ|GENERIC_WRITE,
                                  FILE_SHARE_READ|FILE_SHARE_WRITE, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                h = CreateFile(det->DevicePath, GENERIC_WRITE,
                               FILE_SHARE_READ|FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            }
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES a; a.Size = sizeof(a);
                if (HidD_GetAttributes(h, &a) && a.VendorID == 0x054c) {
                    if (is_ds4_controller_pid(a.ProductID)) {
                        if (best_ds4 == INVALID_HANDLE_VALUE) { best_ds4 = h; best_ds4_pid = a.ProductID; h = INVALID_HANDLE_VALUE; }
                    } else if (is_ds3_pid(a.ProductID)) {
                        if (best_ds3 == INVALID_HANDLE_VALUE) { best_ds3 = h; best_ds3_pid = a.ProductID; h = INVALID_HANDLE_VALUE; }
                    } else if (is_ds4_dongle_pid(a.ProductID)) {
                        if (best_dgl == INVALID_HANDLE_VALUE) { best_dgl = h; best_dgl_pid = a.ProductID; h = INVALID_HANDLE_VALUE; }
                    } else if (any_sony == INVALID_HANDLE_VALUE) {
                        any_sony = h; any_pid = a.ProductID; h = INVALID_HANDLE_VALUE;
                    }
                }
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(devs);

    HANDLE pick = INVALID_HANDLE_VALUE; USHORT pid = 0;
    if (best_ds4 != INVALID_HANDLE_VALUE) { pick = best_ds4; pid = best_ds4_pid; }
    else if (best_ds3 != INVALID_HANDLE_VALUE) { pick = best_ds3; pid = best_ds3_pid; }
    else if (any_sony != INVALID_HANDLE_VALUE) { pick = any_sony; pid = any_pid; }
    else if (best_dgl != INVALID_HANDLE_VALUE) { pick = best_dgl; pid = best_dgl_pid; }

    // Close unpicked
    if (best_ds4 != INVALID_HANDLE_VALUE && best_ds4 != pick) CloseHandle(best_ds4);
    if (best_ds3 != INVALID_HANDLE_VALUE && best_ds3 != pick) CloseHandle(best_ds3);
    if (best_dgl != INVALID_HANDLE_VALUE && best_dgl != pick) CloseHandle(best_dgl);
    if (any_sony != INVALID_HANDLE_VALUE && any_sony != pick) CloseHandle(any_sony);

    if (out_pid) *out_pid = pid;
    return pick;
}

// ---------- feature I/O ----------
static int feature_lengths(HANDLE h, USHORT *out_feat_len) {
    PHIDP_PREPARSED_DATA pp = NULL;
    HIDP_CAPS caps;
    if (!HidD_GetPreparsedData(h, &pp)) return 0;
    NTSTATUS st = HidP_GetCaps(pp, &caps);
    HidD_FreePreparsedData(pp);
    if (st != HIDP_STATUS_SUCCESS) return 0;
    if (out_feat_len) *out_feat_len = caps.FeatureReportByteLength;
    return 1;
}

static int do_set_mac(HANDLE h, USHORT pid, UCHAR report_id, const char* mac_str) {
    UCHAR mac6[6];
    size_t L = strlen(mac_str);
    if (!((L==12)||(L==17)) || !mac_to_bytes(mac_str, L, mac6)) {
        fprintf(stderr, "Invalid MAC. Use 112233445566 or 11:22:33:44:55:66\n");
        return 0;
    }

    USHORT feat_len = 0;
    if (!feature_lengths(h, &feat_len) || feat_len < 8) {
        fprintf(stderr, "Could not query FeatureReportByteLength\n");
        return 0;
    }

    UCHAR *buf = (UCHAR*)calloc(feat_len, 1);
    if (!buf) { fprintf(stderr, "OOM\n"); return 0; }
    buf[0] = report_id;
    buf[1] = 0x00;

    if (is_ds3_pid(pid)) {
        // DS3/Move: forward order at buf[2..7]
        memcpy(buf + 2, mac6, 6);
    } else {
        // DS4 family: common USB feature 0x12 uses reversed order
        buf[2] = mac6[5]; buf[3] = mac6[4]; buf[4] = mac6[3];
        buf[5] = mac6[2]; buf[6] = mac6[1]; buf[7] = mac6[0];
    }

    BOOL ok = HidD_SetFeature(h, buf, feat_len);
    free(buf);
    if (!ok) {
        fprintf(stderr, "HidD_SetFeature failed (err=%lu)\n", GetLastError());
        return 0;
    }
    return 1;
}

static int do_get_mac(HANDLE h, USHORT pid, UCHAR report_id) {
    USHORT feat_len = 0;
    if (!feature_lengths(h, &feat_len) || feat_len < 8) {
        fprintf(stderr, "Could not query FeatureReportByteLength\n");
        return 0;
    }
    UCHAR *buf = (UCHAR*)calloc(feat_len, 1);
    if (!buf) { fprintf(stderr, "OOM\n"); return 0; }
    buf[0] = report_id;
    buf[1] = 0x00;

    BOOL ok = HidD_GetFeature(h, buf, feat_len);
    if (!ok) {
        fprintf(stderr, "HidD_GetFeature failed (err=%lu)\n", GetLastError());
        free(buf);
        return 0;
    }

    // Payload is at [2..7]; DS4 prints reversed, DS3 prints forward
    if (is_ds3_pid(pid)) print_mac_forward(buf + 2);
    else                 print_mac_reverse(buf + 2);

    free(buf);
    return 1;
}

// ---------- main ----------
int main(int argc, char** argv) {
    if (argc != 1 && argc != 2) {
        fprintf(stderr, "usage: %s [mac]\n", argv[0]);
        return 1;
    }

    USHORT pid = 0;
    HANDLE h = open_sony_hid(&pid);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Sony HID not found on USB. Plug the controller by USB (not BT).\n");
        return 2;
    }

    UCHAR report_id = pick_report_id(pid);
    int ok = (argc == 2)
             ? do_set_mac(h, pid, report_id, argv[1])
             : do_get_mac(h, pid, report_id);

    CloseHandle(h);

    if (ok && argc == 2) puts("MAC set OK (unplug/replug USB if readback shows zeros).");
    return ok ? 0 : 3;
}
