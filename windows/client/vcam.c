#define COBJMACROS
#include "vcam.h"
#include <windows.h>
#include <stdlib.h>
#include "../common/mcv_shared.h"
#include "../common/mcv_vcam_api.h"

static IMcVirtualCamera *g_cam;
static HANDLE g_map;
static uint8_t *g_base;
static ULONGLONG g_last_open_try;
static uint8_t *g_tmp;

static bool registered(void)
{
	HKEY k;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" MCV_CLSID_STR L"\\InprocServer32", 0,
			  KEY_READ, &k) != ERROR_SUCCESS)
		return false;
	wchar_t path[MAX_PATH] = {0};
	DWORD sz = sizeof(path) - sizeof(wchar_t), type = 0;
	LSTATUS r = RegQueryValueExW(k, NULL, NULL, &type, (BYTE *)path, &sz);
	RegCloseKey(k);
	return r == ERROR_SUCCESS && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

enum vcam_state vcam_start(long *hr_out)
{
	if (hr_out)
		*hr_out = 0;
	if (g_cam)
		return VCAM_READY;
	PFN_MFCreateVirtualCamera create = mcv_load_api();
	if (!create)
		return VCAM_UNSUPPORTED;
	if (!registered())
		return VCAM_NOT_INSTALLED;
	HRESULT hr = create(MCV_TYPE_SOFTWARE, MCV_LIFETIME_SESSION, MCV_ACCESS_CURRENT_USER, MCV_FRIENDLY_NAME,
			    MCV_CLSID_STR, NULL, 0, &g_cam);
	if (SUCCEEDED(hr)) {
		hr = g_cam->lpVtbl->Start(g_cam, NULL);
		if (FAILED(hr)) {
			g_cam->lpVtbl->Shutdown(g_cam);
			g_cam->lpVtbl->Release(g_cam);
			g_cam = NULL;
		}
	}
	if (hr_out)
		*hr_out = hr;
	return SUCCEEDED(hr) ? VCAM_READY : VCAM_FAILED;
}

void vcam_stop(void)
{
	if (g_cam) {
		g_cam->lpVtbl->Remove(g_cam);
		g_cam->lpVtbl->Shutdown(g_cam);
		g_cam->lpVtbl->Release(g_cam);
		g_cam = NULL;
	}
	if (g_base) {
		UnmapViewOfFile(g_base);
		g_base = NULL;
	}
	if (g_map) {
		CloseHandle(g_map);
		g_map = NULL;
	}
}

/* The media source creates the mapping when an application starts reading the camera. */
static bool open_shm(void)
{
	if (g_base)
		return true;
	ULONGLONG now = GetTickCount64();
	if (now - g_last_open_try < 500)
		return false;
	g_last_open_try = now;
	g_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MCV_SHM_NAME);
	if (!g_map)
		return false;
	g_base = MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, MCV_SHM_SIZE);
	if (!g_base) {
		CloseHandle(g_map);
		g_map = NULL;
		return false;
	}
	return true;
}

bool vcam_in_use(void)
{
	open_shm();
	return g_base && ((struct mcv_header *)g_base)->streaming;
}

void vcam_write(const struct nv12 *src, int rot, bool mirror, bool fill)
{
	if (!open_shm())
		return;
	struct mcv_header *h = (struct mcv_header *)g_base;
	if (h->magic != MCV_MAGIC || !h->streaming)
		return;
	int w = (int)h->want_w, hh = (int)h->want_h;
	if (w <= 0 || hh <= 0 || w > MCV_MAX_W || hh > MCV_MAX_H) {
		w = MCV_MAX_W;
		hh = MCV_MAX_H;
	}
	w &= ~1;
	hh &= ~1;
	if (!g_tmp)
		g_tmp = malloc(MCV_FRAME_MAX);
	if (!g_tmp)
		return;
	struct nv12 d = {g_tmp, g_tmp + (size_t)w * hh, w, w, w, hh};
	nv12_transform(src, &d, rot, mirror, fill);

	/* seqlock publish */
	InterlockedIncrement((volatile LONG *)&h->seq);
	MemoryBarrier();
	memcpy(mcv_frame(g_base), g_tmp, (size_t)w * hh * 3 / 2);
	h->width = (uint32_t)w;
	h->height = (uint32_t)hh;
	h->writer_tick = (int64_t)GetTickCount64();
	h->writer_pid = GetCurrentProcessId();
	MemoryBarrier();
	InterlockedIncrement((volatile LONG *)&h->seq);
}
