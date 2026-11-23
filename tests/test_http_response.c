// [test_http_response.c] 응답 생성 단위 테스트 — 소켓으로 보낸 응답 문자열을 확인
#include "test.h"
#include <sys/socket.h>
#include "http_response.h"

static void test_status_text(void) {
    CHECK_STR(status_text(200), "OK");
    CHECK_STR(status_text(404), "Not Found");
    CHECK_STR(status_text(502), "Bad Gateway");
    CHECK_STR(status_text(504), "Gateway Timeout");
    CHECK_STR(status_text(999), "Unknown");
}

// send_response: 상태 라인, 헤더, 빈 줄, 본문 순서로 나간다
static void test_send_response(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CHECK(send_response(sv[1], 200, "text/plain", "hello", 5, 1) == 0);
    close(sv[1]);

    char response[1024];
    read_all(sv[0], response, sizeof(response));
    close(sv[0]);

    CHECK(strncmp(response, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(response, "Content-Type: text/plain\r\n") != NULL);
    CHECK(strstr(response, "Content-Length: 5\r\n") != NULL);
    CHECK(strstr(response, "Connection: keep-alive\r\n") != NULL);
    CHECK(strstr(response, "\r\n\r\nhello") != NULL);
}

// keep_alive가 0이면 Connection: close
static void test_connection_close(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CHECK(send_headers(sv[1], 200, "text/html", 0, 0) == 0);
    close(sv[1]);

    char response[1024];
    read_all(sv[0], response, sizeof(response));
    close(sv[0]);
    CHECK(strstr(response, "Connection: close\r\n") != NULL);
}

// send_error: 상태 코드가 상태 라인과 본문에 들어간다
static void test_send_error(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CHECK(send_error(sv[1], 404, 0) == 0);
    close(sv[1]);

    char response[1024];
    read_all(sv[0], response, sizeof(response));
    close(sv[0]);

    CHECK(strncmp(response, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(strstr(response, "<h1>404 Not Found</h1>") != NULL);
}

int main(void) {
    test_status_text();
    test_send_response();
    test_connection_close();
    test_send_error();
    return TEST_RESULT();
}
