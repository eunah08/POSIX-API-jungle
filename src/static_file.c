// [static_file.c] 정적 파일 서빙 구현
#include "static_file.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "http_response.h"
#include "mime.h"
#include "rio.h"

// real_path가 root_real_path 폴더 안에 있으면 1
// 예) 루트가 "/www"일 때 "/www/a.html"은 안, "/www-secret/a.html"과 "/etc/passwd"는 밖
static int is_inside_root(const char *root_real_path, const char *real_path) {
    size_t root_length = strlen(root_real_path);

    if (strncmp(real_path, root_real_path, root_length) != 0) {
        return 0;                   // 앞부분이 다르면 밖
    }
    // 앞부분이 같아도 "/www-secret"처럼 이름이 더 이어지면 다른 폴더다
    char next_char = real_path[root_length];
    if (next_char == '\0' || next_char == '/') {
        return 1;
    }
    return 0;
}

// 파일 내용을 읽어서 fd로 조금씩 보낸다 (큰 파일도 메모리에 전부 올리지 않는다)
// 성공 0, 중간에 오류가 나면 -1
static int send_file_body(int fd, const char *file_path) {
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        return -1;
    }

    char buffer[8192];
    int result = 0;

    while (1) {
        ssize_t count = read(file_fd, buffer, sizeof(buffer));
        if (count < 0) {
            result = -1;            // 읽기 오류
            break;
        }
        if (count == 0) {
            break;                  // 파일 끝
        }
        if (rio_writen(fd, buffer, count) < 0) {
            result = -1;            // 클라이언트가 연결을 끊음
            break;
        }
    }

    close(file_fd);
    return result;
}

int serve_static(int fd, const char *root, const http_request *req) {
    // 정적 파일은 GET, HEAD만 가능
    if (strcmp(req->method, "POST") == 0) {
        return 405;
    }

    // 1. 파일 경로 만들기: root + 요청 경로. "/"로 끝나면 index.html을 붙인다
    const char *index_name = "";
    size_t path_length = strlen(req->path);
    if (req->path[path_length - 1] == '/') {
        index_name = "index.html";
    }
    char wanted_path[PATH_MAX];
    int length = snprintf(wanted_path, sizeof(wanted_path), "%s%s%s", root, req->path, index_name);
    if (length < 0 || length >= (int)sizeof(wanted_path)) {
        return 404;                 // 경로가 너무 김
    }

    // 2. "../" 같은 것을 풀어서 진짜 경로를 구하고, 루트 폴더 밖이면 거절한다
    char root_real_path[PATH_MAX];
    char file_real_path[PATH_MAX];
    if (realpath(root, root_real_path) == NULL) {
        return 500;                 // 루트 폴더 자체가 없음 (서버 설정 문제)
    }
    if (realpath(wanted_path, file_real_path) == NULL) {
        return 404;                 // 파일이 없음
    }
    if (!is_inside_root(root_real_path, file_real_path)) {
        return 403;
    }

    // 3. 일반 파일이고 읽을 수 있어야 한다 (폴더 등은 거절)
    struct stat info;
    if (stat(file_real_path, &info) < 0) {
        return 404;
    }
    if (!S_ISREG(info.st_mode) || access(file_real_path, R_OK) != 0) {
        return 403;
    }

    // 4. 헤더 보내기
    if (send_headers(fd, 200, mime_type(file_real_path), info.st_size, req->keep_alive) < 0) {
        return -1;
    }

    // 5. HEAD는 헤더만, GET은 본문까지
    if (strcmp(req->method, "HEAD") == 0) {
        return 0;
    }
    return send_file_body(fd, file_real_path);
}
