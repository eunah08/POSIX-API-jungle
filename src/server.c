// [server.c] 서버 본체 구현
#include "server.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "cgi.h"
#include "forward_proxy.h"
#include "http_request.h"
#include "http_response.h"
#include "net.h"
#include "proxy.h"
#include "router.h"
#include "static_file.h"
#include "threadpool.h"

static const server_config *g_cfg;            // run_server가 설정, 워커들은 읽기만 함
static route_table g_routes;                  // 시작 시 한 번 로드, 이후 읽기 + next 카운터(atomic)만 변경
static volatile sig_atomic_t g_stop = 0;      // 종료 요청 표시 (시그널 핸들러에서 안전하게 쓸 수 있는 타입)

// 소켓에서 seconds초 동안 데이터가 안 오면 read가 오류(EAGAIN)로 끝나도록 설정한다
static void set_recv_timeout(int fd, int seconds) {
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

// "on" / "off" 글자로 바꾼다 (시작 메시지 출력용)
static const char *on_off(int enabled) {
    if (enabled) {
        return "on";
    }
    return "off";
}

int run_server(const server_config *cfg) {
    g_cfg = cfg;

    // 1. Reverse Proxy 규칙 파일이 지정됐다면 읽어 둔다
    if (cfg->routes_path != NULL) {
        if (load_routes(cfg->routes_path, &g_routes) != 0) {
            return -1;
        }
    }

    // 2. 듣기 소켓 열기
    int listenfd = open_listenfd(cfg->port);
    if (listenfd < 0) {
        fprintf(stderr, "포트 %s를 열 수 없습니다\n", cfg->port);
        return -1;
    }
    // accept가 1초마다 깨어나서 종료 요청(g_stop)을 확인할 수 있게 시간 제한을 둔다
    set_recv_timeout(listenfd, 1);

    // 3. 스레드 풀 만들기
    threadpool *pool = threadpool_create(cfg->threads, cfg->queue_size, handle_connection);
    if (pool == NULL) {
        fprintf(stderr, "스레드 풀을 만들 수 없습니다\n");
        close(listenfd);
        return -1;
    }

    printf("listening on port %s (threads=%d, queue=%d, reverse=%s, forward=%s)\n",
           cfg->port, cfg->threads, cfg->queue_size,
           on_off(g_routes.count > 0), on_off(cfg->forward_enabled));
    fflush(stdout);

    // 4. 연결을 받아서 스레드 풀의 큐에 넣는 일을 종료 요청이 올 때까지 반복한다
    while (!g_stop) {
        int connfd = accept(listenfd, NULL, NULL);

        if (connfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;           // 시그널이나 1초 시간 제한으로 깨어난 것 → 종료 요청부터 다시 확인
            }
            perror("accept");       // 그 외 오류(열린 파일이 너무 많음 등): 로그만 남기고 계속
            usleep(10000);          // 같은 오류가 쏟아져도 CPU를 계속 쓰지 않도록 잠깐 쉰다
            continue;
        }

        fcntl(connfd, F_SETFD, FD_CLOEXEC);     // CGI 자식 프로세스가 이 연결을 물려받지 않게 한다

        if (threadpool_submit(pool, connfd) != 0) {
            send_error(connfd, 503, 0);         // 큐가 가득 참: 지금은 바쁘다고 알리고 닫는다
            close(connfd);
        }
    }

    // 5. 종료: 새 연결을 받지 않고, 처리 중이던 연결은 마무리한다
    close(listenfd);
    threadpool_destroy(pool);
    return 0;
}

// 요청 하나를 알맞은 처리기로 보낸다. 반환값은 처리기 규칙과 같다 (0 = 응답 보냄, 양수 = 보낼 오류 코드, -1 = 연결 끊김)
// 프록시와 CGI는 응답 길이를 미리 모르므로 응답이 끝나면 연결을 닫아야 한다 → *must_close를 1로 알린다
static int dispatch(int connfd, const http_request *req, int *must_close) {
    // 1. 주소가 http://... 로 시작하면 Forward Proxy 요청
    if (is_absolute_uri(req->uri)) {
        *must_close = 1;
        if (!g_cfg->forward_enabled) {
            return 403;             // -f 옵션이 없으면 아무나 쓰는 프록시가 되지 않도록 거절
        }
        return forward_proxy_request(connfd, req);
    }

    // 2. 라우팅 규칙에 맞으면 Reverse Proxy
    route *matched = match_route(&g_routes, req->path);
    if (matched != NULL) {
        *must_close = 1;
        return reverse_proxy_request(connfd, req, matched);
    }

    // 3. /cgi-bin/ 으로 시작하면 CGI
    if (strncmp(req->path, CGI_PREFIX, strlen(CGI_PREFIX)) == 0) {
        *must_close = 1;
        return serve_cgi(connfd, g_cfg->cgi_dir, req);
    }

    // 4. 나머지는 정적 파일
    return serve_static(connfd, g_cfg->root, req);
}

void handle_connection(int connfd) {
    rio_t rio;
    rio_readinitb(&rio, connfd);

    // 요청이 한동안 안 오면 read가 시간 초과로 끝나서 유휴 연결이 워커를 붙잡고 있지 못한다
    set_recv_timeout(connfd, KEEPALIVE_TIMEOUT_SEC);

    // keep-alive: 같은 연결로 요청이 계속 오는 동안 반복한다
    while (1) {
        http_request req;

        // 요청 읽기
        int result = read_request(&rio, &req);
        if (result < 0) {
            break;                  // 연결 끊김 또는 시간 초과
        }
        if (result > 0) {
            send_error(connfd, result, 0);      // 잘못된 요청: 오류 응답을 보내고 연결을 닫는다
            free_request(&req);
            break;
        }

        // 요청 처리
        int must_close = 0;
        result = dispatch(connfd, &req, &must_close);

        // 이 연결을 계속 쓸지: 클라이언트가 원하고, 처리기가 닫으라고 하지 않았을 때만
        int keep_open = 0;
        if (req.keep_alive && !must_close) {
            keep_open = 1;
        }

        // 처리기가 아직 응답을 안 보내고 오류 코드만 돌려줬다면 여기서 한 번에 보낸다
        if (result > 0) {
            result = send_error(connfd, result, keep_open);
        }
        free_request(&req);

        if (result < 0 || !keep_open) {
            break;
        }
    }

    close(connfd);                  // 빠뜨리면 fd가 계속 쌓인다
}

void request_stop(void) {
    g_stop = 1;
}
