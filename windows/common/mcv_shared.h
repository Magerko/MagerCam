/*
 * Общая память между MagerCam.exe (пишет кадры) и медиаисточником виртуальной камеры
 * (magercam-vcam.dll, его грузит служба Frame Server).
 *
 * Источник работает под LOCAL SERVICE и может создавать объекты в Global\, а обычный
 * пользовательский процесс не может. Поэтому маппинг создаёт DLL, с DACL, которая пускает клиента,
 * а клиент только открывает его, пока какое-то приложение смотрит в камеру.
 *
 * Кадры передаю через seqlock: писатель делает `seq` нечётным, пишет кадр NV12, делает чётным.
 * Читатель копирует кадр и повторяет попытку, если `seq` был нечётным или изменился.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#define MCV_SHM_NAME L"Global\\MagerCamVirtualCamera"
#define MCV_MAX_SLOTS 4 /* «MagerCam», «MagerCam 2»… — по камере на телефон */
#define MCV_MAGIC 0x4D435643u /* 'MCVC' */
#define MCV_VERSION 1
#define MCV_MAX_W 1920
#define MCV_MAX_H 1080
#define MCV_HEADER_SIZE 4096
#define MCV_FRAME_MAX ((size_t)MCV_MAX_W * MCV_MAX_H * 3 / 2)
#define MCV_SHM_SIZE (MCV_HEADER_SIZE + MCV_FRAME_MAX)

struct mcv_header {
	uint32_t magic;
	uint32_t version;
	/* пишет медиаисточник */
	volatile uint32_t want_w, want_h; /* формат, который выбрало приложение */
	volatile int32_t streaming;      /* 1, пока приложение читает кадры */
	volatile uint32_t source_pid;
	/* пишет клиент */
	volatile int32_t seq;            /* счётчик seqlock, нечётный во время записи */
	volatile uint32_t width, height; /* NV12, шаг строки равен ширине */
	volatile int64_t writer_tick;    /* GetTickCount64() последнего кадра, по нему понимаю, жив ли клиент */
	volatile uint32_t writer_pid;
};

/* у первой камеры имя прежнее, чтобы старые клиенты и DLL понимали друг друга */
static inline void mcv_shm_name(int slot, const wchar_t *prefix, wchar_t *out, int n)
{
	if (slot <= 0)
		_snwprintf(out, n, L"%lsMagerCamVirtualCamera", prefix);
	else
		_snwprintf(out, n, L"%lsMagerCamVirtualCamera%d", prefix, slot + 1);
	out[n - 1] = 0;
}

static inline uint8_t *mcv_frame(void *base)
{
	return (uint8_t *)base + MCV_HEADER_SIZE;
}
