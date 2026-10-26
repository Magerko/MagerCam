/*
 * Minimal C declaration of the Windows 11 virtual camera API (mfvirtualcamera.h is not part of
 * mingw). MFCreateVirtualCamera is resolved at run time so the program still starts on Windows 10.
 */
#pragma once
#include <windows.h>
#include <mfobjects.h>

/* {6B1E3C7A-4D2F-4A8B-9C5E-2F7A1D3B8E41}: CLSID of the media source in magercam-vcam.dll */
#define MCV_CLSID_STR L"{6B1E3C7A-4D2F-4A8B-9C5E-2F7A1D3B8E41}"
#define MCV_FRIENDLY_NAME L"MagerCam"

enum { MCV_TYPE_SOFTWARE = 0 };
enum { MCV_LIFETIME_SESSION = 0, MCV_LIFETIME_SYSTEM = 1 };
enum { MCV_ACCESS_CURRENT_USER = 0, MCV_ACCESS_ALL_USERS = 1 };

typedef struct IMcVirtualCamera IMcVirtualCamera;
typedef struct IMcVirtualCameraVtbl {
	/* IUnknown */
	HRESULT(STDMETHODCALLTYPE *QueryInterface)(IMcVirtualCamera *, REFIID, void **);
	ULONG(STDMETHODCALLTYPE *AddRef)(IMcVirtualCamera *);
	ULONG(STDMETHODCALLTYPE *Release)(IMcVirtualCamera *);
	/* IMFAttributes: 30 methods, unused here */
	void *attributes[30];
	/* IMFVirtualCamera */
	HRESULT(STDMETHODCALLTYPE *AddDeviceSourceInfo)(IMcVirtualCamera *, LPCWSTR);
	HRESULT(STDMETHODCALLTYPE *AddProperty)(IMcVirtualCamera *, const void *, ULONG, const BYTE *, ULONG);
	HRESULT(STDMETHODCALLTYPE *AddRegistryEntry)(IMcVirtualCamera *, LPCWSTR, LPCWSTR, DWORD, const BYTE *, ULONG);
	HRESULT(STDMETHODCALLTYPE *Start)(IMcVirtualCamera *, IUnknown * /* IMFAsyncCallback */);
	HRESULT(STDMETHODCALLTYPE *Stop)(IMcVirtualCamera *);
	HRESULT(STDMETHODCALLTYPE *Remove)(IMcVirtualCamera *);
	HRESULT(STDMETHODCALLTYPE *GetMediaSource)(IMcVirtualCamera *, IUnknown **);
	void *SendCameraProperty;
	void *CreateSyncEvent;
	void *CreateSyncSemaphore;
	HRESULT(STDMETHODCALLTYPE *Shutdown)(IMcVirtualCamera *);
} IMcVirtualCameraVtbl;
struct IMcVirtualCamera {
	const IMcVirtualCameraVtbl *lpVtbl;
};

typedef HRESULT(WINAPI *PFN_MFCreateVirtualCamera)(int type, int lifetime, int access, LPCWSTR friendlyName,
						   LPCWSTR sourceId, const GUID *categories, ULONG categoryCount,
						   IMcVirtualCamera **out);

static inline PFN_MFCreateVirtualCamera mcv_load_api(void)
{
	HMODULE m = LoadLibraryW(L"mfsensorgroup.dll");
	return m ? (PFN_MFCreateVirtualCamera)(void *)GetProcAddress(m, "MFCreateVirtualCamera") : NULL;
}
