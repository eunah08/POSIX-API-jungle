// [test_http_request.c] HTTP 파서 단위 테스트 — parse_request_line / parse_header_line은 문자열로,
// read_request는 socketpair(소켓 한 쌍)에 요청 문자열을 써 넣어 검증
#include "test.h"
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "http_request.h"

// 정상: "GET /index.html?a=1 HTTP/1.1\r\n" → method, path "/index.html", query "a=1", keep_alive 1
static void test_request_line_ok(void) {
    http_request req;
    memset(&req, 0, sizeof(req));
    CHECK(parse_request_line("GET /index.html?a=1 HTTP/1.1\r\n", &req) == 0);
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.uri, "/index.html?a=1");
    CHECK_STR(req.path, "/index.html");
    CHECK_STR(req.query, "a=1");
    CHECK_STR(req.version, "HTTP/1.1");
    CHECK(req.keep_alive == 1);

    // 절대 URI (Forward Proxy): path/query는 호스트 뒤 기준
    memset(&req, 0, sizeof(req));
    CHECK(parse_request_line("GET http://localhost:8001/a.html?x=1 HTTP/1.1\r\n", &req) == 0);
    CHECK_STR(req.uri, "http://localhost:8001/a.html?x=1");
    CHECK_STR(req.path, "/a.html");
    CHECK_STR(req.query, "x=1");
}

// 정상: POST, HTTP/1.0 (keep_alive 기본 0)
static void test_request_line_post_http10(void) {
    http_request req;
    memset(&req, 0, sizeof(req));
    CHECK(parse_request_line("POST /cgi-bin/adder HTTP/1.0\r\n", &req) == 0);
    CHECK_STR(req.method, "POST");
    CHECK_STR(req.query, "");
    CHECK(req.keep_alive == 0);
}

// 오류: 토큰 개수 틀림 → 400, 지원 안 하는 메서드 → 501, HTTP/2.0 → 505, 너무 긴 URI → 414
static void test_request_line_errors(void) {
    http_request req;
    memset(&req, 0, sizeof(req));
    CHECK(parse_request_line("BAD\r\n", &req) == 400);
    CHECK(parse_request_line("GET /a\r\n", &req) == 400);
    CHECK(parse_request_line("GET /a HTTP/1.1 extra\r\n", &req) == 400);
    CHECK(parse_request_line("DELETE /a HTTP/1.1\r\n", &req) == 501);
    CHECK(parse_request_line("GET /a HTTP/2.0\r\n", &req) == 505);
    CHECK(parse_request_line("GET noslash HTTP/1.1\r\n", &req) == 400);

    char line[MAX_URI_LEN + 64];
    memset(line, 0, sizeof(line));
    strcpy(line, "GET /");
    memset(line + 5, 'a', MAX_URI_LEN);                  // URI가 MAX_URI_LEN을 넘는다
    strcat(line, " HTTP/1.1\r\n");
    CHECK(parse_request_line(line, &req) == 414);
}

// 정상: "Host: localhost\r\n" 저장, get_header("host") 대소문자 무시
static void test_header_ok(void) {
    http_request req;
    memset(&req, 0, sizeof(req));
    CHECK(parse_header_line("Host:   localhost  \r\n", &req) == 0);
    CHECK(req.header_count == 1);
    CHECK_STR(get_header(&req, "host"), "localhost");
    CHECK_STR(get_header(&req, "HOST"), "localhost");
    CHECK(get_header(&req, "X-None") == NULL);
}

// Content-Length: 숫자 → content_length, 숫자 아님 → 400, MAX_BODY_LEN 초과 → 413
static void test_header_content_length(void) {
    http_request req;
    memset(&req, 0, sizeof(req));
    CHECK(parse_header_line("Content-Length: 42\r\n", &req) == 0);
    CHECK(req.content_length == 42);
    CHECK(parse_header_line("Content-Length: abc\r\n", &req) == 400);
    CHECK(parse_header_line("Content-Length: -5\r\n", &req) == 400);
    CHECK(parse_header_line("Content-Length: 1048577\r\n", &req) == 413);
    CHECK(parse_header_line("Content-Length: 99999999999999999999\r\n", &req) == 413);
}

// Connection: close → keep_alive 0, keep-alive → 1
static void test_header_connection(void) {
    http_request req;
    memset(&req, 0, sizeof(req));
    req.keep_alive = 1;
    CHECK(parse_header_line("Connection: close\r\n", &req) == 0);
    CHECK(req.keep_alive == 0);
    CHECK(parse_header_line("Connection: Keep-Alive\r\n", &req) == 0);
    CHECK(req.keep_alive == 1);
}

// 오류: ':' 없음 → 400, 헤더 MAX_HEADERS 초과 → 431
static void test_header_errors(void) {
    http_request req;
    memset(&req, 0, sizeof(req));
    CHECK(parse_header_line("no colon here\r\n", &req) == 400);
    for (int i = 0; i < MAX_HEADERS; i++) {
        CHECK(parse_header_line("X-A: 1\r\n", &req) == 0);
    }
    CHECK(parse_header_line("X-A: 1\r\n", &req) == 431);
}

// 소켓으로 요청 문자열 전체를 보내고 read_request 결과를 확인한다. 반환값 = read_request의 반환값
static int read_from_string(const char *text, http_request *req) {
    int sv[2];
    int rc = -2;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -2;
    if (write(sv[1], text, strlen(text)) < 0) { close(sv[0]); close(sv[1]); return -2; }
    close(sv[1]);                                        // 보낼 것을 다 보냈으니 닫아 EOF를 만든다

    rio_t rio;
    rio_readinitb(&rio, sv[0]);
    rc = read_request(&rio, req);
    close(sv[0]);
    return rc;
}

// read_request: 헤더 + 본문 끝까지 읽기, 그리고 규칙 위반 응답 코드
static void test_read_request(void) {
    http_request req;

    CHECK(read_from_string("POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\na=1&b=2", &req) == 0);
    CHECK(req.content_length == 7);
    CHECK(req.body != NULL && memcmp(req.body, "a=1&b=2", 7) == 0);
    free_request(&req);
    CHECK(req.body == NULL);

    CHECK(read_from_string("GET / HTTP/1.1\r\n\r\n", &req) == 400);                         // Host 없음
    CHECK(read_from_string("GET / HTTP/1.0\r\n\r\n", &req) == 0);                            // 1.0은 Host 불필요
    CHECK(read_from_string("POST / HTTP/1.1\r\nHost: x\r\n\r\n", &req) == 411);              // 본문 길이 없음
    CHECK(read_from_string("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", &req) == 501);
    CHECK(read_from_string("BAD\r\n\r\n", &req) == 400);
    CHECK(read_from_string("", &req) == -1);                                                 // 바로 EOF
    CHECK(read_from_string("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc", &req) == -1);   // 본문 도중 끊김
}

int main(void) {
    test_request_line_ok();
    test_request_line_post_http10();
    test_request_line_errors();
    test_header_ok();
    test_header_content_length();
    test_header_connection();
    test_header_errors();
    test_read_request();
    return TEST_RESULT();
}
