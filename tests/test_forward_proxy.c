// [test_forward_proxy.c] Forward Proxy 절대 URI 파싱 단위 테스트
#include "test.h"
#include "forward_proxy.h"

// "http://..." / "HTTP://..." → 1, "/index.html", "https://..." → 0
static void test_is_absolute_uri(void) {
    CHECK(is_absolute_uri("http://example.com/") == 1);
    CHECK(is_absolute_uri("HTTP://example.com/") == 1);
    CHECK(is_absolute_uri("/index.html") == 0);
    CHECK(is_absolute_uri("https://example.com/") == 0);
}

// "http://example.com/a.html" → host "example.com", port "80", path "/a.html"
static void test_parse_default_port(void) {
    char host[64], port[8], path[64];
    CHECK(parse_absolute_uri("http://example.com/a.html", host, sizeof(host), port, sizeof(port), path, sizeof(path)) == 0);
    CHECK_STR(host, "example.com");
    CHECK_STR(port, "80");
    CHECK_STR(path, "/a.html");
}

// "http://localhost:8001/api?x=1" → port "8001", path "/api?x=1" (쿼리 유지)
static void test_parse_port_and_query(void) {
    char host[64], port[8], path[64];
    CHECK(parse_absolute_uri("http://localhost:8001/api?x=1", host, sizeof(host), port, sizeof(port), path, sizeof(path)) == 0);
    CHECK_STR(host, "localhost");
    CHECK_STR(port, "8001");
    CHECK_STR(path, "/api?x=1");
}

// "http://example.com" (경로 없음) → path "/"
static void test_parse_no_path(void) {
    char host[64], port[8], path[64];
    CHECK(parse_absolute_uri("http://example.com", host, sizeof(host), port, sizeof(port), path, sizeof(path)) == 0);
    CHECK_STR(path, "/");
    CHECK(parse_absolute_uri("http://example.com?x=1", host, sizeof(host), port, sizeof(port), path, sizeof(path)) == 0);
    CHECK_STR(path, "/?x=1");
}

// 오류: "http:///a" (호스트 없음), "http://h:99999/" (포트 범위 밖), 버퍼보다 긴 호스트 → -1
static void test_parse_errors(void) {
    char host[8], port[8], path[16];
    CHECK(parse_absolute_uri("http:///a", host, sizeof(host), port, sizeof(port), path, sizeof(path)) == -1);
    CHECK(parse_absolute_uri("http://h:99999/", host, sizeof(host), port, sizeof(port), path, sizeof(path)) == -1);
    CHECK(parse_absolute_uri("http://h:0/", host, sizeof(host), port, sizeof(port), path, sizeof(path)) == -1);
    CHECK(parse_absolute_uri("http://h:/", host, sizeof(host), port, sizeof(port), path, sizeof(path)) == -1);
    CHECK(parse_absolute_uri("http://averylonghostname/", host, sizeof(host), port, sizeof(port), path, sizeof(path)) == -1);
    CHECK(parse_absolute_uri("http://h/averyveryverylongpath", host, sizeof(host), port, sizeof(port), path, sizeof(path)) == -1);
    CHECK(parse_absolute_uri("/a.html", host, sizeof(host), port, sizeof(port), path, sizeof(path)) == -1);
}

int main(void) {
    test_is_absolute_uri();
    test_parse_default_port();
    test_parse_port_and_query();
    test_parse_no_path();
    test_parse_errors();
    return TEST_RESULT();
}
