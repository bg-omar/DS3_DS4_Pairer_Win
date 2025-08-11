#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "comctl32.lib")

/* -------- PID helpers -------- */
static int is_ds3_pid(USHORT pid){ return (pid==0x0268 || pid==0x042F); }
static int is_ds4_controller_pid(USHORT pid){
	switch(pid){ case 0x05C4: case 0x09CC: case 0x0CE6: case 0x0CDA: return 1; default: return 0; }
}
static int is_ds4_dongle_pid(USHORT pid){ return pid==0x0BA0; }
static UCHAR pick_report_id(USHORT pid){ return is_ds3_pid(pid) ? 0xF5 : 0x12; }

/* -------- utils -------- */
static int char_to_nibble(char c){
	if(c>='0'&&c<='9') return c-'0';
	if(c>='a'&&c<='f') return c-'a'+10;
	if(c>='A'&&c<='F') return c-'A'+10;
	return -1;
}
static int mac_to_bytesA(const char* in, size_t in_len, unsigned char out6[6]){
	size_t i=0, p=0;
	while (p+1<in_len && i<6){
		if(in[p]==':'){ p++; continue; }
		if(char_to_nibble(in[p])<0 || char_to_nibble(in[p+1])<0) return 0;
		out6[i++] = (unsigned char)((char_to_nibble(in[p])<<4) | char_to_nibble(in[p+1]));
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

/* -------- model -------- */
typedef struct {
	WCHAR  path[520];
	USHORT pid;
	WCHAR  label[160];
} DeviceItem;

typedef struct {
	WCHAR name[64];
	WCHAR mac[32];  /* "aa:bb:cc:dd:ee:ff" */
} Preset;

/* -------- presets persistence -------- */
static int get_appdata_path(WCHAR out[MAX_PATH]){
	DWORD n = GetEnvironmentVariableW(L"APPDATA", out, MAX_PATH);
	return (n>0 && n<MAX_PATH);
}
static void ensure_dir_exists(const WCHAR* dir){
	CreateDirectoryW(dir, NULL); /* no-op if exists */
}
static void get_presets_file_path(WCHAR out[MAX_PATH]){
	WCHAR base[MAX_PATH]=L"";
	if(get_appdata_path(base)){
		_snwprintf(out, MAX_PATH-1, L"%ls\\SixaxisPairer", base);
		ensure_dir_exists(out);
		_snwprintf(out, MAX_PATH-1, L"%ls\\SixaxisPairer\\presets.txt", base);
	}else{
		lstrcpynW(out, L".\\presets.txt", MAX_PATH);
	}
}
static int load_presets(Preset** arr, size_t* count){
	*arr=NULL; *count=0;
	WCHAR path[MAX_PATH]; get_presets_file_path(path);
	FILE* f = _wfopen(path, L"rt, ccs=UTF-8");
	if(!f) return 1; /* no presets yet = ok */

	size_t cap=8; Preset* list=(Preset*)calloc(cap,sizeof(Preset));
	if(!list){ fclose(f); return 0; }

	WCHAR line[256];
	while(fgetws(line, 256, f)){
		/* strip newline */
		size_t L=wcslen(line);
		while(L>0 && (line[L-1]==L'\n' || line[L-1]==L'\r')){ line[--L]=0; }
		if(L==0) continue;

		/* expect name=mac */
		WCHAR* eq = wcschr(line, L'=');
		if(!eq) continue;
		*eq = 0;
		WCHAR* nm = line;
		WCHAR* mc = eq+1;

		if(*count==cap){
			cap*=2;
			Preset* tmp=(Preset*)realloc(list, cap*sizeof(Preset));
			if(!tmp){ free(list); fclose(f); return 0; }
			list=tmp;
		}
		lstrcpynW(list[*count].name, nm, 64);
		lstrcpynW(list[*count].mac,  mc, 32);
		(*count)++;
	}
	fclose(f);
	*arr = list;
	return 1;
}
static int save_presets(const Preset* arr, size_t count){
	WCHAR path[MAX_PATH]; get_presets_file_path(path);
	FILE* f = _wfopen(path, L"wt, ccs=UTF-8");
	if(!f) return 0;
	for(size_t i=0;i<count;i++){
		fwprintf(f, L"%ls=%ls\n", arr[i].name, arr[i].mac);
	}
	fclose(f);
	return 1;
}
static int mac_format_okW(const WCHAR* mac){
	/* accept XX:XX:XX:XX:XX:XX or XXXXXXXXXXXX */
	int colons=0, hex=0;
	for(const WCHAR* p=mac; *p; ++p){
		if(*p==L':'){ colons++; continue; }
		if(!( (*p>=L'0'&&*p<=L'9') || (*p>=L'a'&&*p<=L'f') || (*p>=L'A'&&*p<=L'F') )) return 0;
		hex++;
	}
	if(colons==5 && hex==12) return 1;
	if(colons==0 && hex==12) return 1;
	return 0;
}

/* -------- enumerate Sony HID devices -------- */
static DeviceItem* list_sony(size_t* count){
	GUID g;
	HDEVINFO devs;
	SP_DEVICE_INTERFACE_DATA ifd;
	DWORD idx=0, need=0;
	DeviceItem* arr=0;
	size_t cap=8;

	*count=0;
	HidD_GetHidGuid(&g);
	devs = SetupDiGetClassDevs(&g, NULL, NULL, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
	if(devs==INVALID_HANDLE_VALUE) return NULL;

	arr=(DeviceItem*)calloc(cap, sizeof(DeviceItem));
	if(!arr){ SetupDiDestroyDeviceInfoList(devs); return NULL; }

	ZeroMemory(&ifd, sizeof(ifd));
	ifd.cbSize = sizeof(ifd);

	while(SetupDiEnumDeviceInterfaces(devs, NULL, &g, idx++, &ifd)){
		PSP_DEVICE_INTERFACE_DETAIL_DATA det;
		HANDLE h;
		HIDD_ATTRIBUTES a;
		WCHAR prod[128]={0};

		need=0;
		SetupDiGetDeviceInterfaceDetail(devs, &ifd, NULL, 0, &need, NULL);
		det=(PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(need);
		if(!det) continue;
		det->cbSize = sizeof(*det);

		if(SetupDiGetDeviceInterfaceDetail(devs, &ifd, det, need, NULL, NULL)){
			h = CreateFile(det->DevicePath, GENERIC_READ|GENERIC_WRITE,
						   FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
						   FILE_ATTRIBUTE_NORMAL, NULL);
			if(h==INVALID_HANDLE_VALUE){
				h = CreateFile(det->DevicePath, GENERIC_WRITE,
							   FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							   FILE_ATTRIBUTE_NORMAL, NULL);
			}
			if(h!=INVALID_HANDLE_VALUE){
				a.Size = sizeof(a);
				if(HidD_GetAttributes(h, &a) && a.VendorID==0x054C){
					if(*count==cap){
						DeviceItem* tmp;
						cap*=2;
						tmp=(DeviceItem*)realloc(arr, cap*sizeof(DeviceItem));
						if(!tmp){ CloseHandle(h); free(det); break; }
						arr=tmp;
					}
					{
						DeviceItem* it=&arr[*count];
						ZeroMemory(it, sizeof(*it));
						lstrcpynW(it->path, det->DevicePath, (int)(sizeof(it->path)/sizeof(WCHAR)));
						it->pid = a.ProductID;

						HidD_GetProductString(h, prod, sizeof(prod));
						{
							const WCHAR *kind = L"Sony HID";
							if (is_ds4_controller_pid(a.ProductID)) kind = L"Controller";
							else if (is_ds4_dongle_pid(a.ProductID)) kind = L"Dongle";
							else if (is_ds3_pid(a.ProductID))       kind = L"DS3/Sixaxis";

							if(prod[0])
								_snwprintf(it->label, (int)(sizeof(it->label)/sizeof(WCHAR))-1,
										   L"%ls — %ls (PID %04X)", kind, prod, a.ProductID);
							else
								_snwprintf(it->label, (int)(sizeof(it->label)/sizeof(WCHAR))-1,
										   L"%ls (PID %04X)", kind, a.ProductID);
						}
						(*count)++;
					}
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

/* -------- HID feature helpers -------- */
static int get_feat_len(HANDLE h, USHORT* feat_len){
	PHIDP_PREPARSED_DATA pp=NULL;
	HIDP_CAPS caps;
	NTSTATUS st;

	if(!HidD_GetPreparsedData(h, &pp)) return 0;
	st = HidP_GetCaps(pp, &caps);
	HidD_FreePreparsedData(pp);
	if(st != HIDP_STATUS_SUCCESS) return 0;
	if(caps.FeatureReportByteLength < 8) return 0;
	*feat_len = caps.FeatureReportByteLength;
	return 1;
}

static int read_mac(HANDLE h, USHORT pid, UCHAR report_id, char out[18]){
	USHORT L=0;
	unsigned char* buf;
	BOOL ok;

	if(!get_feat_len(h,&L)) return 0;
	buf=(unsigned char*)calloc(L,1);
	if(!buf) return 0;

	buf[0]=report_id; buf[1]=0x00;
	ok = HidD_GetFeature(h, buf, L);
	if(!ok){ free(buf); return 0; }

	if(is_ds3_pid(pid)) bytes_to_macA_forward(buf+2,out);
	else                bytes_to_macA_reverse(buf+2,out);

	free(buf);
	return 1;
}

static int write_mac(HANDLE h, USHORT pid, UCHAR report_id, const char* mac_str){
	size_t n = strlen(mac_str);
	unsigned char mac[6];
	USHORT L=0;
	unsigned char* buf;
	BOOL ok;

	if(!((n==12)||(n==17)) || !mac_to_bytesA(mac_str,n,mac)) return 0;
	if(!get_feat_len(h,&L)) return 0;
	buf=(unsigned char*)calloc(L,1);
	if(!buf) return 0;

	buf[0]=report_id; buf[1]=0x00;
	if(is_ds3_pid(pid)){
		memcpy(buf+2, mac, 6);
	}else{
		buf[2]=mac[5]; buf[3]=mac[4]; buf[4]=mac[3];
		buf[5]=mac[2]; buf[6]=mac[1]; buf[7]=mac[0];
	}

	ok = HidD_SetFeature(h, buf, L);
	free(buf);
	return ok ? 1 : 0;
}

/* -------- GUI -------- */
#define IDC_DEV       1001
#define IDC_MAC       1002
#define IDC_READ      1003
#define IDC_SET       1004
#define IDC_STATUS    1005
#define IDC_REFRESH   1006
#define IDC_PRESET    1101
#define IDC_PRESETNM  1102
#define IDC_SAVEP     1103
#define IDC_LOADP     1104
#define IDC_DELP      1105

typedef struct {
	HWND hCombo, hEdit, hRead, hSet, hStatus, hRefresh;
	HWND hPresetCombo, hPresetName, hSavePreset, hLoadPreset, hDelPreset;
	DeviceItem* items; size_t nitems;
	Preset* presets; size_t npresets;
} App;

static void set_status(HWND h, LPCWSTR msg){ SetWindowTextW(h, msg); }

static void populate_devices(App* a){
	size_t n=0; DeviceItem* items=list_sony(&n);
	SendMessage(a->hCombo, CB_RESETCONTENT, 0, 0);
	if(a->items) free(a->items);
	a->items = items; a->nitems = n;
	if(n==0){
		set_status(a->hStatus, L"Status: no Sony HID found (USB).");
	}else{
		size_t i; int pick=0;
		for(i=0;i<n;i++){
			SendMessageW(a->hCombo, CB_ADDSTRING, 0, (LPARAM)items[i].label);
			if(is_ds4_controller_pid(items[i].pid)) pick=(int)i; /* prefer controller */
		}
		SendMessageW(a->hCombo, CB_SETCURSEL, pick, 0);
		set_status(a->hStatus, L"Status: device list refreshed.");
	}
}
static void populate_presets(App* a){
	if(a->presets){ free(a->presets); a->presets=NULL; a->npresets=0; }
	if(!load_presets(&a->presets, &a->npresets)){
		set_status(a->hStatus, L"Status: failed to load presets.");
		return;
	}
	SendMessage(a->hPresetCombo, CB_RESETCONTENT, 0, 0);
	for(size_t i=0;i<a->npresets;i++){
		SendMessageW(a->hPresetCombo, CB_ADDSTRING, 0, (LPARAM)a->presets[i].name);
	}
	if(a->npresets>0) SendMessage(a->hPresetCombo, CB_SETCURSEL, 0, 0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
	App* app = (App*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

	if(msg==WM_CREATE){
		INITCOMMONCONTROLSEX ic;
		HFONT hf;
		HWND hCombo, hEdit, hRead, hSet, hStat, hRef;
		HWND hPresetCombo, hPresetName, hSaveP, hLoadP, hDelP;
		App* a;

		ic.dwSize = sizeof(ic);
		ic.dwICC  = ICC_STANDARD_CLASSES;
		InitCommonControlsEx(&ic);

		hf=(HFONT)GetStockObject(DEFAULT_GUI_FONT);

		CreateWindowW(L"STATIC", L"Device:", WS_CHILD|WS_VISIBLE, 10,10,60,20, hwnd, NULL, NULL, NULL);
		hCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
							   80,8,240,200, hwnd, (HMENU)IDC_DEV, NULL, NULL);
		hRef   = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD|WS_VISIBLE,
							   330,8,70,24, hwnd, (HMENU)IDC_REFRESH, NULL, NULL);

		CreateWindowW(L"STATIC", L"MAC:", WS_CHILD|WS_VISIBLE, 10,44,60,20, hwnd, NULL, NULL, NULL);
		hEdit  = CreateWindowW(L"EDIT", L"", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
							   80,40,200,24, hwnd, (HMENU)IDC_MAC, NULL, NULL);
		hRead  = CreateWindowW(L"BUTTON", L"Read", WS_CHILD|WS_VISIBLE,
							   290,40,50,24, hwnd, (HMENU)IDC_READ, NULL, NULL);
		hSet   = CreateWindowW(L"BUTTON", L"Set", WS_CHILD|WS_VISIBLE,
							   350,40,50,24, hwnd, (HMENU)IDC_SET, NULL, NULL);

		/* Presets row */
		CreateWindowW(L"STATIC", L"Preset:", WS_CHILD|WS_VISIBLE, 10,74,60,20, hwnd, NULL, NULL, NULL);
		hPresetCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
									 80,72,200,200, hwnd, (HMENU)IDC_PRESET, NULL, NULL);
		hLoadP = CreateWindowW(L"BUTTON", L"Load", WS_CHILD|WS_VISIBLE,
							   290,72,50,24, hwnd, (HMENU)IDC_LOADP, NULL, NULL);
		hDelP  = CreateWindowW(L"BUTTON", L"Delete", WS_CHILD|WS_VISIBLE,
							   350,72,50,24, hwnd, (HMENU)IDC_DELP, NULL, NULL);

		CreateWindowW(L"STATIC", L"Name:", WS_CHILD|WS_VISIBLE, 10,104,60,20, hwnd, NULL, NULL, NULL);
		hPresetName = CreateWindowW(L"EDIT", L"", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
									80,100,200,24, hwnd, (HMENU)IDC_PRESETNM, NULL, NULL);
		hSaveP = CreateWindowW(L"BUTTON", L"Save", WS_CHILD|WS_VISIBLE,
							   290,100,110,24, hwnd, (HMENU)IDC_SAVEP, NULL, NULL);

		hStat  = CreateWindowW(L"STATIC", L"Status: ready", WS_CHILD|WS_VISIBLE,
							   10,132,390,20, hwnd, (HMENU)IDC_STATUS, NULL, NULL);

		SendMessage(hCombo, WM_SETFONT, (WPARAM)hf, TRUE);
		SendMessage(hEdit,  WM_SETFONT, (WPARAM)hf, TRUE);
		SendMessage(hRead,  WM_SETFONT, (WPARAM)hf, TRUE);
		SendMessage(hSet,   WM_SETFONT, (WPARAM)hf, TRUE);
		SendMessage(hPresetCombo, WM_SETFONT, (WPARAM)hf, TRUE);
		SendMessage(hPresetName,  WM_SETFONT, (WPARAM)hf, TRUE);
		SendMessage(hLoadP, WM_SETFONT, (WPARAM)hf, TRUE);
		SendMessage(hDelP,  WM_SETFONT, (WPARAM)hf, TRUE);
		SendMessage(hSaveP, WM_SETFONT, (WPARAM)hf, TRUE);
		SendMessage(hRef,   WM_SETFONT, (WPARAM)hf, TRUE);
		SendMessage(hStat,  WM_SETFONT, (WPARAM)hf, TRUE);

		a=(App*)calloc(1,sizeof(App));
		if(!a) return -1;
		a->hCombo=hCombo; a->hEdit=hEdit; a->hRead=hRead; a->hSet=hSet; a->hStatus=hStat; a->hRefresh=hRef;
		a->hPresetCombo=hPresetCombo; a->hPresetName=hPresetName; a->hSavePreset=hSaveP; a->hLoadPreset=hLoadP; a->hDelPreset=hDelP;
		SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)a);

		populate_devices(a);
		populate_presets(a);
		return 0;
	}

	if(msg==WM_COMMAND){
		if(!app) return 0;
		WORD id = LOWORD(wParam);

		if(id==IDC_REFRESH){
			populate_devices(app);
			return 0;
		}
		if(id==IDC_READ || id==IDC_SET){
			int sel = (int)SendMessage(app->hCombo, CB_GETCURSEL, 0, 0);
			if(sel<0 || (size_t)sel>=app->nitems){
				set_status(app->hStatus, L"Select a device first.");
				return 0;
			}
			{
				DeviceItem* it=&app->items[sel];
				HANDLE h = CreateFileW(it->path, GENERIC_READ|GENERIC_WRITE,
									   FILE_SHARE_READ|FILE_SHARE_WRITE, NULL,
									   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				if(h==INVALID_HANDLE_VALUE){
					h = CreateFileW(it->path, GENERIC_WRITE,
									FILE_SHARE_READ|FILE_SHARE_WRITE, NULL,
									OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				}
				if(h==INVALID_HANDLE_VALUE){ set_status(app->hStatus, L"Open failed."); return 0; }

				{
					UCHAR report_id = pick_report_id(it->pid);
					if(id==IDC_READ){
						char mac[18]="";
						if(read_mac(h, it->pid, report_id, mac)){
							WCHAR wmac[18];
							MultiByteToWideChar(CP_UTF8,0,mac,-1,wmac,18);
							SetWindowTextW(app->hEdit, wmac);
							set_status(app->hStatus, L"Status: MAC read OK.");
						}else{
							set_status(app->hStatus, L"Read failed (try replug USB).");
						}
					}else{
						WCHAR wmac[64]; char macA[64];
						GetWindowTextW(app->hEdit, wmac, 64);
						if(!mac_format_okW(wmac)){
							set_status(app->hStatus, L"Invalid MAC format. Use XX:XX:XX:XX:XX:XX.");
							CloseHandle(h); return 0;
						}
						WideCharToMultiByte(CP_UTF8,0,wmac,-1,macA,64,NULL,NULL);
						if(write_mac(h, it->pid, report_id, macA)){
							set_status(app->hStatus, L"Status: MAC set OK (replug to verify).");
						}else{
							set_status(app->hStatus, L"Set failed.");
						}
					}
				}
				CloseHandle(h);
			}
			return 0;
		}

		if(id==IDC_LOADP){
			int sel=(int)SendMessage(app->hPresetCombo, CB_GETCURSEL, 0, 0);
			if(sel<0 || (size_t)sel>=app->npresets){ set_status(app->hStatus, L"No preset selected."); return 0; }
			SetWindowTextW(app->hEdit, app->presets[sel].mac);
			SetWindowTextW(app->hPresetName, app->presets[sel].name);
			set_status(app->hStatus, L"Preset loaded into MAC field.");
			return 0;
		}
		if(id==IDC_SAVEP){
			WCHAR name[64]={0}, mac[64]={0};
			int i;
			GetWindowTextW(app->hPresetName, name, 64);
			GetWindowTextW(app->hEdit, mac, 64);
			if(name[0]==0){ set_status(app->hStatus, L"Enter a preset Name."); return 0; }
			if(!mac_format_okW(mac)){ set_status(app->hStatus, L"Invalid MAC format."); return 0; }

			/* upsert */
			for(i=0;i<(int)app->npresets;i++){
				if(_wcsicmp(app->presets[i].name, name)==0){
					lstrcpynW(app->presets[i].mac, mac, 32);
					if(!save_presets(app->presets, app->npresets)) set_status(app->hStatus, L"Failed to save presets.");
					populate_presets(app);
					set_status(app->hStatus, L"Preset updated.");
					return 0;
				}
			}
			/* new */
			{
				Preset* tmp=(Preset*)realloc(app->presets, (app->npresets+1)*sizeof(Preset));
				if(!tmp){ set_status(app->hStatus, L"Out of memory."); return 0; }
				app->presets=tmp;
				lstrcpynW(app->presets[app->npresets].name, name, 64);
				lstrcpynW(app->presets[app->npresets].mac,  mac,  32);
				app->npresets++;
				if(!save_presets(app->presets, app->npresets)) set_status(app->hStatus, L"Failed to save presets.");
				populate_presets(app);
				set_status(app->hStatus, L"Preset saved.");
			}
			return 0;
		}
		if(id==IDC_DELP){
			int sel=(int)SendMessage(app->hPresetCombo, CB_GETCURSEL, 0, 0);
			if(sel<0 || (size_t)sel>=app->npresets){ set_status(app->hStatus, L"No preset selected."); return 0; }
			/* remove sel */
			if(app->npresets>0){
				for(size_t i=sel+1;i<app->npresets;i++) app->presets[i-1]=app->presets[i];
				app->npresets--;
				if(app->npresets==0){ free(app->presets); app->presets=NULL; }
				else{
					Preset* tmp=(Preset*)realloc(app->presets, app->npresets*sizeof(Preset));
					if(tmp) app->presets=tmp;
				}
				if(!save_presets(app->presets, app->npresets)) set_status(app->hStatus, L"Failed to save presets.");
				populate_presets(app);
				set_status(app->hStatus, L"Preset deleted.");
			}
			return 0;
		}
	}

	if(msg==WM_DESTROY){
		if(app){
			if(app->items) free(app->items);
			if(app->presets) free(app->presets);
			free(app);
			SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
		}
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow){
	const wchar_t *cls=L"SixaxisPairerGUI";
	WNDCLASSEXW wcx;
	HWND hwnd;
	MSG msg;

	ZeroMemory(&wcx, sizeof(wcx));
	wcx.cbSize        = sizeof(wcx);
	wcx.style         = CS_HREDRAW|CS_VREDRAW;
	wcx.lpfnWndProc   = WndProc;
	wcx.hInstance     = hInst;
	wcx.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wcx.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
	wcx.lpszClassName = cls;

	if(!RegisterClassExW(&wcx)) return 0;

	hwnd = CreateWindowExW(0, cls, L"Sixaxis/DS4 Pairer",
						   WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
						   CW_USEDEFAULT, CW_USEDEFAULT, 420, 190,
						   NULL, NULL, hInst, NULL);
	if(!hwnd) return 0;

	ShowWindow(hwnd, nShow);
	UpdateWindow(hwnd);

	while(GetMessage(&msg,NULL,0,0)>0){
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return (int)msg.wParam;
}
