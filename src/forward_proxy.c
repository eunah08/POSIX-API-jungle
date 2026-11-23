// [forward_proxy.c] Forward Proxy 구현
#include "forward_proxy.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "net.h"
#include "proxy.h"

// "http://"로 시작하면 1
int is_absolute_uri(const char *uri) {
    return strncasecmp(uri, "http://", 7) == 0;
}

// 포트 번호 문자열이 1~65535 사이의 숫자이면 1
static int is_valid_port(const char *text) {
    if (text[0] == '\0') {
        return 0;
    }
    for (int i = 0; text[i] != '\0'; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return 0;
        }
    }
    if (strlen(text) > 5) {
        return 0;
    }

    int number = atoi(text);
    if (number < 1 || number > 65535) {
        return 0;
    }
    return 1;
}

// 예) "http://example.com:8000/a.html?x=1"
//      → host "example.com", port "8000", path "/a.html?x=1"
int parse_absolute_uri(const char *uri, char *host, size_t hostlen,
                       char *port, size_t portlen, char *path, size_t pathlen) {
    if (!is_absolute_uri(uri)) {
        return -1;
    }

    // 1. "http://" 뒤에서 첫 '/' 또는 '?' 직전까지가 주소 부분 ("example.com:8000")
    const char *address = uri + 7;
    int address_length = 0;
    while (address[address_length] != '/' && address[address_length] != '?' && address[address_length] != '\0') {
        address_length++;
    }
    const char *rest = address + address_length;    // 경로가 시작되는 위치 ("/a.html?x=1")

    // 2. 주소 부분에서 ':'를 찾는다 ('@'가 있는 user:pass@host 형식은 지원하지 않음)
    int colon_position = -1;
    for (int i = 0; i < address_length; i++) {
        if (address[i] == '@') {
            return -1;
        }
        if (address[i] == ':' && colon_position < 0) {
            colon_position = i;
        }
    }

    // 3. 호스트와 포트로 나눈다
    int host_length = address_length;
    char port_text[16] = "80";                      // 포트를 안 적으면 80
    if (colon_position >= 0) {
        host_length = colon_position;
        int port_length = address_length - colon_position - 1;
        if (port_length <= 0 || port_length >= (int)sizeof(port_text)) {
            return -1;
        }
        memcpy(port_text, address + colon_position + 1, port_length);
        port_text[port_length] = '\0';
    }

    if (host_length == 0 || host_length >= (int)hostlen) {
        return -1;                  // 호스트가 비었거나 버퍼보다 김
    }
    if (!is_valid_port(port_text) || strlen(port_text) >= portlen) {
        return -1;
    }

    // 4. 경로: "/"로 시작하면 그대로, 아니면 "/"를 앞에 붙인다 ("http://host" → "/", "http://host?x=1" → "/?x=1")
    int needed;
    if (rest[0] == '/') {
        needed = snprintf(path, pathlen, "%s", rest);
    } else {
        needed = snprintf(path, pathlen, "/%s", rest);
    }
    if (needed < 0 || needed >= (int)pathlen) {
        return -1;                  // 경로가 버퍼보다 김
    }

    // 5. 결과 저장
    memcpy(host, address, host_length);
    host[host_length] = '\0';
    strcpy(port, port_text);
    return 0;
}

int forward_proxy_request(int clientfd, const http_request *req) {
    char host[256];
    char port[8];
    char path[MAX_URI_LEN];

    // 1. 요청 URI를 호스트 / 포트 / 경로로 나눈다
    if (parse_absolute_uri(req->uri, host, sizeof(host), port, sizeof(port), path, sizeof(path)) != 0) {
        return 400;
    }

    // 2. 원 서버에 연결한다 (호스트 이름을 못 찾거나 연결이 안 되면 502)
    int serverfd = open_clientfd(host, port);
    if (serverfd < 0) {
        return 502;
    }

    // 3. 요청을 전달하고 응답을 클라이언트에게 돌려준다 (요청 라인에는 경로만 넣는다)
    int result = proxy_relay(clientfd, serverfd, req, path);
    close(serverfd);
    return result;
}
