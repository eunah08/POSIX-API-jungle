// [net.c] 소켓 연결 도우미 구현
#include "net.h"
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define CONNECT_TIMEOUT_SEC 5   // 연결이 안 되는 호스트 때문에 워커가 오래 묶이지 않게 한다

// CGI 같은 자식 프로세스가 exec할 때 이 소켓을 물려받지 않도록 표시한다
static void set_close_on_exec(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

// 서버용 듣기 소켓을 만든다. 성공하면 fd, 실패하면 -1
int open_listenfd(const char *port) {
    struct addrinfo hints;
    struct addrinfo *candidates;    // getaddrinfo가 찾아 준 주소 후보 목록
    struct addrinfo *candidate;
    int listenfd = -1;

    // 1. 어떤 주소를 원하는지 알려 준다 (TCP, 서버용, 포트는 숫자)
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG | AI_NUMERICSERV;
    if (getaddrinfo(NULL, port, &hints, &candidates) != 0) {
        return -1;
    }

    // 2. 후보를 하나씩 시도해서 bind에 성공하는 소켓을 찾는다
    for (candidate = candidates; candidate != NULL; candidate = candidate->ai_next) {
        listenfd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (listenfd < 0) {
            continue;
        }
        set_close_on_exec(listenfd);

        // 서버를 껐다 바로 켜도 "Address already in use"가 안 나게 한다
        int yes = 1;
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(listenfd, candidate->ai_addr, candidate->ai_addrlen) == 0) {
            break;                  // 성공
        }
        close(listenfd);
        listenfd = -1;
    }
    freeaddrinfo(candidates);

    if (listenfd < 0) {
        return -1;
    }

    // 3. 연결 요청을 받을 수 있는 상태로 바꾼다
    if (listen(listenfd, 1024) < 0) {
        close(listenfd);
        return -1;
    }
    return listenfd;
}

// host:port 서버에 연결된 소켓을 만든다 (프록시가 백엔드나 원 서버에 붙을 때 사용)
// 성공하면 fd, 실패하면 -1. 실패해도 프로그램을 끝내지 않는다 (호출한 쪽이 502를 보낸다)
int open_clientfd(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *candidates;
    struct addrinfo *candidate;
    int clientfd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
    if (getaddrinfo(host, port, &hints, &candidates) != 0) {
        return -1;                  // 호스트 이름을 못 찾음 (DNS 실패)
    }

    for (candidate = candidates; candidate != NULL; candidate = candidate->ai_next) {
        clientfd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (clientfd < 0) {
            continue;
        }
        set_close_on_exec(clientfd);

        // connect가 무한정 기다리지 않도록 시간 제한을 둔다
        struct timeval timeout;
        timeout.tv_sec = CONNECT_TIMEOUT_SEC;
        timeout.tv_usec = 0;
        setsockopt(clientfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (connect(clientfd, candidate->ai_addr, candidate->ai_addrlen) == 0) {
            break;                  // 연결 성공
        }
        close(clientfd);
        clientfd = -1;
    }
    freeaddrinfo(candidates);
    return clientfd;
}
