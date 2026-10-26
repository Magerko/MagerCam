/*
 * Test tool for the MagerCam virtual camera.
 *   vcamtest direct <dll>   load the media source in-process (no registration) and read frames
 *   vcamtest create         create a session virtual camera, open it as a normal app would, read frames
 *   vcamtest read           open the "MagerCam" camera of a running MagerCam.exe and read frames
 *   vcamtest feed           only write a moving test pattern into the shared memory
 * A test pattern is fed into the shared memory meanwhile; one frame is saved as vcamtest.bmp.
 */
#define COBJMACROS
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include <stdbool.h>
#include "../common/mcv_shared.h"
#include "../common/mcv_vcam_api.h"

static volatile LONG g_stop;

static DWORD WINAPI feeder(void *arg)
{
	(void)arg;
	HANDLE map = NULL;
	uint8_t *base = NULL;
	int frame = 0;
	const int W = 1280, H = 720;
	while (!g_stop) {
		if (!base) {
			map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MCV_SHM_NAME);
			if (!map)
				map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\MagerCamVirtualCamera");
			if (map) {
				base = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, MCV_SHM_SIZE);
				printf("feeder: shared memory opened (%s)\n", base ? "ok" : "map failed");
			}
			if (!base) {
				Sleep(200);
				continue;
			}
		}
		struct mcv_header *h = (struct mcv_header *)base;
		InterlockedIncrement((volatile LONG *)&h->seq);
		uint8_t *y = mcv_frame(base), *uv = y + W * H;
		int bar = (frame * 8) % W;
		for (int r = 0; r < H; r++)
			for (int c = 0; c < W; c++)
				y[r * W + c] = (c >= bar && c < bar + 80) ? 235 : (uint8_t)(16 + (c * 200 / W));
		for (int r = 0; r < H / 2; r++)
			for (int c = 0; c < W; c += 2) {
				uv[r * W + c] = (uint8_t)(64 + r * 128 / (H / 2));
				uv[r * W + c + 1] = 160;
			}
		h->width = W;
		h->height = H;
		h->writer_tick = (int64_t)GetTickCount64();
		h->writer_pid = GetCurrentProcessId();
		MemoryBarrier();
		InterlockedIncrement((volatile LONG *)&h->seq);
		if (frame % 60 == 0)
			printf("feeder: frame %d, consumer wants %ux%u streaming=%d\n", frame, h->want_w, h->want_h, h->streaming);
		frame++;
		Sleep(33);
	}
	return 0;
}

static void save_bmp(const char *path, const uint8_t *y, const uint8_t *uv, int w, int h, int pitch)
{
	int row = w * 3;
	int pad = (4 - row % 4) % 4;
	BITMAPFILEHEADER fh = {0x4D42};
	BITMAPINFOHEADER ih = {sizeof(ih), w, h, 1, 24};
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize = fh.bfOffBits + (row + pad) * h;
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(&fh, sizeof(fh), 1, f);
	fwrite(&ih, sizeof(ih), 1, f);
	uint8_t *line = malloc(row + pad);
	memset(line, 0, row + pad);
	for (int r = h - 1; r >= 0; r--) {
		for (int c = 0; c < w; c++) {
			int Y = y[r * pitch + c] - 16, U = uv[(r / 2) * pitch + (c & ~1)] - 128, V = uv[(r / 2) * pitch + (c & ~1) + 1] - 128;
			int R = (298 * Y + 459 * V + 128) >> 8, G = (298 * Y - 55 * U - 136 * V + 128) >> 8, B = (298 * Y + 541 * U + 128) >> 8;
			line[c * 3] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B);
			line[c * 3 + 1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G);
			line[c * 3 + 2] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R);
		}
		fwrite(line, row + pad, 1, f);
	}
	free(line);
	fclose(f);
}

static int read_frames(IMFMediaSource *src, int want_w, int want_h)
{
	IMFSourceReader *rd = NULL;
	HRESULT hr = MFCreateSourceReaderFromMediaSource(src, NULL, &rd);
	if (FAILED(hr)) {
		printf("MFCreateSourceReaderFromMediaSource: %08lx\n", hr);
		return 1;
	}
	/* list native types, pick the requested size */
	IMFMediaType *t = NULL;
	DWORD pick = 0;
	for (DWORD i = 0; SUCCEEDED(IMFSourceReader_GetNativeMediaType(rd, 0, i, &t)); i++) {
		UINT32 w, h, n, d;
		UINT64 v = 0;
		IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &v);
		w = (UINT32)(v >> 32), h = (UINT32)v;
		IMFMediaType_GetUINT64(t, &MF_MT_FRAME_RATE, &v);
		n = (UINT32)(v >> 32), d = (UINT32)v;
		printf("  type %lu: %ux%u @ %u/%u\n", i, w, h, n, d);
		if ((int)w == want_w && (int)h == want_h && n == 30)
			pick = i;
		IMFMediaType_Release(t);
	}
	IMFSourceReader_GetNativeMediaType(rd, 0, pick, &t);
	hr = IMFSourceReader_SetCurrentMediaType(rd, 0, NULL, t);
	IMFMediaType_Release(t);
	printf("SetCurrentMediaType(type %lu): %08lx\n", pick, hr);

	DWORD t0 = GetTickCount();
	int got = 0;
	for (int i = 0; i < 90; i++) {
		DWORD idx, flags;
		LONGLONG ts;
		IMFSample *s = NULL;
		hr = IMFSourceReader_ReadSample(rd, 0, 0, &idx, &flags, &ts, &s);
		if (FAILED(hr)) {
			printf("ReadSample: %08lx\n", hr);
			break;
		}
		if (!s)
			continue;
		got++;
		if (got == 45 || got == 5) {
			IMFMediaBuffer *b = NULL;
			IMFSample_ConvertToContiguousBuffer(s, &b);
			BYTE *p;
			DWORD len;
			IMFMediaBuffer_Lock(b, &p, NULL, &len);
			printf("frame %d: %lu bytes, ts %.3f s\n", got, len, ts / 1e7);
			save_bmp(got == 5 ? "vcamtest_early.bmp" : "vcamtest.bmp", p, p + want_w * want_h, want_w, want_h, want_w);
			IMFMediaBuffer_Unlock(b);
			IMFMediaBuffer_Release(b);
		}
		IMFSample_Release(s);
	}
	DWORD dt = GetTickCount() - t0;
	printf("read %d frames in %lu ms (%.1f fps)\n", got, dt, got * 1000.0 / (dt ? dt : 1));
	IMFSourceReader_Release(rd);
	return got > 0 ? 0 : 1;
}

int wmain(int argc, wchar_t **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 2) {
		printf("usage: vcamtest direct <dll> | create | read | feed\n");
		return 2;
	}
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	MFStartup(MF_VERSION, MFSTARTUP_FULL);
	bool read_only = !wcscmp(argv[1], L"read"); /* frames come from MagerCam.exe */
	HANDLE th = read_only ? NULL : CreateThread(NULL, 0, feeder, NULL, 0, NULL);
	int rc = 1;

	if (!wcscmp(argv[1], L"feed")) {
		printf("feeding, Ctrl+C to stop\n");
		WaitForSingleObject(th, INFINITE);
	} else if (!wcscmp(argv[1], L"direct") && argc > 2) {
		HMODULE m = LoadLibraryW(argv[2]);
		if (!m) {
			printf("LoadLibrary failed %lu\n", GetLastError());
			return 1;
		}
		typedef HRESULT(WINAPI * GCO)(REFCLSID, REFIID, void **);
		GCO gco = (GCO)(void *)GetProcAddress(m, "DllGetClassObject");
		CLSID clsid;
		CLSIDFromString(MCV_CLSID_STR, &clsid);
		IClassFactory *cf = NULL;
		HRESULT hr = gco(&clsid, &IID_IClassFactory, (void **)&cf);
		IMFActivate *act = NULL;
		if (SUCCEEDED(hr))
			hr = IClassFactory_CreateInstance(cf, NULL, &IID_IMFActivate, (void **)&act);
		IMFMediaSource *src = NULL;
		if (SUCCEEDED(hr))
			hr = IMFActivate_ActivateObject(act, &IID_IMFMediaSource, (void **)&src);
		printf("activate: %08lx\n", hr);
		if (SUCCEEDED(hr)) {
			rc = read_frames(src, 1280, 720);
			IMFActivate_ShutdownObject(act);
			IMFMediaSource_Release(src);
		}
		if (act)
			IMFActivate_Release(act);
	} else if (!wcscmp(argv[1], L"create") || read_only) {
		PFN_MFCreateVirtualCamera create = mcv_load_api();
		if (!create) {
			printf("MFCreateVirtualCamera not available\n");
			return 1;
		}
		IMcVirtualCamera *vc = NULL;
		HRESULT hr = read_only ? S_OK : create(MCV_TYPE_SOFTWARE, MCV_LIFETIME_SESSION, MCV_ACCESS_CURRENT_USER, L"MagerCam Test",
				    MCV_CLSID_STR, NULL, 0, &vc);
		printf("MFCreateVirtualCamera: %08lx\n", hr);
		if (SUCCEEDED(hr) && vc) {
			hr = vc->lpVtbl->Start(vc, NULL);
			printf("Start: %08lx\n", hr);
		}
		if (SUCCEEDED(hr)) {
			IMFAttributes *a;
			MFCreateAttributes(&a, 1);
			IMFAttributes_SetGUID(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
			IMFActivate **devs = NULL;
			UINT32 n = 0;
			MFEnumDeviceSources(a, &devs, &n);
			IMFActivate *mine = NULL;
			for (UINT32 i = 0; i < n; i++) {
				WCHAR *name = NULL;
				UINT32 len;
				IMFActivate_GetAllocatedString(devs[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
				printf("camera %u: %ls\n", i, name ? name : L"?");
				if (name && wcsstr(name, read_only ? L"MagerCam" : L"MagerCam Test") && !mine) {
					mine = devs[i];
					IMFActivate_AddRef(mine);
				}
				CoTaskMemFree(name);
				IMFActivate_Release(devs[i]);
			}
			CoTaskMemFree(devs);
			if (mine) {
				IMFMediaSource *src = NULL;
				hr = IMFActivate_ActivateObject(mine, &IID_IMFMediaSource, (void **)&src);
				printf("activate: %08lx\n", hr);
				if (SUCCEEDED(hr)) {
					rc = read_frames(src, 1280, 720);
					IMFActivate_ShutdownObject(mine);
					IMFMediaSource_Release(src);
				}
				IMFActivate_Release(mine);
			} else {
				printf("MagerCam camera not found in device list\n");
			}
			hr = vc->lpVtbl->Remove(vc);
			printf("Remove: %08lx\n", hr);
		}
		if (vc) {
			vc->lpVtbl->Shutdown(vc);
			vc->lpVtbl->Release(vc);
		}
	}
	g_stop = 1;
	if (th)
		WaitForSingleObject(th, 2000);
	MFShutdown();
	return rc;
}
