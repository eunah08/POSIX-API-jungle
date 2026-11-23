// [test_cgi.c] CGI 단위 테스트 — 임시 폴더에 작은 셸 스크립트를 CGI 프로그램으로 만들어 실행
#include "test.h"
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "cgi.h"

static char dir[64];            // 임시 CGI 폴더

// 파일을 만들고 권한(mode)을 정한다
static void make_program(const char *name, const char *content, int mode) {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *file = fopen(path, "w");
    fputs(content, file);
    fclose(file);
    chmod(path, mode);
}

static void remove_program(const char *name) {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    remove(path);
}

// serve_cgi를 부르고 반환값을 돌려준다. CGI가 출력한 응답은 response에 담긴다
static int run_cgi(const char *method, const char *path, const char *query, const char *body, char *response, size_t size) {
    http_request req;
    memset(&req, 0, sizeof(req));
    strcpy(req.method, method);
    strcpy(req.path, path);
    strcpy(req.query, query);
    strcpy(req.version, "HTTP/1.1");
    if (body != NULL) {
        req.body = (char *)body;
        req.content_length = strlen(body);
    }

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int result = serve_cgi(sv[1], dir, &req);
    close(sv[1]);
    read_all(sv[0], response, size);
    close(sv[0]);
    return result;
}

// GET: 환경변수(REQUEST_METHOD, QUERY_STRING)가 CGI에 전달된다
static void test_get(void) {
    char response[2048];
    CHECK(run_cgi("GET", "/cgi-bin/show", "a=1&b=2", NULL, response, sizeof(response)) == 0);
    CHECK(strncmp(response, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(response, "method=GET query=a=1&b=2") != NULL);
}

// POST: 본문이 CGI의 stdin으로 전달된다
static void test_post(void) {
    char response[2048];
    CHECK(run_cgi("POST", "/cgi-bin/echo", "", "a=1&b=2", response, sizeof(response)) == 0);
    CHECK(strstr(response, "a=1&b=2") != NULL);
}

// 오류: 없는 프로그램 404, ".." 포함 403, 실행 권한 없음 403, 실행할 수 없는 파일은 연결 종료(-1)
static void test_errors(void) {
    char response[2048];
    CHECK(run_cgi("GET", "/cgi-bin/nope", "", NULL, response, sizeof(response)) == 404);
    CHECK(run_cgi("GET", "/cgi-bin/../secret", "", NULL, response, sizeof(response)) == 403);
    CHECK(run_cgi("GET", "/cgi-bin/", "", NULL, response, sizeof(response)) == 404);
    CHECK(run_cgi("GET", "/cgi-bin/noexec", "", NULL, response, sizeof(response)) == 403);
    CHECK(run_cgi("GET", "/cgi-bin/broken", "", NULL, response, sizeof(response)) == -1);
}

int main(void) {
    strcpy(dir, "/tmp/cgi_test_XXXXXX");
    if (mkdtemp(dir) == NULL) {              // 임시 폴더를 못 만들면 테스트를 할 수 없다
        perror("mkdtemp");
        return 1;
    }

    make_program("show", "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\n'\nprintf 'method=%s query=%s' \"$REQUEST_METHOD\" \"$QUERY_STRING\"\n", 0755);
    make_program("echo", "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\n'\ncat\n", 0755);
    make_program("noexec", "#!/bin/sh\necho hi\n", 0644);
    make_program("broken", "this is not a program\n", 0755);          // 실행 권한은 있지만 실행 불가능한 파일

    test_get();
    test_post();
    test_errors();

    remove_program("show");
    remove_program("echo");
    remove_program("noexec");
    remove_program("broken");
    rmdir(dir);
    return TEST_RESULT();
}
