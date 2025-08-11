#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"hid.lib")
#pragma comment(lib,"comctl32.lib")

// ---------- DS family helpers ----------
static int is_ds3_pid(USHORT pid){ return (pid==0x0268 || pid==0x042F); }
static int is_ds4_controller_pid(USHORT pid){
	switch(pid){ case 0x05C4: case 0x09CC: case 0x0CE6: case 0x0CDA: return 1; default: return 0; }
}
static int is_ds4_dongle_pid(USHORT pid){ return pid==0x0BA0; }
static UCHAR pick_report_id(USHORT pid){ return is_ds3_pid(pid) ? 0xF5 : 0x12; }

// ---------- utils ----------
static int char_to_nibble(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
static int mac_to_bytesA(const char* in, size_t in_len, unsigned char out6[6]){
	size_t i=0;
	for(size_t p=0;p+1<in_len && i<6;){
		if(in[p]==':'){ p++; continue; }
		int hi=char_to_nibble(in[p]), lo=char_to_nibble(in[p+1]);
		if(hi<0||lo<0) return 0;
		out6[i++] = (unsigned char)((hi<<4)|lo);
		p+=2;
	}
	return i==6;
}
static void bytes_to_macA_forward(const unsigned char b[6], char out[18]){
	sprintf(out,"%02x:%02x:%02x:%02x:%02x:%02x",b[0],b[1],b[2],b[3],b[4],b[5]);
}
static void bytes_to_macA_reverse(const unsigned char b[6], char out[18]){
	sprintf(out,"%02x:%02x:%02x:%02x:%02x:%02x",b[5],b[4],b[3],b[2],b[1],b[0]);
}

typedef struct {
	WCHAR  path[520];   // HID path
	USHORT pid;
	WCHAR  label[128];  // pretty name
} DeviceItem;

// ---------- enumerate Sony HID devices, return array ----------
static DeviceItem* list_sony(size_t* count){
	*count = 0;
	GUID g; HidD_GetHidGuid(&g);
	HDEVINFO devs = SetupDiGetClassDevs(&g,NULL,NULL,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
	if(devs==INVALID_HANDLE_VALUE) return NULL;

	SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize=sizeof(ifd);
	DWORD idx=0, cap=8;
	DeviceItem* arr = (DeviceItem*)calloc(cap,sizeof(DeviceItem));
	if(!arr){ SetupDiDestroyDeviceInfoList(devs); return NULL; }

	while(SetupDiEnumDeviceInterfaces(devs,NULL,&g,idx++,&ifd)){
		DWORD need=0;
		SetupDiGetDeviceInterfaceDetail(devs,&ifd,NULL,0,&need,NULL);
		PSP_DEVICE_INTERFACE_DETAIL_DATA det=(PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(need);
		if(!det) continue;
		det->cbSize=sizeof(*det);
		if(SetupDiGetDeviceInterfaceDetail(devs,&ifd,det,need,NULL,NULL)){
			HANDLE h = CreateFile(det->DevicePath, GENERIC_READ|GENERIC_WRITE,
								  FILE_SHARE_READ|FILE_SHARE_WRITE, NULL,
								  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if(h==INVALID_HANDLE_VALUE){
				h = CreateFile(det->DevicePath, GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
							   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			}
			if(h!=INVALID_HANDLE_VALUE){
				HIDD_ATTRIBUTES a; a.Size=sizeof(a);
				if(HidD_GetAttributes(h,&a) && a.VendorID==0x054c){
					if(*count==cap){ cap*=2; arr=(DeviceItem*)realloc(arr,cap*sizeof(DeviceItem)); }
					DeviceItem *it=&arr[*count];
					wcsncpy(it->path, det->DevicePath, _countof(it->path)-1);
					it->pid = a.ProductID;

					WCHAR prod[128]=L"", manf[128]=L"";
					HidD_GetProductString(h, prod, sizeof(prod));
					HidD_GetManufacturerString(h, manf, sizeof(manf));
					_snwprintf(it->label,_countof(it->label)-1,L"%ls (PID %04X)",
							   (prod[0]?prod:L"Sony HID"), a.ProductID);
					(*count)++;
				}
				CloseHandle(h);
			}
		}
		free(det);
	}
	SetupDiDestroyDeviceInfoList(devs);
	if(*count==0){ free(arr); return NULL; }
	return arr;
}

// ---------- feature helpers ----------
static int get_feat_len(HANDLE h, USHORT* feat_len){
	PHIDP_PREPARSED_DATA pp=NULL; HIDP_CAPS caps;
	if(!HidD_GetPreparsedData(h,&pp)) return 0;
	NTSTATUS st=HidP_GetCaps(pp,&caps);
	HidD_FreePreparsedData(pp);
	if(st!=HIDP_STATUS_SUCCESS) return 0;
	*feat_len=caps.FeatureReportByteLength;
	return (*feat_len>=8);
}

static int read_mac(HANDLE h, USHORT pid, UCHAR report_id, char out[18]){
	USHORT L=0; if(!get_feat_len(h,&L)) return 0;
	unsigned char* buf=(unsigned char*)calloc(L,1); if(!buf) return 0;
	buf[0]=report_id; buf[1]=0x00;
	BOOL ok = HidD_GetFeature(h, buf, L);
	if(!ok){ free(buf); return 0; }
	if(is_ds3_pid(pid)) bytes_to_macA_forward(buf+2,out);
	else                bytes_to_macA_reverse(buf+2,out);
	free(buf); return 1;
}

static int write_mac(HANDLE h, USHORT pid, UCHAR report_id, const char* mac_str){
	unsigned char mac[6];
	size_t n=strlen(mac_str);
	if(!((n==12)||(n==17)) || !mac_to_bytesA(mac_str,n,mac)) return 0;

	USHORT L=0; if(!get_feat_len(h,&L)) return 0;
	unsigned char* buf=(unsigned char*)calloc(L,1); if(!buf) return 0;
	buf[0]=report_id; buf[1]=0x00;
	if(is_ds3_pid(pid)){
		memcpy(buf+2, mac, 6);
	}else{
		buf[2]=mac[5]; buf[3]=mac[4]; buf[4]=mac[3]; buf[5]=mac[2]; buf[6]=mac[1]; buf[7]=mac[0];
	}
	BOOL ok = HidD_SetFeature(h, buf, L);
	free(buf);
	return ok ? 1 : 0;
}

// ---------- GUI ----------
#define IDC_DEV     1001
#define IDC_MAC     1002
#define IDC_READ    1003
#define IDC_SET     1004
#define IDC_STATUS  1005

typedef struct {
	HWND hCombo, hEdit, hRead, hSet, hStatus;
	DeviceItem* items; size_t nitems;
} App;

static void set_status(HWND h, LPCWSTR msg){
	SetWindowTextW(h, msg);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
	App* app = (App*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	switch(msg){
		case WM_CREATE:{
			INITCOMMONCONTROLSEX ic={sizeof(ic), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&ic);

			HFONT hf=(HFONT)GetStockObject(DEFAULT_GUI_FONT);

			CreateWindowW(L"STATIC", L"Device:", WS_CHILD|WS_VISIBLE, 10,10,60,20, hwnd, NULL, NULL, NULL);
			HWND hCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
										80,8,320,200, hwnd, (HMENU)IDC_DEV, NULL, NULL);
			CreateWindowW(L"STATIC", L"MAC:", WS_CHILD|WS_VISIBLE, 10,44,60,20, hwnd, NULL, NULL, NULL);
			HWND hEdit  = CreateWindowW(L"EDIT", L"", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
										80,40,200,24, hwnd, (HMENU)IDC_MAC, NULL, NULL);
			HWND hRead  = CreateWindowW(L"BUTTON", L"Read", WS_CHILD|WS_VISIBLE,
										290,40,50,24, hwnd, (HMENU)IDC_READ, NULL, NULL);
			HWND hSet   = CreateWindowW(L"BUTTON", L"Set", WS_CHILD|WS_VISIBLE,
										350,40,50,24, hwnd, (HMENU)IDC_SET, NULL, NULL);
			HWND hStat  = CreateWindowW(L"STATIC", L"Status: ready", WS_CHILD|WS_VISIBLE,
										10,75,390,20, hwnd, (HMENU)IDC_STATUS, NULL, NULL);

			SendMessage(hCombo, WM_SETFONT, (WPARAM)hf, TRUE);
			SendMessage(hEdit,  WM_SETFONT, (WPARAM)hf, TRUE);
			SendMessage(hRead,  WM_SETFONT, (WPARAM)hf, TRUE);
			SendMessage(hSet,   WM_SETFONT, (WPARAM)hf, TRUE);
			SendMessage(hStat,  WM_SETFONT, (WPARAM)hf, TRUE);

			App* a=(App*)calloc(1,sizeof(App));
			a->hCombo=hCombo; a->hEdit=hEdit; a->hRead=hRead; a->hSet=hSet; a->hStatus=hStat;
			SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)a);

			size_t n=0; DeviceItem* items=list_sony(&n);
			a->items=items; a->nitems=n;
			if(n==0){
				set_status(hStat, L"Status: no Sony HID found (USB).");
			}else{
				for(size_t i=0;i<n;i++){
					SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)items[i].label);
				}
				SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
				set_status(hStat, L"Status: device list loaded.");
			}
			return 0;
		}
		case WM_COMMAND:{
			if(!app) break;
			WORD id = LOWORD(wParam);
			if(id==IDC_READ || id==IDC_SET){
				int sel = (int)SendMessage(app->hCombo, CB_GETCURSEL, 0, 0);
				if(sel<0 || (size_t)sel>=app->nitems){ set_status(app->hStatus, L"Select a device first."); return 0; }
				DeviceItem* it=&app->items[sel];

				HANDLE h = CreateFileW(it->path, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
									   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				if(h==INVALID_HANDLE_VALUE){
					h = CreateFileW(it->path, GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
									NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				}
				if(h==INVALID_HANDLE_VALUE){ set_status(app->hStatus, L"Open failed."); return 0; }

				UCHAR report_id = pick_report_id(it->pid);

				if(id==IDC_READ){
					char mac[18]="";
					if(read_mac(h, it->pid, report_id, mac)){
						WCHAR wmac[18]; MultiByteToWideChar(CP_UTF8,0,mac,-1,wmac,18);
						SetWindowTextW(app->hEdit, wmac);
						set_status(app->hStatus, L"Status: MAC read OK.");
					}else{
						set_status(app->hStatus, L"Read failed (try replug USB).");
					}
				}else{
					WCHAR wmac[64]; GetWindowTextW(app->hEdit, wmac, _countof(wmac));
					char macA[64]; WideCharToMultiByte(CP_UTF8,0,wmac,-1,macA,64,NULL,NULL);
					if(write_mac(h, it->pid, report_id, macA)){
						set_status(app->hStatus, L"Status: MAC set OK (replug to verify).");
					}else{
						set_status(app->hStatus, L"Set failed (check format 11:22:33:44:55:66).");
					}
				}
				CloseHandle(h);
				return 0;
			}
			break;
		}
		case WM_DESTROY:{
			if(app){
				if(app->items) free(app->items);
				free(app);
			}
			PostQuitMessage(0); return 0;
		}
	}
	return DefWindowProc(hwnd,msg,wParam,lParam);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow){
	const wchar_t *cls=L"SixaxisPairerGUI";
	WNDCLASS wc={0}; wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
	wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.lpszClassName=cls;
	wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
	RegisterClass(&wc);

	HWND hwnd = CreateWindowW(cls, L"Sixaxis/DS4 Pairer",
							  WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
							  CW_USEDEFAULT, CW_USEDEFAULT, 420, 140, NULL, NULL, hInst, NULL);
	ShowWindow(hwnd, nShow); UpdateWindow(hwnd);

	MSG msg; while(GetMessage(&msg,NULL,0,0)>0){ TranslateMessage(&msg); DispatchMessage(&msg); }
	return (int)msg.wParam;
}
