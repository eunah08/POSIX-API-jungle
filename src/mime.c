// [mime.c] MIME 타입 매핑 구현
#include "mime.h"
#include <string.h>
#include <strings.h>

// 확장자와 Content-Type을 짝지어 둔 표
typedef struct {
    const char *extension;          // '.' 없는 확장자
    const char *type;
} mime_entry;

static const mime_entry mime_table[] = {
    {"html", "text/html"},
    {"htm",  "text/html"},
    {"css",  "text/css"},
    {"js",   "application/javascript"},
    {"png",  "image/png"},
    {"jpg",  "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif",  "image/gif"},
    {"txt",  "text/plain"},
    {"json", "application/json"},
};

#define MIME_COUNT ((int)(sizeof(mime_table) / sizeof(mime_table[0])))   // 표에 들어 있는 항목 수

// 파일 경로의 확장자를 보고 Content-Type을 돌려준다. 모르면 "application/octet-stream"
const char *mime_type(const char *path) {
    const char *last_slash = strrchr(path, '/');
    const char *last_dot = strrchr(path, '.');

    // 확장자가 없는 경우
    if (last_dot == NULL) {
        return "application/octet-stream";
    }
    // '.'이 마지막 '/'보다 앞이면 디렉터리 이름의 점이다 ("/dir.v1/file")
    if (last_slash != NULL && last_dot < last_slash) {
        return "application/octet-stream";
    }

    const char *extension = last_dot + 1;
    for (int i = 0; i < MIME_COUNT; i++) {
        if (strcasecmp(extension, mime_table[i].extension) == 0) {
            return mime_table[i].type;
        }
    }
    return "application/octet-stream";
}
