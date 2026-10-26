/* Камеру "MagerCam" регистрирую только на время сессии, а кадры пишу в общую память,
 * откуда их забирает magercam-vcam.dll внутри Frame Server. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../common/mcv_nv12.h"

enum vcam_state {
	VCAM_OFF,
	VCAM_NOT_INSTALLED, /* DLL не зарегистрирована, нужен установщик */
	VCAM_UNSUPPORTED,   /* Windows 10 и более ранние: там нет MFCreateVirtualCamera */
	VCAM_FAILED,
	VCAM_READY,
};

enum vcam_state vcam_start(long *hr_out);
void vcam_stop(void);
bool vcam_in_use(void);
/* кадр подгоняю под размер, который запросило приложение, а не под размер с телефона */
void vcam_write(const struct nv12 *src, int rot, bool mirror, bool fill);
