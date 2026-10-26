#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdbool.h>
#include <stdint.h>

#define MC_DEFAULT_PORT 4848
#define MC_DISCOVERY_PORT 4849

enum {
	PKT_VIDEO_CONFIG = 1,
	PKT_VIDEO_FRAME = 2,
	PKT_AUDIO_CONFIG = 3,
	PKT_AUDIO = 4,
	PKT_STATUS = 5,
	PKT_PING = 6,
};

SOCKET tcp_connect(const char *host, int port, int timeout_ms);
void set_recv_timeout(SOCKET sock, int ms);
bool recv_all(SOCKET sock, uint8_t *buf, size_t n);
bool send_all(SOCKET sock, const char *buf, size_t n);
int read_http_header(SOCKET sock);
bool http_get(const char *host, int port, const char *path);
void url_encode(char *out, size_t outsz, const char *v);

struct found_device {
	char name[96];
	char ip[64];
	int port;
};
int discovery_run(int timeout_ms, struct found_device *out, int max);

bool find_adb(const char *configured, char *out, size_t sz);
/* при ошибке кладу в err текст для пользователя в UTF-8 */
bool adb_prepare(const char *adb_cfg, int port, char *err, size_t errsz);

enum json_type { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ };
struct json {
	enum json_type type;
	char *key;
	char *str;
	double num;   /* для JSON_BOOL тоже здесь, 0 или 1 */
	struct json *child, *next;
};
struct json *json_parse(const char *text);
void json_free(struct json *j);
const struct json *json_get(const struct json *obj, const char *key);
const char *json_str(const struct json *obj, const char *key, const char *def);
double json_num(const struct json *obj, const char *key, double def);
bool json_bool(const struct json *obj, const char *key, bool def);

wchar_t *utf8_to_wide(const char *s); /* результат освобождаю через free() */
void wide_to_utf8(const wchar_t *w, char *out, size_t sz);
