// [test_router.c] Reverse Proxy 라우팅 단위 테스트
#include "test.h"
#include "router.h"

// "/api/ 127.0.0.1:8001 127.0.0.1:8002" → prefix, 백엔드 2개, host/port 분리
static void test_parse_route_line(void) {
    route r;
    CHECK(parse_route_line("/api/ 127.0.0.1:8001 127.0.0.1:8002\n", &r) == 1);
    CHECK_STR(r.prefix, "/api/");
    CHECK(r.backend_count == 2);
    CHECK_STR(r.backends[0].host, "127.0.0.1");
    CHECK_STR(r.backends[0].port, "8001");
    CHECK_STR(r.backends[1].port, "8002");
}

// 빈 줄·주석 → 0, 백엔드 없음·':' 없음·포트 범위 밖 → -1
static void test_parse_route_line_errors(void) {
    route r;
    CHECK(parse_route_line("\n", &r) == 0);
    CHECK(parse_route_line("# comment\n", &r) == 0);
    CHECK(parse_route_line("/api/\n", &r) == -1);                    // 백엔드 없음
    CHECK(parse_route_line("/api/ localhost\n", &r) == -1);          // ':' 없음
    CHECK(parse_route_line("/api/ localhost:99999\n", &r) == -1);    // 포트 범위 밖
    CHECK(parse_route_line("/api/ localhost:0\n", &r) == -1);
    CHECK(parse_route_line("/api/ localhost:80a\n", &r) == -1);      // 숫자가 아님
    CHECK(parse_route_line("api/ localhost:80\n", &r) == -1);        // prefix가 '/'로 시작하지 않음
}

// "/api/" 와 "/api/v2/" 규칙이 있을 때 "/api/v2/x" → 긴 쪽, 맞는 규칙 없음 → NULL
static void test_match_longest_prefix(void) {
    route_table table;
    table.count = 0;
    CHECK(parse_route_line("/api/ a:1\n", &table.routes[0]) == 1);
    CHECK(parse_route_line("/api/v2/ b:2\n", &table.routes[1]) == 1);
    table.count = 2;

    route *r = match_route(&table, "/api/v2/x");
    CHECK(r != NULL && strcmp(r->prefix, "/api/v2/") == 0);
    r = match_route(&table, "/api/other");
    CHECK(r != NULL && strcmp(r->prefix, "/api/") == 0);
    CHECK(match_route(&table, "/index.html") == NULL);
}

// 백엔드 2개면 next_backend가 8001 → 8002 → 8001 순서
static void test_round_robin(void) {
    route r;
    CHECK(parse_route_line("/api/ h:8001 h:8002\n", &r) == 1);

    // CHECK_STR은 인자를 여러 번 평가하므로, 호출 결과를 변수에 먼저 받는다
    const backend *first = next_backend(&r);
    const backend *second = next_backend(&r);
    const backend *third = next_backend(&r);
    CHECK_STR(first->port, "8001");
    CHECK_STR(second->port, "8002");
    CHECK_STR(third->port, "8001");
}

int main(void) {
    test_parse_route_line();
    test_parse_route_line_errors();
    test_match_longest_prefix();
    test_round_robin();
    return TEST_RESULT();
}
