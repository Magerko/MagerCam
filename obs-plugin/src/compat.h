/*
 * Плагин писался под Windows, и в нём остались вызовы Win32: потоки, критические секции, события, сокеты.
 * Для Linux я не переписываю их по всему файлу, а даю здесь те же имена поверх POSIX,
 * чтобы основная логика оставалась одной на обе системы.
 */
#pragma once
#include <stdbool.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mswsock.h>
#include <windows.h>

#define MC_ADB_NAME "adb.exe"
#define MC_HW_LABEL "Аппаратное декодирование (D3D11)"
#define mc_strtok strtok_s

#else /* POSIX */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MC_ADB_NAME "adb"
#define MC_HW_LABEL "Аппаратное декодирование (VA-API)"
#define mc_strtok strtok_r

typedef int SOCKET;
typedef long LONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long DWORD;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SD_BOTH SHUT_RDWR
#define closesocket close

typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *m)
{
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	/* критическая секция Windows рекурсивна, повторяю это, чтобы поведение не разошлось */
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(m, &a);
	pthread_mutexattr_destroy(&a);
}
#define DeleteCriticalSection(m) pthread_mutex_destroy(m)
#define EnterCriticalSection(m) pthread_mutex_lock(m)
#define LeaveCriticalSection(m) pthread_mutex_unlock(m)

#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)

static inline ULONGLONG GetTickCount64(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)ts.tv_nsec / 1000000ULL;
}

static inline void Sleep(DWORD ms)
{
	struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
	}
}

/* Событие с автосбросом, как CreateEvent(NULL, FALSE, FALSE, NULL) */
struct mc_event {
	pthread_mutex_t m;
	pthread_cond_t c;
	bool set;
};
typedef struct mc_event *mc_event_t;

static inline mc_event_t mc_event_create(void)
{
	struct mc_event *e = calloc(1, sizeof(*e));
	pthread_mutex_init(&e->m, NULL);
	pthread_condattr_t a;
	pthread_condattr_init(&a);
	pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
	pthread_cond_init(&e->c, &a);
	pthread_condattr_destroy(&a);
	return e;
}

static inline void mc_event_set(mc_event_t e)
{
	pthread_mutex_lock(&e->m);
	e->set = true;
	pthread_cond_signal(&e->c);
	pthread_mutex_unlock(&e->m);
}

static inline void mc_event_wait(mc_event_t e, DWORD ms)
{
	struct timespec until;
	clock_gettime(CLOCK_MONOTONIC, &until);
	until.tv_sec += ms / 1000;
	until.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (until.tv_nsec >= 1000000000L) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&e->m);
	while (!e->set)
		if (pthread_cond_timedwait(&e->c, &e->m, &until) == ETIMEDOUT)
			break;
	e->set = false;
	pthread_mutex_unlock(&e->m);
}

static inline void mc_event_destroy(mc_event_t e)
{
	pthread_cond_destroy(&e->c);
	pthread_mutex_destroy(&e->m);
	free(e);
}

typedef pthread_t mc_thread_t;
#define MC_THREAD_FN(name, arg) static void *name(void *arg)
#define MC_THREAD_RETURN return NULL

static inline bool mc_thread_start(mc_thread_t *t, void *(*fn)(void *), void *arg)
{
	return pthread_create(t, NULL, fn, arg) == 0;
}
static inline void mc_thread_join(mc_thread_t t)
{
	pthread_join(t, NULL);
}
static inline bool mc_thread_detached(void *(*fn)(void *), void *arg)
{
	pthread_t t;
	if (pthread_create(&t, NULL, fn, arg) != 0)
		return false;
	pthread_detach(t);
	return true;
}

static inline void mc_set_nonblock(SOCKET s, bool on)
{
	int fl = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
}
static inline bool mc_connect_pending(void)
{
	return errno == EINPROGRESS;
}

#endif

#ifdef _WIN32
typedef HANDLE mc_event_t;
static inline mc_event_t mc_event_create(void)
{
	return CreateEventW(NULL, FALSE, FALSE, NULL);
}
#define mc_event_set(e) SetEvent(e)
#define mc_event_wait(e, ms) WaitForSingleObject((e), (ms))
#define mc_event_destroy(e) CloseHandle(e)

typedef HANDLE mc_thread_t;
#define MC_THREAD_FN(name, arg) static DWORD WINAPI name(LPVOID arg)
#define MC_THREAD_RETURN return 0

static inline bool mc_thread_start(mc_thread_t *t, LPTHREAD_START_ROUTINE fn, void *arg)
{
	*t = CreateThread(NULL, 0, fn, arg, 0, NULL);
	return *t != NULL;
}
static inline void mc_thread_join(mc_thread_t t)
{
	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);
}
static inline bool mc_thread_detached(LPTHREAD_START_ROUTINE fn, void *arg)
{
	HANDLE h = CreateThread(NULL, 0, fn, arg, 0, NULL);
	if (!h)
		return false;
	CloseHandle(h);
	return true;
}

static inline void mc_set_nonblock(SOCKET s, bool on)
{
	u_long nb = on ? 1 : 0;
	ioctlsocket(s, FIONBIO, &nb);
}
static inline bool mc_connect_pending(void)
{
	return WSAGetLastError() == WSAEWOULDBLOCK;
}
#endif
