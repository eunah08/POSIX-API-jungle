// [test.h] 최소 단위 테스트 도구 — 외부 라이브러리 없이 CHECK 매크로로 검증
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int test_failures = 0;

// 조건이 거짓이면 파일:줄과 조건식을 출력하고 실패로 센다 (테스트는 계속 진행)
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        test_failures++; \
    } \
} while (0)

// 문자열 비교용
#define CHECK_STR(actual, expected) CHECK((actual) != NULL && strcmp((actual), (expected)) == 0)

// main의 마지막에 return TEST_RESULT(); — 실패가 있으면 1 (make test 중단)
#define TEST_RESULT() \
    (test_failures ? (fprintf(stderr, "  %d check(s) failed\n", test_failures), 1) : (printf("  ok\n"), 0))

// fd에서 EOF까지 전부 읽어 buf에 문자열로 담는다 (서버가 소켓으로 보낸 응답을 확인할 때 사용). 읽은 바이트 수를 돌려준다
// 보내는 쪽 fd를 먼저 close해야 EOF가 와서 끝난다
static inline int read_all(int fd, char *buf, size_t size) {
    size_t total = 0;

    while (total + 1 < size) {
        ssize_t count = read(fd, buf + total, size - 1 - total);
        if (count <= 0) {
            break;
        }
        total = total + (size_t)count;
    }
    buf[total] = '\0';
    return (int)total;
}

#endif
