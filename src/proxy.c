// [proxy.c] 프록시 공통 중계 + Reverse Proxy 구현
#include "proxy.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "net.h"
#include "rio.h"

// 요청 머리(요청 라인 + 헤더 전부)가 들어갈 버퍼 크기
// 요청 라인은 URI 최대 길이 + 여유, 헤더는 최대 개수 x (이름 + 값 + ": " + 줄바꿈)이면 항상 충분하다
#define REQUEST_HEAD_SIZE (MAX_URI_LEN + 128 + MAX_HEADERS * (MAX_HEADER_NAME + MAX_HEADER_VALUE + 8))

// 연결 관리용 헤더이면 1. 이런 헤더는 그대로 전달하지 않는다 (우리가 Connection: close를 새로 붙인다)
static int is_connection_header(const char *name) {
    if (strcasecmp(name, "Connection") == 0) {
        return 1;
    }
    if (strcasecmp(name, "Proxy-Connection") == 0) {
        return 1;
    }
    if (strcasecmp(name, "Keep-Alive") == 0) {
        return 1;
    }
    return 0;
}

// 백엔드로 보낼 요청 머리를 head에 문자열로 만든다. 성공 0, 버퍼가 모자라면 -1
// 예) "GET /a.html HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n"
static int build_request_head(char *head, size_t head_size, const http_request *req, const char *target) {
    char line[MAX_URI_LEN + 128];

    // 요청 라인
    snprintf(line, sizeof(line), "%s %s %s\r\n", req->method, target, req->version);
    if (strlen(line) >= head_size) {
        return -1;
    }
    strcpy(head, line);

    // 헤더들 (연결 관리용 헤더는 뺀다)
    for (int i = 0; i < req->header_count; i++) {
        if (is_connection_header(req->headers[i].name)) {
            continue;
        }
        snprintf(line, sizeof(line), "%s: %s\r\n", req->headers[i].name, req->headers[i].value);
        if (strlen(head) + strlen(line) >= head_size) {
            return -1;
        }
        strcat(head, line);
    }

    // 응답이 끝나면 연결을 닫도록 요청하고, 빈 줄로 헤더를 끝낸다
    const char *last_lines = "Connection: close\r\n\r\n";
    if (strlen(head) + strlen(last_lines) >= head_size) {
        return -1;
    }
    strcat(head, last_lines);
    return 0;
}

// 백엔드에게 요청을 보낸다 (머리 + 본문). 성공 0, 실패 -1
static int send_request(int upstreamfd, const http_request *req, const char *target) {
    char head[REQUEST_HEAD_SIZE];
    if (build_request_head(head, sizeof(head), req, target) < 0) {
        return -1;
    }

    if (rio_writen(upstreamfd, head, strlen(head)) < 0) {
        return -1;
    }
    if (req->body != NULL) {
        if (rio_writen(upstreamfd, req->body, req->content_length) < 0) {
            return -1;
        }
    }
    return 0;
}

// 백엔드의 응답을 끝까지 읽어서 클라이언트에게 그대로 전달한다
// 성공 0, 응답이 시작되기 전에 문제가 생기면 502/504, 전달 도중 끊기면 -1
static int copy_response(int clientfd, int upstreamfd) {
    char buffer[8192];
    int got_response = 0;           // 응답을 한 바이트라도 받았는지

    while (1) {
        ssize_t count = read(upstreamfd, buffer, sizeof(buffer));

        if (count < 0) {
            if (errno == EINTR) {
                continue;           // 시그널 때문에 끊겼으면 다시 읽기
            }
            if (got_response) {
                return -1;          // 이미 일부를 보냈으니 오류 응답을 줄 수 없다
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 504;         // 시간 안에 응답이 오지 않음
            }
            return 502;
        }

        if (count == 0) {
            break;                  // 백엔드가 응답을 끝내고 연결을 닫음
        }

        got_response = 1;
        if (rio_writen(clientfd, buffer, count) < 0) {
            return -1;              // 클라이언트가 중간에 끊음
        }
    }

    if (!got_response) {
        return 502;                 // 아무 응답 없이 연결이 닫힘
    }
    return 0;
}

int proxy_relay(int clientfd, int upstreamfd, const http_request *req, const char *target) {
    // 1. 백엔드 응답을 기다리는 시간 제한 설정
    struct timeval timeout;
    timeout.tv_sec = PROXY_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(upstreamfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // 2. 요청 전달
    if (send_request(upstreamfd, req, target) < 0) {
        return 502;
    }

    // 3. 응답 전달
    return copy_response(clientfd, upstreamfd);
}

int reverse_proxy_request(int clientfd, const http_request *req, route *r) {
    // 백엔드마다 한 번씩만 시도한다. 연결이 안 되면 다음 백엔드로 넘어간다
    for (int i = 0; i < r->backend_count; i++) {
        const backend *b = next_backend(r);

        int backendfd = open_clientfd(b->host, b->port);
        if (backendfd < 0) {
            continue;               // 이 백엔드는 연결 실패 → 다음 백엔드
        }

        int result = proxy_relay(clientfd, backendfd, req, req->uri);
        close(backendfd);
        return result;
    }

    return 502;                     // 모든 백엔드에 연결하지 못함
}
