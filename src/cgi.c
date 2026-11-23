// [cgi.c] CGI 실행 구현
//
// CGI란: 요청이 올 때마다 별도의 프로그램(cgi-bin/adder 등)을 실행하고, 그 프로그램이 출력한 내용을 응답으로 보내는 방식
//
// 전체 흐름 (CSAPP 11.6 Tiny와 같은 방식)
//   1. 실행할 프로그램 파일을 찾는다 (없으면 404, 실행 못 하면 403)
//   2. 상태 라인("HTTP/1.1 200 OK")을 먼저 보낸다. 나머지 헤더와 본문은 CGI 프로그램이 직접 출력한다
//   3. fork로 자식 프로세스를 만든다. 자식은 dup2로 stdout을 클라이언트 소켓에 연결한 뒤 execve로 CGI 프로그램이 된다
//      → 프로그램이 printf로 출력하면 그 내용이 그대로 클라이언트에게 간다
//   4. 부모는 자식이 끝날 때까지 기다린다 (waitpid). 너무 오래 걸리면 강제 종료한다
#include "cgi.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "rio.h"

#define CGI_TIMEOUT_SEC 10                                           // CGI가 이 시간 안에 끝나지 않으면 강제 종료
#define CGI_STATUS_LINE "HTTP/1.1 200 OK\r\nConnection: close\r\n"   // 나머지 헤더는 CGI 프로그램이 직접 출력한다
#define EXEC_FAILED_CODE 127                                         // 자식이 execve에 실패했을 때의 종료 코드

// CGI 프로그램에게 전달할 환경변수 모음 (요청 정보를 이 방법으로 알려 준다)
typedef struct {
    char method[32];                // REQUEST_METHOD=GET
    char query[MAX_URI_LEN + 16];   // QUERY_STRING=a=1&b=2
    char length[48];                // CONTENT_LENGTH=7
    char protocol[32];              // SERVER_PROTOCOL=HTTP/1.1
    char type[MAX_HEADER_VALUE + 16];   // CONTENT_TYPE=...
    char *list[7];                  // execve에 넘길 목록 (끝은 NULL)
} cgi_env;

// 환경변수 목록을 채운다
static void fill_env(cgi_env *env, const http_request *req) {
    snprintf(env->method, sizeof(env->method), "REQUEST_METHOD=%s", req->method);
    snprintf(env->query, sizeof(env->query), "QUERY_STRING=%s", req->query);
    snprintf(env->length, sizeof(env->length), "CONTENT_LENGTH=%zu", req->content_length);
    snprintf(env->protocol, sizeof(env->protocol), "SERVER_PROTOCOL=%s", req->version);

    const char *content_type = get_header(req, "Content-Type");
    if (content_type == NULL) {
        content_type = "";
    }
    snprintf(env->type, sizeof(env->type), "CONTENT_TYPE=%s", content_type);

    env->list[0] = env->method;
    env->list[1] = env->query;
    env->list[2] = env->length;
    env->list[3] = env->protocol;
    env->list[4] = env->type;
    env->list[5] = "GATEWAY_INTERFACE=CGI/1.1";
    env->list[6] = NULL;
}

// CGI 프로그램이 입력(stdin)으로 읽을 fd를 만든다
//   POST 본문이 있으면: 임시 파일에 본문을 적고 처음부터 읽게 한다
//   없으면: /dev/null (읽으면 바로 끝)
// 실패하면 -1
static int open_cgi_input(const http_request *req) {
    if (req->body == NULL || req->content_length == 0) {
        return open("/dev/null", O_RDONLY);
    }

    FILE *temp = tmpfile();
    if (temp == NULL) {
        return -1;
    }
    int fd = dup(fileno(temp));
    fclose(temp);
    if (fd < 0) {
        return -1;
    }

    if (rio_writen(fd, req->body, req->content_length) < 0) {
        close(fd);
        return -1;
    }
    lseek(fd, 0, SEEK_SET);         // 읽기 위치를 파일 맨 앞으로
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

// 요청 경로에서 실행할 프로그램 경로를 만들고 확인한다
// 성공 0, 실패하면 보낼 오류 코드 (404, 403)
static int find_program(char *program, size_t size, const char *cgi_dir, const char *request_path) {
    size_t prefix_length = strlen(CGI_PREFIX);

    // "/cgi-bin/" 뒤에 프로그램 이름이 있어야 한다
    if (strncmp(request_path, CGI_PREFIX, prefix_length) != 0 || request_path[prefix_length] == '\0') {
        return 404;
    }
    // ".."가 있으면 다른 폴더의 프로그램을 실행하려는 시도일 수 있다
    if (strstr(request_path, "..") != NULL) {
        return 403;
    }

    int length = snprintf(program, size, "%s/%s", cgi_dir, request_path + prefix_length);
    if (length < 0 || length >= (int)size) {
        return 404;
    }

    struct stat info;
    if (stat(program, &info) < 0) {
        return 404;                 // 파일이 없음
    }
    if (!S_ISREG(info.st_mode)) {
        return 403;                 // 폴더 등
    }
    if (access(program, X_OK) != 0) {
        return 403;                 // 실행 권한이 없음
    }
    return 0;
}

// 자식 프로세스가 끝날 때까지 기다린다 (최대 CGI_TIMEOUT_SEC초)
// 정상 종료면 0, 실행 실패나 시간 초과면 -1
static int wait_for_child(pid_t pid) {
    int waited_ms = 0;

    while (1) {
        int status;
        pid_t finished = waitpid(pid, &status, WNOHANG);       // WNOHANG: 안 끝났으면 기다리지 않고 바로 돌아옴

        if (finished == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == EXEC_FAILED_CODE) {
                return -1;          // execve에 실패해서 끝난 것
            }
            return 0;
        }
        if (finished < 0 && errno != EINTR) {
            return -1;
        }

        if (waited_ms >= CGI_TIMEOUT_SEC * 1000) {              // 너무 오래 걸림: 강제 종료하고 회수
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            return -1;
        }
        usleep(2000);                                           // 2ms 쉬었다가 다시 확인
        waited_ms = waited_ms + 2;
    }
}

int serve_cgi(int fd, const char *cgi_dir, const http_request *req) {
    // 1. 실행할 프로그램 확인
    char program[1024];
    int error = find_program(program, sizeof(program), cgi_dir, req->path);
    if (error != 0) {
        return error;
    }

    // 2. 자식에게 줄 환경변수와 입력(stdin) 준비 (fork 전에 모두 만들어 둔다)
    cgi_env env;
    fill_env(&env, req);

    int input_fd = open_cgi_input(req);
    if (input_fd < 0) {
        return 500;
    }

    // 3. 상태 라인을 먼저 보낸다. 나머지 헤더와 본문은 CGI 프로그램이 출력한다
    if (rio_writen(fd, CGI_STATUS_LINE, strlen(CGI_STATUS_LINE)) < 0) {
        close(input_fd);
        return -1;
    }

    // 4. fork: 이 줄 이후 부모와 자식이 각각 아래 코드를 실행한다
    pid_t pid = fork();
    if (pid < 0) {
        close(input_fd);
        return -1;                  // 상태 라인을 이미 보냈으니 오류 응답은 못 보내고 연결을 닫는다
    }

    // ── 자식 프로세스: 입출력을 연결하고 CGI 프로그램으로 변신(execve) ──
    if (pid == 0) {
        dup2(input_fd, STDIN_FILENO);       // 프로그램의 입력(stdin)을 본문으로
        dup2(fd, STDOUT_FILENO);            // 프로그램의 출력(stdout)을 클라이언트 소켓으로

        char *arguments[2];
        arguments[0] = program;
        arguments[1] = NULL;
        execve(program, arguments, env.list);

        _exit(EXEC_FAILED_CODE);            // 여기까지 왔다면 execve가 실패한 것이다
    }

    // ── 부모 프로세스 ──
    close(input_fd);                        // 입력 fd는 자식이 가져갔으니 부모는 닫는다

    // 5. 자식이 끝날 때까지 기다리고 회수한다 (회수하지 않으면 좀비 프로세스가 남는다)
    return wait_for_child(pid);
}
