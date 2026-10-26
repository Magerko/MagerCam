/* Client side of the virtual camera: registers the "MagerCam" camera for this session and
 * writes frames into the shared memory read by magercam-vcam.dll. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../common/mcv_nv12.h"

enum vcam_state {
	VCAM_OFF,
	VCAM_NOT_INSTALLED, /* DLL not registered: run the installer */
	VCAM_UNSUPPORTED,   /* Windows 10 or older */
	VCAM_FAILED,
	VCAM_READY,
};

enum vcam_state vcam_start(long *hr_out);
void vcam_stop(void);
/* true while some application reads the camera */
bool vcam_in_use(void);
/* Rotates/mirrors/fits the frame into the size the application asked for and publishes it. */
void vcam_write(const struct nv12 *src, int rot, bool mirror, bool fill);
