// [cgi.c] CGI 실행 구현
//
// CGI란: 요청이 올 때마다 별도의 프로그램(cgi-bin/adder 등)을 실행하고, 그 프로그램이 출력한 내용을 응답으로 보내는 방식
//
// 전체 흐름
//   1. 실행할 프로그램 파일을 찾는다 (없으면 404, 실행 못 하면 403)
//   2. fork로 자식 프로세스를 만들고, 자식이 execve로 그 프로그램이 된다
//   3. 프로그램이 출력한 내용은 pipe(관)로 받아서 클라이언트에게 전달한다
//      (소켓에 바로 연결하지 않는 이유: 실행 실패나 무응답일 때 500/504 오류 응답을 보내기 위해)
#include "cgi.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "rio.h"

#define CGI_TIMEOUT_SEC 10                                      // CGI가 이 시간 동안 아무 출력이 없으면 504
#define CGI_STATUS_LINE "HTTP/1.1 200 OK\r\nConnection: close\r\n"   // 나머지 헤더는 CGI 프로그램이 직접 출력한다

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

// pipe를 만든다. exec할 때 자동으로 닫히게 표시해 두어서, 다른 CGI 자식이 이 pipe를 몰래 물려받지 않게 한다
static int make_pipe(int fds[2]) {
    if (pipe(fds) < 0) {
        return -1;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
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

// 자식이 프로그램 실행에 성공했는지 확인한다
//   성공하면 error_fd가 exec와 함께 닫혀서 read가 0을 돌려준다
//   실패하면 자식이 오류 번호를 적어 두고 종료하므로 read가 양수를 돌려준다
// 실행에 성공했으면 1, 실패했으면 0
static int child_started_ok(int error_fd) {
    int error_number;
    ssize_t count = read(error_fd, &error_number, sizeof(error_number));

    while (count < 0 && errno == EINTR) {
        count = read(error_fd, &error_number, sizeof(error_number));
    }

    if (count > 0) {
        return 0;
    }
    return 1;
}

// 문제가 생겼을 때 돌려줄 값을 정한다
//   이미 응답을 보내기 시작했다면 오류 응답을 줄 수 없으니 -1(연결 닫기), 아직이면 오류 코드
static int error_code(int already_sent, int code) {
    if (already_sent) {
        return -1;
    }
    return code;
}

// CGI의 출력을 읽어서 클라이언트(fd)에게 전달한다. 첫 출력이 나올 때 상태 라인을 먼저 보낸다
// 성공 0, 아직 아무것도 못 보냈을 때의 문제는 500/504, 전달 도중 문제는 -1
static int relay_output(int fd, int output_fd) {
    char buffer[8192];
    int already_sent = 0;           // 클라이언트에게 뭔가 보냈는지

    while (1) {
        // 출력이 올 때까지 최대 CGI_TIMEOUT_SEC초 기다린다
        struct pollfd waiting;
        waiting.fd = output_fd;
        waiting.events = POLLIN;
        waiting.revents = 0;
        int ready = poll(&waiting, 1, CGI_TIMEOUT_SEC * 1000);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return error_code(already_sent, 500);
        }
        if (ready == 0) {           // 시간 초과
            return error_code(already_sent, 504);
        }

        ssize_t count = read(output_fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return error_code(already_sent, 500);
        }
        if (count == 0) {           // CGI가 출력을 끝냈다
            if (already_sent) {
                return 0;
            }
            return 500;             // 아무 출력도 없었다면 오류
        }

        if (!already_sent) {
            if (rio_writen(fd, CGI_STATUS_LINE, strlen(CGI_STATUS_LINE)) < 0) {
                return -1;
            }
            already_sent = 1;
        }
        if (rio_writen(fd, buffer, count) < 0) {
            return -1;              // 클라이언트가 연결을 끊음
        }
    }
}

int serve_cgi(int fd, const char *cgi_dir, const http_request *req) {
    // 1. 실행할 프로그램 확인
    char program[1024];
    int error = find_program(program, sizeof(program), cgi_dir, req->path);
    if (error != 0) {
        return error;
    }

    // 2. 자식에게 줄 환경변수와 입력, 출력용 pipe 준비 (fork 전에 모두 만들어 둔다)
    cgi_env env;
    fill_env(&env, req);

    int input_fd = open_cgi_input(req);
    if (input_fd < 0) {
        return 500;
    }
    int output_pipe[2];             // [0] = 읽는 쪽(부모), [1] = 쓰는 쪽(자식)
    int error_pipe[2];              // 자식이 실행에 실패했을 때 알려 주는 용도
    if (make_pipe(output_pipe) < 0) {
        close(input_fd);
        return 500;
    }
    if (make_pipe(error_pipe) < 0) {
        close(input_fd);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return 500;
    }

    // 3. fork: 이 줄 이후 부모와 자식이 각각 아래 코드를 실행한다
    pid_t pid = fork();
    if (pid < 0) {
        close(input_fd);
        close(output_pipe[0]);
        close(output_pipe[1]);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return 500;
    }

    // ── 자식 프로세스: 입출력을 연결하고 CGI 프로그램으로 변신(execve) ──
    if (pid == 0) {
        dup2(input_fd, STDIN_FILENO);           // 프로그램의 입력(stdin)을 본문으로
        dup2(output_pipe[1], STDOUT_FILENO);    // 프로그램의 출력(stdout)을 pipe로

        char *arguments[2];
        arguments[0] = program;
        arguments[1] = NULL;
        execve(program, arguments, env.list);

        // 여기까지 왔다면 execve가 실패한 것이다: 오류 번호를 부모에게 알리고 종료
        int error_number = errno;
        ssize_t written = write(error_pipe[1], &error_number, sizeof(error_number));
        (void)written;
        _exit(127);
    }

    // ── 부모 프로세스 ──
    close(input_fd);                // 자식이 쓰는 쪽은 부모가 쓸 일이 없으니 닫는다
    close(output_pipe[1]);
    close(error_pipe[1]);

    int started = child_started_ok(error_pipe[0]);
    close(error_pipe[0]);
    if (!started) {
        close(output_pipe[0]);
        waitpid(pid, NULL, 0);
        return 500;
    }

    // 4. CGI 출력을 클라이언트에게 전달
    int result = relay_output(fd, output_pipe[0]);
    close(output_pipe[0]);

    // 5. 정리: 문제가 있었다면 자식을 강제 종료하고, 항상 waitpid로 자식을 회수한다 (안 하면 좀비 프로세스가 남는다)
    if (result != 0) {
        kill(pid, SIGKILL);
    }
    waitpid(pid, NULL, 0);
    return result;
}
