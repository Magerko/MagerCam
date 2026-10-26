/*
 * Shared memory between MagerCam.exe (writer) and the virtual camera media source
 * (magercam-vcam.dll, loaded by the Windows Frame Server service).
 *
 * The media source runs as LOCAL SERVICE, which may create objects in the Global\ namespace,
 * while a normal user process may not. So the DLL creates the mapping (with a DACL that lets the
 * user's client open it) and the client opens it whenever an application is watching the camera.
 *
 * Frame exchange is a seqlock: the writer makes `seq` odd, writes the NV12 frame, makes it even.
 * The reader copies the frame and retries if `seq` changed or was odd.
 */
#pragma once
#include <stdint.h>

#define MCV_SHM_NAME L"Global\\MagerCamVirtualCamera"
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
	/* written by the media source */
	volatile uint32_t want_w, want_h; /* format the application picked */
	volatile int32_t streaming;      /* 1 while an application is reading frames */
	volatile uint32_t source_pid;
	/* written by the client */
	volatile int32_t seq;            /* seqlock */
	volatile uint32_t width, height; /* NV12, stride == width */
	volatile int64_t writer_tick;    /* GetTickCount64() of the last frame */
	volatile uint32_t writer_pid;
};

/* Frame data starts at MCV_HEADER_SIZE. */
static inline uint8_t *mcv_frame(void *base)
{
	return (uint8_t *)base + MCV_HEADER_SIZE;
}
