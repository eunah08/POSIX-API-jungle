// [http_response.c] HTTP 응답 생성 구현
#include "http_response.h"
#include <stdio.h>
#include <string.h>
#include "rio.h"

// 상태 코드 → 설명 문구
const char *status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

// 상태 라인과 헤더만 보낸다 (본문은 호출한 쪽이 따로 보낸다)
int send_headers(int fd, int status, const char *content_type, size_t content_length, int keep_alive) {
    // Content-Type이 없으면 "내용을 모르는 바이너리"로 표시한다
    if (content_type == NULL) {
        content_type = "application/octet-stream";
    }

    // 연결을 유지할지 닫을지
    const char *connection = "close";
    if (keep_alive) {
        connection = "keep-alive";
    }

    char header[512];
    int length = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: %s\r\n"
                          "\r\n",
                          status, status_text(status), content_type, content_length, connection);
    if (length < 0 || length >= (int)sizeof(header)) {
        return -1;                  // 헤더가 버퍼보다 길어짐
    }

    if (rio_writen(fd, header, length) < 0) {
        return -1;
    }
    return 0;
}

// 헤더 + 본문을 한꺼번에 보낸다
int send_response(int fd, int status, const char *content_type, const void *body, size_t len, int keep_alive) {
    if (send_headers(fd, status, content_type, len, keep_alive) < 0) {
        return -1;
    }
    if (len > 0) {
        if (rio_writen(fd, body, len) < 0) {
            return -1;
        }
    }
    return 0;
}

// 오류 상태 코드에 맞는 짧은 HTML 페이지를 보낸다
int send_error(int fd, int status, int keep_alive) {
    char body[256];
    int length = snprintf(body, sizeof(body),
                          "<html><head><title>%d %s</title></head>"
                          "<body><h1>%d %s</h1></body></html>\n",
                          status, status_text(status), status, status_text(status));
    if (length < 0 || length >= (int)sizeof(body)) {
        return -1;
    }
    return send_response(fd, status, "text/html", body, length, keep_alive);
}
