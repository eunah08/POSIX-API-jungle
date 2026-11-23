// [main.c] 진입점 — 인자 파싱 → 시그널 설정 → 서버 실행
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "server.h"

// 시그널 핸들러: 플래그만 세운다 (실제 종료는 accept 루프가 확인)
static void on_signal(int sig) {
    (void)sig;
    request_stop();
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-p port] [-r root] [-g cgi_dir] [-c routes.conf] [-f] [-t threads] [-q queue_size]\n",
            prog);
}

int main(int argc, char **argv) {
    server_config cfg = {
        .port = "8080",
        .root = "./www",
        .cgi_dir = "./cgi-bin",
        .routes_path = NULL,
        .forward_enabled = 0,
        .threads = 8,
        .queue_size = 64,
    };

    int opt;
    while ((opt = getopt(argc, argv, "p:r:g:c:ft:q:")) != -1) {
        switch (opt) {
        case 'p': cfg.port = optarg; break;
        case 'r': cfg.root = optarg; break;
        case 'g': cfg.cgi_dir = optarg; break;
        case 'c': cfg.routes_path = optarg; break;
        case 'f': cfg.forward_enabled = 1; break;
        case 't': cfg.threads = atoi(optarg); break;
        case 'q': cfg.queue_size = atoi(optarg); break;
        default: usage(argv[0]); return 1;
        }
    }

    // 인자 검사: 포트는 1~65535 (htons는 큰 값을 조용히 잘라 버리므로 직접 확인), threads·queue_size는 양수
    char *end;
    long port = strtol(cfg.port, &end, 10);
    if (*cfg.port == '\0' || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", cfg.port);
        return 1;
    }
    if (cfg.threads <= 0 || cfg.queue_size <= 0) {
        fprintf(stderr, "-t and -q must be positive\n");
        return 1;
    }

    // 시그널 설정
    //   - SIGPIPE 무시: 끊긴 소켓에 write하면 프로세스가 죽는 것을 막고 write가 -1(EPIPE)을 반환하게 함
    //   - SIGINT/SIGTERM: on_signal 등록. SA_RESTART를 빼야 accept가 EINTR로 깨어남
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (run_server(&cfg) != 0) {
        fprintf(stderr, "server failed to start\n");
        return 1;
    }
    return 0;
}
