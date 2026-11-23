// [test_static_file.c] 정적 파일 서빙 단위 테스트 — 임시 폴더에 파일을 만들어 놓고 응답을 확인
#include "test.h"
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "static_file.h"

static char base[64];           // 임시 폴더 (예: /tmp/static_test_AbC123)
static char root[96];           // base/www  ← 서버 루트
static char secret[96];         // base/secret.txt  ← 루트 밖의 파일 (읽히면 안 됨)

static void write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "w");
    fputs(content, file);
    fclose(file);
}

static int setup(void) {
    strcpy(base, "/tmp/static_test_XXXXXX");
    if (mkdtemp(base) == NULL) {            // 임시 폴더를 못 만들면 테스트를 할 수 없다
        perror("mkdtemp");
        return -1;
    }
    snprintf(root, sizeof(root), "%s/www", base);
    mkdir(root, 0755);

    char path[160];
    snprintf(path, sizeof(path), "%s/index.html", root);
    write_file(path, "<h1>hello</h1>");
    snprintf(path, sizeof(path), "%s/style.css", root);
    write_file(path, "body {}");
    snprintf(path, sizeof(path), "%s/sub", root);
    mkdir(path, 0755);
    snprintf(secret, sizeof(secret), "%s/secret.txt", base);
    write_file(secret, "secret");
    return 0;
}

static void cleanup(void) {
    char path[160];
    snprintf(path, sizeof(path), "%s/index.html", root);
    remove(path);
    snprintf(path, sizeof(path), "%s/style.css", root);
    remove(path);
    snprintf(path, sizeof(path), "%s/sub", root);
    rmdir(path);
    remove(secret);
    rmdir(root);
    rmdir(base);
}

// method, path로 요청을 만들어 serve_static을 부르고, 반환값을 돌려준다. 응답은 response에 담긴다
static int request(const char *method, const char *path, char *response, size_t size) {
    http_request req;
    memset(&req, 0, sizeof(req));
    strcpy(req.method, method);
    strcpy(req.path, path);
    strcpy(req.version, "HTTP/1.1");

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int result = serve_static(sv[1], root, &req);
    close(sv[1]);
    read_all(sv[0], response, size);
    close(sv[0]);
    return result;
}

static void test_serve_file(void) {
    char response[2048];

    CHECK(request("GET", "/index.html", response, sizeof(response)) == 0);
    CHECK(strncmp(response, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(response, "Content-Type: text/html\r\n") != NULL);
    CHECK(strstr(response, "Content-Length: 14\r\n") != NULL);
    CHECK(strstr(response, "<h1>hello</h1>") != NULL);

    CHECK(request("GET", "/style.css", response, sizeof(response)) == 0);
    CHECK(strstr(response, "Content-Type: text/css\r\n") != NULL);
}

// 경로가 "/"로 끝나면 index.html을 보낸다
static void test_index(void) {
    char response[2048];
    CHECK(request("GET", "/", response, sizeof(response)) == 0);
    CHECK(strstr(response, "<h1>hello</h1>") != NULL);
}

// HEAD는 헤더만 보내고 본문은 보내지 않는다
static void test_head(void) {
    char response[2048];
    CHECK(request("HEAD", "/index.html", response, sizeof(response)) == 0);
    CHECK(strstr(response, "Content-Length: 14\r\n") != NULL);
    CHECK(strstr(response, "<h1>") == NULL);
}

// 오류: 없는 파일 404, 루트 밖 접근 403, 폴더 403, POST 405
static void test_errors(void) {
    char response[2048];
    CHECK(request("GET", "/nope.html", response, sizeof(response)) == 404);
    CHECK(request("GET", "/../secret.txt", response, sizeof(response)) == 403);
    CHECK(strstr(response, "secret") == NULL);
    CHECK(request("GET", "/sub", response, sizeof(response)) == 403);
    CHECK(request("GET", "/sub/", response, sizeof(response)) == 404);      // sub/index.html이 없음
    CHECK(request("POST", "/index.html", response, sizeof(response)) == 405);
}

int main(void) {
    if (setup() != 0) {
        return 1;
    }
    test_serve_file();
    test_index();
    test_head();
    test_errors();
    cleanup();
    return TEST_RESULT();
}
