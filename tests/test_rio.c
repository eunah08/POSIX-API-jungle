// [test_rio.c] Robust I/O 단위 테스트 — socketpair(서로 이어진 소켓 한 쌍)로 읽기·쓰기 확인
#include "test.h"
#include <sys/socket.h>
#include "rio.h"

// 소켓 한 쌍을 만든다. sv[1]에 쓰면 sv[0]에서 읽힌다
static void make_pair(int sv[2]) {
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
}

// 한 줄씩 읽기: '\n'까지 읽고, 줄바꿈도 결과에 포함된다
static void test_readlineb(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[1], "line1\r\nline2\nlast", 17);
    close(sv[1]);

    rio_t rio;
    rio_readinitb(&rio, sv[0]);
    char line[64];

    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 7);
    CHECK_STR(line, "line1\r\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 6);
    CHECK_STR(line, "line2\n");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 4);    // 줄바꿈 없이 EOF로 끝난 마지막 줄
    CHECK_STR(line, "last");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 0);    // 더 읽을 게 없음 (EOF)
    close(sv[0]);
}

// maxlen보다 긴 줄은 maxlen-1 글자까지만 읽고 나머지는 다음 호출에서 이어 읽는다
static void test_readlineb_too_long(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[1], "abcdefg\n", 8);
    close(sv[1]);

    rio_t rio;
    rio_readinitb(&rio, sv[0]);
    char line[5];

    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 4);
    CHECK_STR(line, "abcd");
    CHECK(rio_readlineb(&rio, line, sizeof(line)) == 4);
    CHECK_STR(line, "efg\n");
    close(sv[0]);
}

// 줄을 읽은 다음 같은 버퍼에서 이어서 정확한 바이트 수만큼 읽기 (헤더 뒤의 본문 읽기와 같은 상황)
static void test_readnb(void) {
    int sv[2];
    make_pair(sv);
    rio_writen(sv[1], "head\nbody12345", 14);
    close(sv[1]);

    rio_t rio;
    rio_readinitb(&rio, sv[0]);
    char line[16];
    char data[16];

    rio_readlineb(&rio, line, sizeof(line));
    CHECK(rio_readnb(&rio, data, 4) == 4);
    CHECK(memcmp(data, "body", 4) == 0);
    CHECK(rio_readnb(&rio, data, 10) == 5);                 // 5바이트만 남아 있어서 n보다 적게 읽힘
    CHECK(memcmp(data, "12345", 5) == 0);
    close(sv[0]);
}

// rio_writen은 요청한 바이트를 전부 보낸다
static void test_writen(void) {
    int sv[2];
    make_pair(sv);

    char data[10000];
    memset(data, 'x', sizeof(data));
    CHECK(rio_writen(sv[1], data, sizeof(data)) == (ssize_t)sizeof(data));
    close(sv[1]);

    rio_t rio;
    rio_readinitb(&rio, sv[0]);
    char received[10000];
    CHECK(rio_readnb(&rio, received, sizeof(received)) == (ssize_t)sizeof(received));
    CHECK(memcmp(data, received, sizeof(data)) == 0);
    close(sv[0]);
}

int main(void) {
    test_readlineb();
    test_readlineb_too_long();
    test_readnb();
    test_writen();
    return TEST_RESULT();
}
