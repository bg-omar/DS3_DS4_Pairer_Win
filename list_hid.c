#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>   // malloc/free
#include <wchar.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

static void print_wstr(const wchar_t* s) {
	if (!s) return;
	int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
	if (n <= 0) return;
	char* u = (char*)malloc((size_t)n);
	if (!u) return;
	WideCharToMultiByte(CP_UTF8, 0, s, -1, u, n, NULL, NULL);
	fputs(u, stdout);
	free(u);
}

int main(void) {
	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);

	HDEVINFO devs = SetupDiGetClassDevs(&hidGuid, NULL, NULL,
										DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (devs == INVALID_HANDLE_VALUE) {
		puts("SetupDiGetClassDevs failed");
		return 1;
	}

	SP_DEVICE_INTERFACE_DATA ifd;
	ifd.cbSize = sizeof(ifd);
	DWORD idx = 0;
	int count = 0;

	while (SetupDiEnumDeviceInterfaces(devs, NULL, &hidGuid, idx++, &ifd)) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetail(devs, &ifd, NULL, 0, &need, NULL);
		PSP_DEVICE_INTERFACE_DETAIL_DATA det =
				(PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(need);
		if (!det) continue;
		det->cbSize = sizeof(*det);

		SP_DEVINFO_DATA did;
		did.cbSize = sizeof(did);

		if (SetupDiGetDeviceInterfaceDetail(devs, &ifd, det, need, NULL, &did)) {
			HANDLE h = CreateFile(det->DevicePath,
								  GENERIC_READ | GENERIC_WRITE,
								  FILE_SHARE_READ | FILE_SHARE_WRITE,
								  NULL, OPEN_EXISTING,
								  FILE_ATTRIBUTE_NORMAL, NULL);
			if (h == INVALID_HANDLE_VALUE) {
				// retry with write-only (some collections don’t allow read)
				h = CreateFile(det->DevicePath,
							   GENERIC_WRITE,
							   FILE_SHARE_READ | FILE_SHARE_WRITE,
							   NULL, OPEN_EXISTING,
							   FILE_ATTRIBUTE_NORMAL, NULL);
			}
			if (h != INVALID_HANDLE_VALUE) {
				HIDD_ATTRIBUTES a; a.Size = sizeof(a);
				if (HidD_GetAttributes(h, &a)) {
					PHIDP_PREPARSED_DATA pp = NULL;
					if (HidD_GetPreparsedData(h, &pp)) {
						HIDP_CAPS caps;
						HidP_GetCaps(pp, &caps);
						HidD_FreePreparsedData(pp);

						printf("Path: ");
						print_wstr(det->DevicePath);
						printf("\n  VID: %04x  PID: %04x  UsagePage: 0x%04x  Usage: 0x%04x  FeatCaps: %u\n",
							   a.VendorID, a.ProductID,
							   caps.UsagePage, caps.Usage,
							   caps.NumberFeatureValueCaps);

						wchar_t prod[128];
						if (HidD_GetProductString(h, prod, sizeof(prod))) {
							printf("  Product: "); print_wstr(prod); printf("\n");
						}
						wchar_t manf[128];
						if (HidD_GetManufacturerString(h, manf, sizeof(manf))) {
							printf("  Mfr: "); print_wstr(manf); printf("\n");
						}
						puts("");
						count++;
					}
				}
				CloseHandle(h);
			}
		}
		free(det);
	}

	SetupDiDestroyDeviceInfoList(devs);
	if (count == 0) puts("(No HID devices opened; ensure you ran as x64 and controller is on USB.)");
	return 0;
}
