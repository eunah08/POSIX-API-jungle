// [rio.c] Robust I/O 구현 — 네트워크 read/write는 쪼개져서 오가므로 원하는 만큼 될 때까지 반복한다
#include "rio.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

// fd로 n바이트를 전부 보낸다. 한 번의 write가 일부만 보낼 수 있어서 다 보낼 때까지 반복한다
ssize_t rio_writen(int fd, const void *buf, size_t n) {
    const char *position = buf;     // 아직 안 보낸 데이터가 시작되는 위치
    size_t remaining = n;           // 아직 못 보낸 바이트 수

    while (remaining > 0) {
        ssize_t written = write(fd, position, remaining);

        if (written < 0) {
            if (errno == EINTR) {   // 시그널 때문에 잠깐 끊긴 것이므로 다시 시도
                continue;
            }
            return -1;              // 진짜 오류 (연결 끊김 등)
        }

        position = position + written;
        remaining = remaining - written;
    }
    return n;
}

// rio_t를 처음 상태로 만든다 (내부 버퍼는 비어 있음)
void rio_readinitb(rio_t *rp, int fd) {
    rp->fd = fd;
    rp->cnt = 0;
    rp->bufp = rp->buf;
}

// 내부 버퍼에서 최대 n바이트를 dst로 꺼낸다. 버퍼가 비어 있으면 소켓에서 한꺼번에 읽어 채운다
// 꺼낸 바이트 수를 돌려준다. EOF면 0, 오류(타임아웃 포함)면 -1
static ssize_t rio_read(rio_t *rp, char *dst, size_t n) {
    // 버퍼가 비어 있는 동안 소켓에서 읽어 채운다
    while (rp->cnt <= 0) {
        ssize_t got = read(rp->fd, rp->buf, sizeof(rp->buf));

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (got == 0) {
            return 0;               // 상대가 연결을 닫음
        }
        rp->cnt = got;
        rp->bufp = rp->buf;
    }

    // 버퍼에 있는 만큼, 최대 n바이트까지만 꺼낸다
    size_t take = n;
    if (rp->cnt < (ssize_t)n) {
        take = rp->cnt;
    }
    memcpy(dst, rp->bufp, take);
    rp->bufp = rp->bufp + take;
    rp->cnt = rp->cnt - take;
    return take;
}

// 한 줄('\n'까지)을 읽는다. 1바이트씩 꺼내서 '\n'이 나오면 멈춘다
ssize_t rio_readlineb(rio_t *rp, void *buf, size_t maxlen) {
    char *line = buf;
    size_t count = 0;               // 지금까지 line에 담은 글자 수

    while (count + 1 < maxlen) {    // 마지막 한 칸은 끝 표시 '\0' 자리
        char c;
        ssize_t result = rio_read(rp, &c, 1);

        if (result < 0) {
            return -1;
        }
        if (result == 0) {          // EOF
            break;
        }

        line[count] = c;
        count++;
        if (c == '\n') {
            break;
        }
    }

    line[count] = '\0';
    return count;                   // 아무것도 못 읽고 EOF면 0
}

// 정확히 n바이트를 읽는다 (본문 읽기용). 연결이 먼저 끊기면 n보다 적게 읽힌다
ssize_t rio_readnb(rio_t *rp, void *buf, size_t n) {
    char *position = buf;
    size_t remaining = n;

    while (remaining > 0) {
        ssize_t result = rio_read(rp, position, remaining);

        if (result < 0) {
            return -1;
        }
        if (result == 0) {          // EOF
            break;
        }

        position = position + result;
        remaining = remaining - result;
    }
    return n - remaining;
}
