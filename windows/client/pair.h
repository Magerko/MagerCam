/* Сопряжение по QR: показываю код с адресами ПК и одноразовым nonce и жду, пока телефон
 * сам подключится и пришлёт свой адрес, порт и код доступа. */
#pragma once
#include <windows.h>
#include <stdbool.h>

#define PAIR_PORT_FIRST 4850
#define PAIR_PORT_LAST 4859

struct pair_result {
	char name[96];
	char ip[64];
	int port;
	char key[16];
};

/* при успехе шлю owner сообщение done_msg, в lParam — struct pair_result*, освобождать через free() */
bool pair_show(HWND owner, HINSTANCE inst, HFONT font, int dpi, UINT done_msg);
void pair_close(void);
