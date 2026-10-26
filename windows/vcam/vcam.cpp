/*
 * Виртуальная камера MagerCam: медиаисточник Media Foundation, который я регистрирую как
 * виртуальную камеру Windows 11 (MFCreateVirtualCamera). DLL грузит служба Frame Server и раздаёт
 * камеру всем приложениям сразу.
 *
 * Кадры беру из общей памяти от MagerCam.exe (см. mcv_shared.h). Если клиент не запущен, показываю
 * картинку «Нет сигнала», а не роняю поток.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#include <windows.h>
#include <sddl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <new>

extern "C" {
#include "../common/mcv_shared.h"
#include "../common/mcv_nv12.h"

// заглушку рисую в 1080p: под 4K её всё равно растянет масштабирование, а памяти и работы вчетверо меньше
#define PH_W 1920
#define PH_H 1080
}

static const GUID CLSID_MagerCamSource = {0x6b1e3c7a, 0x4d2f, 0x4a8b, {0x9c, 0x5e, 0x2f, 0x7a, 0x1d, 0x3b, 0x8e, 0x41}};

// Этих GUID нет в заголовках mingw, значения беру из Windows SDK
static const GUID MY_MF_DEVICESTREAM_STREAM_ID = {0x11bd5120, 0xd124, 0x446b, {0x88, 0xe6, 0x17, 0x06, 0x02, 0x57, 0xff, 0xf9}};
static const GUID MY_MF_DEVICESTREAM_STREAM_CATEGORY = {0x2939e7b8, 0xa62e, 0x4579, {0xb6, 0x74, 0xd4, 0x07, 0x3d, 0xfa, 0xbb, 0xba}};
static const GUID MY_MF_DEVICESTREAM_FRAMESERVER_SHARED = {0x1cb378e9, 0xb279, 0x41d4, {0xaf, 0x97, 0x34, 0xa2, 0x43, 0xe6, 0x83, 0x20}};
static const GUID MY_MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES = {0x17145fd1, 0x1b2b, 0x423c, {0x80, 0x01, 0x2b, 0x68, 0x33, 0xed, 0x35, 0x88}};
static const GUID MY_PINNAME_VIDEO_CAPTURE = {0xfb6c4281, 0x0353, 0x11d1, {0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba}};
static const GUID MY_MFSampleExtension_Token = {0x8294da66, 0xf328, 0x4805, {0xb5, 0x51, 0x00, 0xde, 0xb4, 0xc5, 0x7a, 0x61}};
static const IID IID_MyKsControl = {0x28f54685, 0x06fd, 0x11d2, {0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};
static const IID IID_MyMFMediaStream2 = {0xc5bc37d6, 0x75c7, 0x46a1, {0xa1, 0x32, 0x81, 0xb5, 0xf7, 0x23, 0xc2, 0x0f}};
static const IID IID_MyMFSampleAllocatorControl = {0xda62b958, 0x3a38, 0x4a97, {0xbd, 0x27, 0x14, 0x9c, 0x64, 0x0c, 0x07, 0x71}};

// Этих интерфейсов в mingw тоже нет, объявляю их сам
struct IMyKsControl : public IUnknown {
	virtual HRESULT STDMETHODCALLTYPE KsProperty(void *prop, ULONG propLen, void *data, ULONG dataLen, ULONG *ret) = 0;
	virtual HRESULT STDMETHODCALLTYPE KsMethod(void *m, ULONG mLen, void *data, ULONG dataLen, ULONG *ret) = 0;
	virtual HRESULT STDMETHODCALLTYPE KsEvent(void *e, ULONG eLen, void *data, ULONG dataLen, ULONG *ret) = 0;
};
struct IMyMFMediaStream2 : public IMFMediaStream {
	virtual HRESULT STDMETHODCALLTYPE SetStreamState(MF_STREAM_STATE value) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetStreamState(MF_STREAM_STATE *value) = 0;
};
enum MyAllocatorUsage { UsesProvidedAllocator = 0, UsesCustomAllocator = 1, DoesNotAllocate = 2 };
struct IMySampleAllocatorControl : public IUnknown {
	virtual HRESULT STDMETHODCALLTYPE SetDefaultAllocator(DWORD outStreamId, IUnknown *allocator) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetAllocatorUsage(DWORD outStreamId, DWORD *inStreamId, DWORD *usage) = 0;
};

static HMODULE g_module;
static volatile LONG g_objects;

#define KS_NOT_FOUND HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND)
#ifndef SELFREG_E_CLASS
#define SELFREG_E_CLASS ((HRESULT)0x80040201L)
#endif

template <class T> static void safe_release(T *&p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

static void dbg(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	wvsprintfA(buf, fmt, ap);
	va_end(ap);
	OutputDebugStringA(buf);
}

struct FrameProvider {
	int slot = 0;
	HANDLE map = nullptr;
	uint8_t *base = nullptr;
	uint8_t *copy = nullptr;       // последний целый кадр клиента, прочитанный по seqlock
	uint32_t copy_w = 0, copy_h = 0;
	int32_t copy_seq = -1;
	uint8_t *placeholder = nullptr; // заглушка «Нет сигнала», NV12 PH_W x PH_H

	bool open()
	{
		if (base)
			return true;
		SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, FALSE};
		// Даю полный доступ SYSTEM, LOCAL SERVICE, админам и интерактивным пользователям. Без этой DACL
		// клиент из пользовательской сессии не откроет объект, который создала служба.
		ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;IU)",
								     SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);
		wchar_t name[96];
		mcv_shm_name(slot, L"Global\\", name, 96);
		map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, (DWORD)MCV_SHM_SIZE, name);
		if (!map) { // в Global\ создавать может только служба; если меня грузит обычное приложение (тест), беру Local\ как запасной вариант
			mcv_shm_name(slot, L"Local\\", name, 96);
			map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, (DWORD)MCV_SHM_SIZE, name);
		}
		if (sa.lpSecurityDescriptor)
			LocalFree(sa.lpSecurityDescriptor);
		if (!map) {
			dbg("MagerCam vcam: CreateFileMapping failed %lu\n", GetLastError());
			return false;
		}
		base = (uint8_t *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, MCV_SHM_SIZE);
		if (!base) {
			CloseHandle(map);
			map = nullptr;
			return false;
		}
		auto *h = (mcv_header *)base;
		if (h->magic != MCV_MAGIC) {
			h->version = MCV_VERSION;
			h->magic = MCV_MAGIC;
		}
		h->source_pid = GetCurrentProcessId();
		return true;
	}

	void close()
	{
		if (base) {
			((mcv_header *)base)->streaming = 0;
			UnmapViewOfFile(base);
		}
		if (map)
			CloseHandle(map);
		base = nullptr;
		map = nullptr;
		free(copy);
		copy = nullptr;
		free(placeholder);
		placeholder = nullptr;
	}

	void set_streaming(bool on, UINT32 w, UINT32 h)
	{
		if (!base)
			return;
		auto *hd = (mcv_header *)base;
		hd->want_w = w;
		hd->want_h = h;
		hd->streaming = on ? 1 : 0;
	}

	/* Забираю свежий кадр клиента по seqlock: нечётный seq значит, что запись идёт, а если seq
	 * поменялся во время копирования, кадр порван и я пробую снова. Клиента считаю живым, если он
	 * писал кадр за последние 3 с. */
	bool poll_client()
	{
		if (!base)
			return false;
		auto *h = (mcv_header *)base;
		if (h->writer_tick == 0 || GetTickCount64() - (uint64_t)h->writer_tick > 3000)
			return false;
		if (!copy)
			copy = (uint8_t *)malloc(MCV_FRAME_MAX);
		if (!copy)
			return false;
		for (int attempt = 0; attempt < 4; attempt++) {
			int32_t s1 = h->seq;
			if (s1 & 1) {
				Sleep(1);
				continue;
			}
			if (s1 == copy_seq)
				return true;
			MemoryBarrier();
			uint32_t w = h->width, hh = h->height;
			if (!w || !hh || w > MCV_MAX_W || hh > MCV_MAX_H)
				return false;
			memcpy(copy, mcv_frame(base), (size_t)w * hh * 3 / 2);
			MemoryBarrier();
			if (h->seq == s1) {
				copy_w = w;
				copy_h = hh;
				copy_seq = s1;
				return true;
			}
		}
		return copy_seq >= 0;
	}

	void make_placeholder()
	{
		if (placeholder)
			return;
		const int W = PH_W, H = PH_H;
		placeholder = (uint8_t *)malloc((size_t)W * H * 3 / 2);
		if (!placeholder)
			return;
		BITMAPINFO bi = {};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = W;
		bi.bmiHeader.biHeight = -H;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		void *bits = nullptr;
		HDC dc = CreateCompatibleDC(nullptr);
		HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!dc || !bmp || !bits) {
			struct nv12 d = {placeholder, placeholder + (size_t)W * H, W, W, W, H};
			nv12_fill_black(&d);
			if (bmp)
				DeleteObject(bmp);
			if (dc)
				DeleteDC(dc);
			return;
		}
		HGDIOBJ old = SelectObject(dc, bmp);
		RECT r = {0, 0, W, H};
		HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
		FillRect(dc, &r, bg);
		DeleteObject(bg);
		SetBkMode(dc, TRANSPARENT);
		HFONT big = CreateFontW(120, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
		HFONT small = CreateFontW(52, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
		SelectObject(dc, big);
		SetTextColor(dc, RGB(90, 200, 140));
		RECT t1 = {0, H / 2 - 170, W, H / 2 - 20};
		DrawTextW(dc, L"MagerCam", -1, &t1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
		SelectObject(dc, small);
		SetTextColor(dc, RGB(200, 204, 214));
		RECT t2 = {0, H / 2 + 10, W, H / 2 + 90};
		DrawTextW(dc, L"\x041D\x0435\x0442 \x0441\x0438\x0433\x043D\x0430\x043B\x0430", -1, &t2,
			  DT_CENTER | DT_SINGLELINE | DT_VCENTER); // "Нет сигнала"
		SetTextColor(dc, RGB(140, 146, 160));
		RECT t3 = {0, H / 2 + 100, W, H / 2 + 180};
		// "Запустите MagerCam на компьютере и подключите телефон"
		DrawTextW(dc,
			  L"\x0417\x0430\x043F\x0443\x0441\x0442\x0438\x0442\x0435 MagerCam \x043D\x0430 "
			  L"\x043A\x043E\x043C\x043F\x044C\x044E\x0442\x0435\x0440\x0435 \x0438 "
			  L"\x043F\x043E\x0434\x043A\x043B\x044E\x0447\x0438\x0442\x0435 "
			  L"\x0442\x0435\x043B\x0435\x0444\x043E\x043D",
			  -1, &t3, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
		GdiFlush();
		// Перевожу BGRA в NV12 (BT.709, ограниченный диапазон)
		const uint8_t *px = (const uint8_t *)bits;
		uint8_t *Y = placeholder, *UV = placeholder + (size_t)W * H;
		for (int y = 0; y < H; y++) {
			for (int x = 0; x < W; x++) {
				const uint8_t *p = px + ((size_t)y * W + x) * 4;
				int b = p[0], g = p[1], rr = p[2];
				Y[(size_t)y * W + x] = (uint8_t)((47 * rr + 157 * g + 16 * b + 128) / 256 + 16);
				if (!(y & 1) && !(x & 1)) {
					int u = (-26 * rr - 87 * g + 112 * b + 128) / 256 + 128;
					int v = (112 * rr - 102 * g - 10 * b + 128) / 256 + 128;
					uint8_t *o = UV + (size_t)(y / 2) * W + x;
					o[0] = (uint8_t)u;
					o[1] = (uint8_t)v;
				}
			}
		}
		SelectObject(dc, old);
		DeleteObject(big);
		DeleteObject(small);
		DeleteObject(bmp);
		DeleteDC(dc);
	}

	void render(struct nv12 *dst)
	{
		bool live = poll_client() && copy_seq >= 0;
		const uint8_t *src;
		int sw, sh;
		if (live) {
			src = copy;
			sw = (int)copy_w;
			sh = (int)copy_h;
		} else {
			make_placeholder();
			if (!placeholder) {
				nv12_fill_black(dst);
				return;
			}
			src = placeholder;
			sw = PH_W;
			sh = PH_H;
		}
		if (sw == dst->w && sh == dst->h) {
			for (int r = 0; r < sh; r++)
				memcpy(dst->y + (size_t)r * dst->ys, src + (size_t)r * sw, sw);
			for (int r = 0; r < sh / 2; r++)
				memcpy(dst->uv + (size_t)r * dst->uvs, src + (size_t)sw * sh + (size_t)r * sw, sw);
			return;
		}
		struct nv12 s = {(uint8_t *)src, (uint8_t *)src + (size_t)sw * sh, sw, sw, sw, sh};
		nv12_transform(&s, dst, 0, false, false);
	}
};

class MagerSource;

class MagerStream final : public IMyMFMediaStream2, public IMyKsControl {
public:
	MagerStream(MagerSource *src, CRITICAL_SECTION *lock, int slot) : m_source(src), m_lock(lock)
	{
		m_frames.slot = slot;
		InterlockedIncrement(&g_objects);
	}
	HRESULT init();

	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaStream ||
		    riid == IID_MyMFMediaStream2)
			*ppv = static_cast<IMyMFMediaStream2 *>(this);
		else if (riid == IID_MyKsControl)
			*ppv = static_cast<IMyKsControl *>(this);
		else {
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		AddRef();
		return S_OK;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
	STDMETHODIMP_(ULONG) Release() override
	{
		LONG r = InterlockedDecrement(&m_ref);
		if (!r)
			delete this;
		return r;
	}

	STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent **ev) override
	{
		IMFMediaEventQueue *q;
		{
			EnterCriticalSection(m_lock);
			q = m_queue;
			if (q)
				q->AddRef();
			LeaveCriticalSection(m_lock);
		}
		if (!q)
			return MF_E_SHUTDOWN;
		HRESULT hr = q->GetEvent(flags, ev);
		q->Release();
		return hr;
	}
	STDMETHODIMP BeginGetEvent(IMFAsyncCallback *cb, IUnknown *state) override
	{
		EnterCriticalSection(m_lock);
		HRESULT hr = m_queue ? m_queue->BeginGetEvent(cb, state) : MF_E_SHUTDOWN;
		LeaveCriticalSection(m_lock);
		return hr;
	}
	STDMETHODIMP EndGetEvent(IMFAsyncResult *res, IMFMediaEvent **ev) override
	{
		EnterCriticalSection(m_lock);
		HRESULT hr = m_queue ? m_queue->EndGetEvent(res, ev) : MF_E_SHUTDOWN;
		LeaveCriticalSection(m_lock);
		return hr;
	}
	STDMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT st, const PROPVARIANT *v) override
	{
		EnterCriticalSection(m_lock);
		HRESULT hr = m_queue ? m_queue->QueueEventParamVar(t, ext, st, v) : MF_E_SHUTDOWN;
		LeaveCriticalSection(m_lock);
		return hr;
	}

	STDMETHODIMP GetMediaSource(IMFMediaSource **ppSource) override;
	STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor **sd) override
	{
		if (!sd)
			return E_POINTER;
		EnterCriticalSection(m_lock);
		HRESULT hr = MF_E_SHUTDOWN;
		if (m_desc) {
			*sd = m_desc;
			m_desc->AddRef();
			hr = S_OK;
		}
		LeaveCriticalSection(m_lock);
		return hr;
	}
	STDMETHODIMP RequestSample(IUnknown *token) override
	{
		EnterCriticalSection(m_lock);
		HRESULT hr = S_OK;
		if (!m_queue)
			hr = MF_E_SHUTDOWN;
		else if (m_state != MF_STREAM_STATE_RUNNING)
			hr = MF_E_MEDIA_SOURCE_WRONGSTATE;
		else if (m_pending >= MAX_PENDING)
			hr = MF_E_SAMPLEALLOCATOR_EMPTY;
		else {
			m_tokens[(m_head + m_pending) % MAX_PENDING] = token;
			if (token)
				token->AddRef();
			m_pending++;
			SetEvent(m_wake);
		}
		LeaveCriticalSection(m_lock);
		return hr;
	}

	STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override
	{
		EnterCriticalSection(m_lock);
		bool run = value == MF_STREAM_STATE_RUNNING;
		bool was = m_state == MF_STREAM_STATE_RUNNING;
		m_state = value;
		LeaveCriticalSection(m_lock);
		if (run && !was)
			start_worker();
		else if (!run && was)
			stop_worker();
		return S_OK;
	}
	STDMETHODIMP GetStreamState(MF_STREAM_STATE *value) override
	{
		if (!value)
			return E_POINTER;
		EnterCriticalSection(m_lock);
		*value = m_state;
		LeaveCriticalSection(m_lock);
		return S_OK;
	}

	STDMETHODIMP KsProperty(void *, ULONG, void *, ULONG, ULONG *) override { return KS_NOT_FOUND; }
	STDMETHODIMP KsMethod(void *, ULONG, void *, ULONG, ULONG *) override { return KS_NOT_FOUND; }
	STDMETHODIMP KsEvent(void *, ULONG, void *, ULONG, ULONG *) override { return KS_NOT_FOUND; }

	HRESULT start(IMFMediaType *type);
	HRESULT stop();
	void shutdown();
	void set_allocator(IMFVideoSampleAllocatorEx *a)
	{
		EnterCriticalSection(m_lock);
		safe_release(m_allocator);
		m_allocator = a;
		if (a)
			a->AddRef();
		LeaveCriticalSection(m_lock);
	}
	IMFAttributes *attributes() { return m_attrs; }

private:
	~MagerStream()
	{
		shutdown();
		InterlockedDecrement(&g_objects);
	}
	void start_worker();
	void stop_worker();
	static DWORD WINAPI worker_main(void *p) { return ((MagerStream *)p)->worker(); }
	DWORD worker();
	HRESULT deliver(IUnknown *token);
	void flush_tokens();

	enum { MAX_PENDING = 16 };
	volatile LONG m_ref = 1;
	MagerSource *m_source; // слабая ссылка: стрим принадлежит источнику, иначе получится цикл ссылок
	CRITICAL_SECTION *m_lock;
	IMFMediaEventQueue *m_queue = nullptr;
	IMFStreamDescriptor *m_desc = nullptr;
	IMFAttributes *m_attrs = nullptr;
	IMFMediaType *m_type = nullptr;
	IMFVideoSampleAllocatorEx *m_allocator = nullptr;
	bool m_allocator_ready = false;
	MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
	IUnknown *m_tokens[MAX_PENDING] = {};
	int m_head = 0, m_pending = 0;
	HANDLE m_wake = nullptr, m_thread = nullptr;
	volatile bool m_quit = false;
	UINT32 m_w = 1920, m_h = 1080;
	UINT64 m_interval = 333333; // в единицах по 100 нс
	FrameProvider m_frames;
};

class MagerSource final : public IMFMediaSourceEx, public IMFGetService, public IMyKsControl, public IMySampleAllocatorControl {
public:
	explicit MagerSource(int slot) : m_slot(slot)
	{
		InitializeCriticalSection(&m_lock);
		InterlockedIncrement(&g_objects);
	}
	HRESULT init()
	{
		HRESULT hr = MFCreateEventQueue(&m_queue);
		if (FAILED(hr))
			return hr;
		hr = MFCreateAttributes(&m_attrs, 4);
		if (FAILED(hr))
			return hr;
		m_stream = new (std::nothrow) MagerStream(this, &m_lock, m_slot);
		if (!m_stream)
			return E_OUTOFMEMORY;
		hr = m_stream->init();
		if (FAILED(hr))
			return hr;
		IMFStreamDescriptor *sd = nullptr;
		m_stream->GetStreamDescriptor(&sd);
		hr = MFCreatePresentationDescriptor(1, &sd, &m_pd);
		sd->Release();
		if (FAILED(hr))
			return hr;
		return m_pd->SelectStream(0);
	}

	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaSource ||
		    riid == IID_IMFMediaSourceEx)
			*ppv = static_cast<IMFMediaSourceEx *>(this);
		else if (riid == IID_IMFGetService)
			*ppv = static_cast<IMFGetService *>(this);
		else if (riid == IID_MyKsControl)
			*ppv = static_cast<IMyKsControl *>(this);
		else if (riid == IID_MyMFSampleAllocatorControl)
			*ppv = static_cast<IMySampleAllocatorControl *>(this);
		else {
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		AddRef();
		return S_OK;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
	STDMETHODIMP_(ULONG) Release() override
	{
		LONG r = InterlockedDecrement(&m_ref);
		if (!r)
			delete this;
		return r;
	}

	STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent **ev) override
	{
		IMFMediaEventQueue *q;
		EnterCriticalSection(&m_lock);
		q = m_queue;
		if (q)
			q->AddRef();
		LeaveCriticalSection(&m_lock);
		if (!q)
			return MF_E_SHUTDOWN;
		HRESULT hr = q->GetEvent(flags, ev);
		q->Release();
		return hr;
	}
	STDMETHODIMP BeginGetEvent(IMFAsyncCallback *cb, IUnknown *state) override
	{
		EnterCriticalSection(&m_lock);
		HRESULT hr = m_queue ? m_queue->BeginGetEvent(cb, state) : MF_E_SHUTDOWN;
		LeaveCriticalSection(&m_lock);
		return hr;
	}
	STDMETHODIMP EndGetEvent(IMFAsyncResult *res, IMFMediaEvent **ev) override
	{
		EnterCriticalSection(&m_lock);
		HRESULT hr = m_queue ? m_queue->EndGetEvent(res, ev) : MF_E_SHUTDOWN;
		LeaveCriticalSection(&m_lock);
		return hr;
	}
	STDMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT st, const PROPVARIANT *v) override
	{
		EnterCriticalSection(&m_lock);
		HRESULT hr = m_queue ? m_queue->QueueEventParamVar(t, ext, st, v) : MF_E_SHUTDOWN;
		LeaveCriticalSection(&m_lock);
		return hr;
	}

	STDMETHODIMP GetCharacteristics(DWORD *c) override
	{
		if (!c)
			return E_POINTER;
		if (m_shutdown)
			return MF_E_SHUTDOWN;
		*c = MFMEDIASOURCE_IS_LIVE;
		return S_OK;
	}
	STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor **pd) override
	{
		if (!pd)
			return E_POINTER;
		EnterCriticalSection(&m_lock);
		HRESULT hr = m_shutdown ? MF_E_SHUTDOWN : m_pd->Clone(pd);
		LeaveCriticalSection(&m_lock);
		return hr;
	}
	STDMETHODIMP Start(IMFPresentationDescriptor *pd, const GUID *fmt, const PROPVARIANT *pos) override
	{
		if (!pd || !pos)
			return E_INVALIDARG;
		if (fmt && *fmt != GUID_NULL)
			return MF_E_UNSUPPORTED_TIME_FORMAT;
		if (m_shutdown)
			return MF_E_SHUTDOWN;

		BOOL selected = FALSE;
		IMFStreamDescriptor *sd = nullptr;
		HRESULT hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
		if (FAILED(hr))
			return hr;
		IMFMediaTypeHandler *mth = nullptr;
		IMFMediaType *type = nullptr;
		hr = sd->GetMediaTypeHandler(&mth);
		if (SUCCEEDED(hr))
			hr = mth->GetCurrentMediaType(&type);
		safe_release(mth);
		safe_release(sd);
		if (FAILED(hr))
			return hr;

		PROPVARIANT var;
		PropVariantInit(&var);
		var.vt = VT_UNKNOWN;
		var.punkVal = static_cast<IMyMFMediaStream2 *>(m_stream);
		QueueEvent(m_started_once ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, &var);
		m_started_once = true;

		hr = m_stream->start(type);
		type->Release();
		if (FAILED(hr))
			return hr;

		PROPVARIANT now;
		PropVariantInit(&now);
		now.vt = VT_I8;
		now.hVal.QuadPart = MFGetSystemTime();
		QueueEvent(MESourceStarted, GUID_NULL, S_OK, &now);
		return S_OK;
	}
	STDMETHODIMP Stop() override
	{
		if (m_shutdown)
			return MF_E_SHUTDOWN;
		m_stream->stop();
		QueueEvent(MESourceStopped, GUID_NULL, S_OK, nullptr);
		return S_OK;
	}
	STDMETHODIMP Pause() override { return m_shutdown ? MF_E_SHUTDOWN : MF_E_INVALID_STATE_TRANSITION; }
	STDMETHODIMP Shutdown() override
	{
		EnterCriticalSection(&m_lock);
		if (m_shutdown) {
			LeaveCriticalSection(&m_lock);
			return S_OK;
		}
		m_shutdown = true;
		LeaveCriticalSection(&m_lock);
		if (m_stream)
			m_stream->shutdown();
		EnterCriticalSection(&m_lock);
		if (m_queue)
			m_queue->Shutdown();
		safe_release(m_queue);
		LeaveCriticalSection(&m_lock);
		return S_OK;
	}

	STDMETHODIMP GetSourceAttributes(IMFAttributes **a) override
	{
		if (!a)
			return E_POINTER;
		if (m_shutdown)
			return MF_E_SHUTDOWN;
		*a = m_attrs;
		m_attrs->AddRef();
		return S_OK;
	}
	STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes **a) override
	{
		if (!a)
			return E_POINTER;
		if (m_shutdown)
			return MF_E_SHUTDOWN;
		if (id != 0)
			return MF_E_INVALIDSTREAMNUMBER;
		*a = m_stream->attributes();
		(*a)->AddRef();
		return S_OK;
	}
	STDMETHODIMP SetD3DManager(IUnknown *) override { return m_shutdown ? MF_E_SHUTDOWN : S_OK; }

	STDMETHODIMP GetService(REFGUID, REFIID, LPVOID *) override { return MF_E_UNSUPPORTED_SERVICE; }

	STDMETHODIMP KsProperty(void *, ULONG, void *, ULONG, ULONG *) override { return KS_NOT_FOUND; }
	STDMETHODIMP KsMethod(void *, ULONG, void *, ULONG, ULONG *) override { return KS_NOT_FOUND; }
	STDMETHODIMP KsEvent(void *, ULONG, void *, ULONG, ULONG *) override { return KS_NOT_FOUND; }

	STDMETHODIMP SetDefaultAllocator(DWORD id, IUnknown *alloc) override
	{
		if (id != 0)
			return MF_E_INVALIDSTREAMNUMBER;
		IMFVideoSampleAllocatorEx *a = nullptr;
		if (alloc)
			alloc->QueryInterface(IID_IMFVideoSampleAllocatorEx, (void **)&a);
		m_stream->set_allocator(a);
		safe_release(a);
		return S_OK;
	}
	STDMETHODIMP GetAllocatorUsage(DWORD id, DWORD *inId, DWORD *usage) override
	{
		if (!inId || !usage)
			return E_POINTER;
		if (id != 0)
			return MF_E_INVALIDSTREAMNUMBER;
		*inId = id;
		*usage = UsesProvidedAllocator;
		return S_OK;
	}

private:
	~MagerSource()
	{
		Shutdown();
		if (m_stream)
			m_stream->Release();
		safe_release(m_pd);
		safe_release(m_attrs);
		DeleteCriticalSection(&m_lock);
		InterlockedDecrement(&g_objects);
	}

	volatile LONG m_ref = 1;
	int m_slot;
	CRITICAL_SECTION m_lock;
	IMFMediaEventQueue *m_queue = nullptr;
	IMFPresentationDescriptor *m_pd = nullptr;
	IMFAttributes *m_attrs = nullptr;
	MagerStream *m_stream = nullptr;
	volatile bool m_shutdown = false;
	bool m_started_once = false;
};

static HRESULT make_type(UINT32 w, UINT32 h, UINT32 fps, IMFMediaType **out)
{
	IMFMediaType *t = nullptr;
	HRESULT hr = MFCreateMediaType(&t);
	if (FAILED(hr))
		return hr;
	t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
	MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
	MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps, 1);
	MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
	t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
	t->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
	t->SetUINT32(MF_MT_SAMPLE_SIZE, w * h * 3 / 2);
	*out = t;
	return S_OK;
}

static void set_stream_attrs(IMFAttributes *a)
{
	a->SetGUID(MY_MF_DEVICESTREAM_STREAM_CATEGORY, MY_PINNAME_VIDEO_CAPTURE);
	a->SetUINT32(MY_MF_DEVICESTREAM_STREAM_ID, 0);
	a->SetUINT32(MY_MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
	a->SetUINT32(MY_MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, 1 /* MFFrameSourceTypes_Color */);
}

HRESULT MagerStream::init()
{
	HRESULT hr = MFCreateEventQueue(&m_queue);
	if (FAILED(hr))
		return hr;
	m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	// 1080p идёт первым: его берут программы, которые не спрашивают формат, а 4K им обычно не нужен
	static const UINT32 sizes[][2] = {{1920, 1080}, {3840, 2160}, {2560, 1440}, {1280, 720}, {960, 540}, {640, 360}};
	static const UINT32 fps_list[] = {30, 60};
	IMFMediaType *types[12] = {};
	DWORD n = 0;
	for (auto &s : sizes) {
		for (UINT32 fps : fps_list) {
			if (SUCCEEDED(make_type(s[0], s[1], fps, &types[n])))
				n++;
		}
	}
	hr = MFCreateStreamDescriptor(0, n, types, &m_desc);
	if (SUCCEEDED(hr)) {
		IMFMediaTypeHandler *mth = nullptr;
		if (SUCCEEDED(m_desc->GetMediaTypeHandler(&mth))) {
			mth->SetCurrentMediaType(types[0]);
			mth->Release();
		}
		set_stream_attrs(m_desc);
	}
	for (DWORD i = 0; i < n; i++)
		types[i]->Release();
	if (FAILED(hr))
		return hr;
	hr = MFCreateAttributes(&m_attrs, 8);
	if (FAILED(hr))
		return hr;
	set_stream_attrs(m_attrs);
	return S_OK;
}

STDMETHODIMP MagerStream::GetMediaSource(IMFMediaSource **ppSource)
{
	if (!ppSource)
		return E_POINTER;
	EnterCriticalSection(m_lock);
	HRESULT hr = MF_E_SHUTDOWN;
	if (m_queue && m_source) {
		hr = m_source->QueryInterface(IID_IMFMediaSource, (void **)ppSource);
	}
	LeaveCriticalSection(m_lock);
	return hr;
}

HRESULT MagerStream::start(IMFMediaType *type)
{
	UINT32 w = 0, h = 0, num = 30, den = 1;
	MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
	MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
	if (!w || !h)
		return MF_E_INVALIDMEDIATYPE;
	EnterCriticalSection(m_lock);
	safe_release(m_type);
	m_type = type;
	type->AddRef();
	m_w = w;
	m_h = h;
	m_interval = num ? (UINT64)10000000 * (den ? den : 1) / num : 333333;
	m_allocator_ready = false;
	if (m_allocator) {
		m_allocator->UninitializeSampleAllocator();
		m_allocator_ready = SUCCEEDED(m_allocator->InitializeSampleAllocator(10, type));
	}
	m_state = MF_STREAM_STATE_RUNNING;
	LeaveCriticalSection(m_lock);
	dbg("MagerCam vcam: start %ux%u @ %u/%u\n", w, h, num, den);
	start_worker();
	QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr);
	return S_OK;
}

HRESULT MagerStream::stop()
{
	EnterCriticalSection(m_lock);
	m_state = MF_STREAM_STATE_STOPPED;
	LeaveCriticalSection(m_lock);
	stop_worker();
	QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr);
	return S_OK;
}

void MagerStream::shutdown()
{
	EnterCriticalSection(m_lock);
	m_state = MF_STREAM_STATE_STOPPED;
	LeaveCriticalSection(m_lock);
	stop_worker();
	EnterCriticalSection(m_lock);
	if (m_queue)
		m_queue->Shutdown();
	safe_release(m_queue);
	safe_release(m_desc);
	safe_release(m_attrs);
	safe_release(m_type);
	if (m_allocator)
		m_allocator->UninitializeSampleAllocator();
	safe_release(m_allocator);
	m_source = nullptr;
	LeaveCriticalSection(m_lock);
	if (m_wake) {
		CloseHandle(m_wake);
		m_wake = nullptr;
	}
}

void MagerStream::flush_tokens()
{
	EnterCriticalSection(m_lock);
	while (m_pending > 0) {
		IUnknown *t = m_tokens[m_head];
		m_tokens[m_head] = nullptr;
		m_head = (m_head + 1) % MAX_PENDING;
		m_pending--;
		if (t)
			t->Release();
	}
	LeaveCriticalSection(m_lock);
}

void MagerStream::start_worker()
{
	if (m_thread)
		return;
	m_quit = false;
	m_thread = CreateThread(nullptr, 0, worker_main, this, 0, nullptr);
}

void MagerStream::stop_worker()
{
	if (m_thread) {
		m_quit = true;
		if (m_wake)
			SetEvent(m_wake);
		WaitForSingleObject(m_thread, 5000);
		CloseHandle(m_thread);
		m_thread = nullptr;
	}
	flush_tokens();
}

DWORD MagerStream::worker()
{
	m_frames.open();
	EnterCriticalSection(m_lock);
	m_frames.set_streaming(true, m_w, m_h);
	LeaveCriticalSection(m_lock);
	// Беру таймер высокого разрешения: со стандартным тиком 15,6 мс вместо 30 кадров/с выходит около 21
	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
	if (!timer)
		timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
	LARGE_INTEGER first;
	first.QuadPart = -10000;
	SetWaitableTimer(timer, &first, 1, nullptr, nullptr, FALSE);
	HANDLE waits[2] = {m_wake, timer};
	LONGLONG due = 0;
	int32_t last_seq = -1;
	while (!m_quit) {
		WaitForMultipleObjects(2, waits, FALSE, 20);
		if (m_quit)
			break;
		EnterCriticalSection(m_lock);
		bool have = m_pending > 0 && m_state == MF_STREAM_STATE_RUNNING;
		LONGLONG interval = (LONGLONG)m_interval;
		LeaveCriticalSection(m_lock);
		if (!have)
			continue;
		LONGLONG now = MFGetSystemTime();
		// Держу ровный темп по согласованной частоте кадров, но свежий кадр клиента отдаю до полкадра
		// раньше срока: так следую таймингу телефона и не добавляю задержку.
		bool fresh = false;
		if (m_frames.base) {
			auto *h = (mcv_header *)m_frames.base;
			fresh = !(h->seq & 1) && h->seq != last_seq && h->writer_tick &&
				GetTickCount64() - (uint64_t)h->writer_tick < 3000;
		}
		if (due == 0 || now - due > interval)
			due = now;
		if (!(now >= due || (fresh && now >= due - interval / 2)))
			continue;
		IUnknown *token = nullptr;
		EnterCriticalSection(m_lock);
		if (m_pending > 0) {
			token = m_tokens[m_head];
			m_tokens[m_head] = nullptr;
			m_head = (m_head + 1) % MAX_PENDING;
			m_pending--;
		}
		LeaveCriticalSection(m_lock);
		if (m_frames.base)
			last_seq = ((mcv_header *)m_frames.base)->seq;
		HRESULT hr = deliver(token);
		if (token)
			token->Release();
		if (FAILED(hr))
			dbg("MagerCam vcam: deliver failed %08lx\n", hr);
		due += interval;
	}
	CancelWaitableTimer(timer);
	CloseHandle(timer);
	m_frames.set_streaming(false, 0, 0);
	m_frames.close();
	return 0;
}

HRESULT MagerStream::deliver(IUnknown *token)
{
	IMFSample *sample = nullptr;
	HRESULT hr = E_FAIL;
	EnterCriticalSection(m_lock);
	UINT32 w = m_w, h = m_h;
	UINT64 interval = m_interval;
	if (m_allocator && m_allocator_ready)
		hr = m_allocator->AllocateSample(&sample);
	LeaveCriticalSection(m_lock);
	if (FAILED(hr)) {
		IMFMediaBuffer *buf = nullptr;
		hr = MFCreateSample(&sample);
		if (SUCCEEDED(hr))
			hr = MFCreate2DMediaBuffer(w, h, MFVideoFormat_NV12.Data1, FALSE, &buf);
		if (SUCCEEDED(hr))
			hr = sample->AddBuffer(buf);
		safe_release(buf);
		if (FAILED(hr)) {
			safe_release(sample);
			return hr;
		}
	}

	IMFMediaBuffer *buf = nullptr;
	hr = sample->GetBufferByIndex(0, &buf);
	if (FAILED(hr)) {
		sample->Release();
		return hr;
	}
	IMF2DBuffer *b2 = nullptr;
	BYTE *scan0 = nullptr;
	LONG pitch = 0;
	bool locked2d = false;
	if (SUCCEEDED(buf->QueryInterface(IID_IMF2DBuffer, (void **)&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch)))
		locked2d = true;
	BYTE *plain = nullptr;
	if (!locked2d) {
		DWORD maxLen = 0;
		hr = buf->Lock(&plain, &maxLen, nullptr);
		if (FAILED(hr) || maxLen < w * h * 3 / 2) {
			if (SUCCEEDED(hr))
				buf->Unlock();
			safe_release(b2);
			buf->Release();
			sample->Release();
			return FAILED(hr) ? hr : E_FAIL;
		}
		scan0 = plain;
		pitch = (LONG)w;
	}
	if (pitch > 0) {
		struct nv12 d = {scan0, scan0 + (size_t)pitch * h, (int)pitch, (int)pitch, (int)w, (int)h};
		m_frames.render(&d);
	}
	if (locked2d)
		b2->Unlock2D();
	else {
		buf->Unlock();
		buf->SetCurrentLength(w * h * 3 / 2);
	}
	safe_release(b2);
	buf->Release();

	sample->SetSampleTime(MFGetSystemTime());
	sample->SetSampleDuration((LONGLONG)interval);
	if (token)
		sample->SetUnknown(MY_MFSampleExtension_Token, token);

	PROPVARIANT var;
	PropVariantInit(&var);
	var.vt = VT_UNKNOWN;
	var.punkVal = sample;
	hr = QueueEvent(MEMediaSample, GUID_NULL, S_OK, &var);
	sample->Release();
	return hr;
}

// Активатор: именно его Frame Server создаёт через COM
class MagerActivate final : public IMFActivate {
public:
	explicit MagerActivate(int slot) : m_slot(slot) { InterlockedIncrement(&g_objects); }
	HRESULT init() { return MFCreateAttributes(&m_attrs, 4); }

	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IMFAttributes || riid == IID_IMFActivate) {
			*ppv = static_cast<IMFActivate *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
	STDMETHODIMP_(ULONG) Release() override
	{
		LONG r = InterlockedDecrement(&m_ref);
		if (!r)
			delete this;
		return r;
	}

	STDMETHODIMP ActivateObject(REFIID riid, void **ppv) override
	{
		if (!ppv)
			return E_POINTER;
		*ppv = nullptr;
		if (!m_source) {
			auto *s = new (std::nothrow) MagerSource(m_slot);
			if (!s)
				return E_OUTOFMEMORY;
			HRESULT hr = s->init();
			if (FAILED(hr)) {
				dbg("MagerCam vcam: source init failed %08lx\n", hr);
				s->Shutdown();
				s->Release();
				return hr;
			}
			m_source = s;
		}
		return m_source->QueryInterface(riid, ppv);
	}
	STDMETHODIMP ShutdownObject() override
	{
		if (m_source) {
			m_source->Shutdown();
			m_source->Release();
			m_source = nullptr;
		}
		return S_OK;
	}
	STDMETHODIMP DetachObject() override
	{
		safe_release(m_source);
		return S_OK;
	}

#define FWD(name, params, args) \
	STDMETHODIMP name params override { return m_attrs->name args; }
	FWD(GetItem, (REFGUID k, PROPVARIANT *v), (k, v))
	FWD(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE *t), (k, t))
	FWD(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL *r), (k, v, r))
	FWD(Compare, (IMFAttributes * a, MF_ATTRIBUTES_MATCH_TYPE t, BOOL *r), (a, t, r))
	FWD(GetUINT32, (REFGUID k, UINT32 *v), (k, v))
	FWD(GetUINT64, (REFGUID k, UINT64 *v), (k, v))
	FWD(GetDouble, (REFGUID k, double *v), (k, v))
	FWD(GetGUID, (REFGUID k, GUID *v), (k, v))
	FWD(GetStringLength, (REFGUID k, UINT32 *v), (k, v))
	FWD(GetString, (REFGUID k, LPWSTR v, UINT32 n, UINT32 *l), (k, v, n, l))
	FWD(GetAllocatedString, (REFGUID k, LPWSTR *v, UINT32 *l), (k, v, l))
	FWD(GetBlobSize, (REFGUID k, UINT32 *v), (k, v))
	FWD(GetBlob, (REFGUID k, UINT8 *v, UINT32 n, UINT32 *l), (k, v, n, l))
	FWD(GetAllocatedBlob, (REFGUID k, UINT8 **v, UINT32 *l), (k, v, l))
	FWD(GetUnknown, (REFGUID k, REFIID r, LPVOID *v), (k, r, v))
	FWD(SetItem, (REFGUID k, REFPROPVARIANT v), (k, v))
	FWD(DeleteItem, (REFGUID k), (k))
	FWD(DeleteAllItems, (), ())
	FWD(SetUINT32, (REFGUID k, UINT32 v), (k, v))
	FWD(SetUINT64, (REFGUID k, UINT64 v), (k, v))
	FWD(SetDouble, (REFGUID k, double v), (k, v))
	FWD(SetGUID, (REFGUID k, REFGUID v), (k, v))
	FWD(SetString, (REFGUID k, LPCWSTR v), (k, v))
	FWD(SetBlob, (REFGUID k, const UINT8 *v, UINT32 n), (k, v, n))
	FWD(SetUnknown, (REFGUID k, IUnknown * v), (k, v))
	FWD(LockStore, (), ())
	FWD(UnlockStore, (), ())
	FWD(GetCount, (UINT32 * n), (n))
	FWD(GetItemByIndex, (UINT32 i, GUID *k, PROPVARIANT *v), (i, k, v))
	FWD(CopyAllItems, (IMFAttributes * d), (d))
#undef FWD

private:
	~MagerActivate()
	{
		safe_release(m_source);
		safe_release(m_attrs);
		InterlockedDecrement(&g_objects);
	}
	volatile LONG m_ref = 1;
	IMFAttributes *m_attrs = nullptr;
	MagerSource *m_source = nullptr;
	int m_slot;
};

class ClassFactory : public IClassFactory {
public:
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IClassFactory) {
			*ppv = static_cast<IClassFactory *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return 2; }
	STDMETHODIMP_(ULONG) Release() override { return 1; }
	STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override
	{
		if (!ppv)
			return E_POINTER;
		*ppv = nullptr;
		if (outer)
			return CLASS_E_NOAGGREGATION;
		auto *a = new (std::nothrow) MagerActivate(m_slot);
		if (!a)
			return E_OUTOFMEMORY;
		HRESULT hr = a->init();
		if (SUCCEEDED(hr))
			hr = a->QueryInterface(riid, ppv);
		a->Release();
		return hr;
	}
	STDMETHODIMP LockServer(BOOL lock) override
	{
		if (lock)
			InterlockedIncrement(&g_objects);
		else
			InterlockedDecrement(&g_objects);
		return S_OK;
	}
	int m_slot = 0;
};
static ClassFactory g_factory[MCV_MAX_SLOTS];

// CLSID камеры номер slot: базовый, у которого последний байт 41 + slot
static int clsid_slot(REFCLSID clsid)
{
	GUID base = CLSID_MagerCamSource;
	base.Data4[7] = clsid.Data4[7];
	if (memcmp(&base, &clsid, sizeof(GUID)) != 0)
		return -1;
	int slot = clsid.Data4[7] - CLSID_MagerCamSource.Data4[7];
	return slot >= 0 && slot < MCV_MAX_SLOTS ? slot : -1;
}

static void clsid_string(int slot, wchar_t *out, int n)
{
	GUID g = CLSID_MagerCamSource;
	g.Data4[7] = (BYTE)(g.Data4[7] + slot);
	StringFromGUID2(g, out, n);
}

extern "C" {

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *)
{
	if (reason == DLL_PROCESS_ATTACH) {
		g_module = inst;
		DisableThreadLibraryCalls(inst);
		for (int i = 0; i < MCV_MAX_SLOTS; i++)
			g_factory[i].m_slot = i;
	}
	return TRUE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
	int slot = clsid_slot(clsid);
	if (slot < 0)
		return CLASS_E_CLASSNOTAVAILABLE;
	return g_factory[slot].QueryInterface(riid, ppv);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return g_objects == 0 ? S_OK : S_FALSE;
}

HRESULT WINAPI DllRegisterServer(void)
{
	wchar_t clsid[64], key[160], path[MAX_PATH];
	GetModuleFileNameW(g_module, path, MAX_PATH);
	for (int slot = 0; slot < MCV_MAX_SLOTS; slot++) {
		clsid_string(slot, clsid, 64);
		// Регистрирую в HKLM, а не в HKCU: Frame Server работает под LOCAL SERVICE и HKCU пользователя не видит
		wsprintfW(key, L"Software\\Classes\\CLSID\\%s", clsid);
		HKEY k;
		if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
			return SELFREG_E_CLASS;
		const wchar_t *name = L"MagerCam Virtual Camera";
		RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE *)name, (DWORD)(wcslen(name) + 1) * 2);
		RegCloseKey(k);
		wsprintfW(key, L"Software\\Classes\\CLSID\\%s\\InprocServer32", clsid);
		if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
			return SELFREG_E_CLASS;
		RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE *)path, (DWORD)(wcslen(path) + 1) * 2);
		const wchar_t *tm = L"Both";
		RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, (const BYTE *)tm, (DWORD)(wcslen(tm) + 1) * 2);
		RegCloseKey(k);
	}
	return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
	wchar_t clsid[64], key[160];
	HRESULT hr = S_OK;
	for (int slot = 0; slot < MCV_MAX_SLOTS; slot++) {
		clsid_string(slot, clsid, 64);
		wsprintfW(key, L"Software\\Classes\\CLSID\\%s", clsid);
		LSTATUS r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
		if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND)
			hr = SELFREG_E_CLASS;
	}
	return hr;
}

}
