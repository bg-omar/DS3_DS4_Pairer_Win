// build: clang -framework IOKit -framework CoreFoundation pair_sixaxis_mac.c -o pair_sixaxis
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <stdio.h>
#include <ctype.h>

static const int VENDOR = 0x054c;
static const int PRODUCTS[] = { 0x0268, 0x042f };
static const uint8_t MAC_REPORT_ID = 0xf5;

// … (helper: char_to_nibble, mac_to_bytes) same as above …

static IOHIDDeviceRef open_device() {
	IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	IOHIDManagerSetDeviceMatching(mgr, NULL);
	IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);

	CFSetRef devset = IOHIDManagerCopyDevices(mgr);
	if (!devset) return NULL;
	CFIndex n = CFSetGetCount(devset);
	IOHIDDeviceRef *devs = (IOHIDDeviceRef*)calloc(n, sizeof(*devs));
	CFSetGetValues(devset, (const void **)devs);

	IOHIDDeviceRef match = NULL;
	for (CFIndex i=0;i<n;i++) {
		CFTypeRef v = IOHIDDeviceGetProperty(devs[i], CFSTR(kIOHIDVendorIDKey));
		CFTypeRef p = IOHIDDeviceGetProperty(devs[i], CFSTR(kIOHIDProductIDKey));
		if (!v || !p) continue;
		int vid = (int)CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &vid);
		int pid = 0; CFNumberGetValue((CFNumberRef)p, kCFNumberIntType, &pid);
		if (vid == VENDOR) {
			for (size_t k=0;k<sizeof(PRODUCTS)/sizeof(PRODUCTS[0]);++k) {
				if (pid == PRODUCTS[k]) { match = devs[i]; break; }
			}
		}
		if (match) break;
	}
	CFRelease(devset);
	if (!match) { CFRelease(mgr); free(devs); return NULL; }
	CFRetain(match); // keep the device
	IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
	CFRelease(mgr);
	free(devs);
	return match;
}

static int set_mac(IOHIDDeviceRef dev, const char* mac) {
	uint8_t buf[8] = {0}, m[6] = {0};
	size_t L = strlen(mac);
	if (!((L==12)||(L==17)) || !mac_to_bytes(mac, L, m)) { fprintf(stderr,"Invalid MAC\n"); return 0; }
	buf[0]=MAC_REPORT_ID; buf[1]=0x00; memcpy(buf+2, m, 6);
	IOReturn r = IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature, MAC_REPORT_ID, buf, sizeof(buf));
	if (r) { fprintf(stderr,"IOHIDDeviceSetReport err=0x%x\n", r); return 0; }
	return 1;
}
static int get_mac(IOHIDDeviceRef dev) {
	uint8_t buf[8] = {0}; CFIndex len = sizeof(buf);
	buf[0]=MAC_REPORT_ID; buf[1]=0x00;
	IOReturn r = IOHIDDeviceGetReport(dev, kIOHIDReportTypeFeature, MAC_REPORT_ID, buf, &len);
	if (r || len < 8) { fprintf(stderr,"IOHIDDeviceGetReport err=0x%x, len=%ld\n", r, (long)len); return 0; }
	printf("%02x:%02x:%02x:%02x:%02x:%02x\n", buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
	return 1;
}

int main(int argc, char** argv) {
	if (argc != 1 && argc != 2) { fprintf(stderr, "usage: %s [mac]\n", argv[0]); return 1; }
	IOHIDDeviceRef dev = open_device();
	if (!dev) { fprintf(stderr, "controller not found\n"); return 2; }
	int ok = (argc==2) ? set_mac(dev, argv[1]) : get_mac(dev);
	CFRelease(dev);
	return ok ? 0 : 3;
}
