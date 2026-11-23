// [http_request.c] HTTP 요청 파서 구현
// TCP는 메시지 경계가 없으므로 HTTP 규칙으로 경계를 찾는다: 헤더 끝 = 빈 줄("\r\n"), 본문 끝 = Content-Length
#include "http_request.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ───────── 작은 도우미 함수들 ─────────

// line 끝의 줄바꿈(\r, \n)을 뺀 복사본을 out에 만든다. out에 다 안 들어가면 -1
static int copy_without_newline(const char *line, char *out, size_t out_size) {
    size_t length = strlen(line);

    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        length--;
    }
    if (length >= out_size) {
        return -1;
    }

    memcpy(out, line, length);
    out[length] = '\0';
    return 0;
}

// 공백으로 나뉜 다음 단어를 잘라서 돌려준다. 더 이상 단어가 없으면 NULL
// cursor는 "다음에 읽을 위치"를 가리키고, 호출할 때마다 앞으로 움직인다
static char *next_word(char **cursor) {
    char *start = *cursor;

    while (*start == ' ') {         // 단어 앞의 공백 건너뛰기
        start++;
    }
    if (*start == '\0') {
        return NULL;
    }

    char *end = start;
    while (*end != ' ' && *end != '\0') {
        end++;
    }
    if (*end == ' ') {
        *end = '\0';                // 단어 끝에 문자열 끝 표시를 넣어 잘라 낸다
        end++;
    }

    *cursor = end;
    return start;
}

// text 안에 word가 (대소문자 구분 없이) 들어 있으면 1
static int contains_ignore_case(const char *text, const char *word) {
    size_t word_length = strlen(word);
    size_t text_length = strlen(text);

    for (size_t i = 0; i + word_length <= text_length; i++) {
        if (strncasecmp(text + i, word, word_length) == 0) {
            return 1;
        }
    }
    return 0;
}

// 문자열이 숫자로만 이루어져 있으면 1 (빈 문자열은 0)
static int is_all_digits(const char *text) {
    if (text[0] == '\0') {
        return 0;
    }
    for (int i = 0; text[i] != '\0'; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return 0;
        }
    }
    return 1;
}

static int is_absolute_uri_text(const char *uri) {
    return strncasecmp(uri, "http://", 7) == 0;
}

// ───────── 요청 라인 ─────────

// uri를 path와 query로 나누어 req에 채운다
//   "/a.html?x=1"           → path "/a.html",  query "x=1"
//   "http://host/a.html?x=1" → path "/a.html",  query "x=1" (호스트 부분은 건너뜀)
static void split_path_and_query(const char *uri, http_request *req) {
    const char *path_start = uri;

    // 절대 URI면 "http://" 뒤의 호스트 부분을 건너뛰어 경로 시작 위치를 찾는다
    if (is_absolute_uri_text(uri)) {
        path_start = uri + 7;
        while (*path_start != '/' && *path_start != '?' && *path_start != '\0') {
            path_start++;
        }
    }

    const char *question_mark = strchr(path_start, '?');

    // path 만들기
    size_t path_length;
    if (question_mark == NULL) {
        path_length = strlen(path_start);
    } else {
        path_length = question_mark - path_start;
    }
    if (path_length == 0) {
        strcpy(req->path, "/");     // "http://host" 처럼 경로가 없으면 "/"
    } else {
        memcpy(req->path, path_start, path_length);
        req->path[path_length] = '\0';
    }

    // query 만들기
    if (question_mark == NULL) {
        req->query[0] = '\0';
    } else {
        strcpy(req->query, question_mark + 1);
    }
}

int parse_request_line(const char *line, http_request *req) {
    char text[MAX_LINE];
    if (copy_without_newline(line, text, sizeof(text)) < 0) {
        return 414;
    }

    // "메서드 URI 버전" 세 단어로 나눈다
    char *cursor = text;
    char *method = next_word(&cursor);
    char *uri = next_word(&cursor);
    char *version = next_word(&cursor);
    char *extra = next_word(&cursor);

    if (method == NULL || uri == NULL || version == NULL) {
        return 400;                 // 단어가 3개보다 적음
    }
    if (extra != NULL) {
        return 400;                 // 단어가 3개보다 많음
    }

    // 각 부분이 올바른지 확인
    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0 && strcmp(method, "HEAD") != 0) {
        return 501;
    }
    if (strlen(uri) >= MAX_URI_LEN) {
        return 414;
    }
    if (strcmp(version, "HTTP/1.0") != 0 && strcmp(version, "HTTP/1.1") != 0) {
        return 505;
    }
    if (uri[0] != '/' && !is_absolute_uri_text(uri)) {
        return 400;                 // "/경로" 또는 "http://..." 만 받는다
    }

    // 올바르면 req에 저장
    strcpy(req->method, method);
    strcpy(req->uri, uri);
    strcpy(req->version, version);
    split_path_and_query(uri, req);

    // HTTP/1.1은 기본으로 연결 유지, HTTP/1.0은 기본으로 닫음
    if (strcmp(version, "HTTP/1.1") == 0) {
        req->keep_alive = 1;
    } else {
        req->keep_alive = 0;
    }
    return 0;
}

// ───────── 헤더 ─────────

// Content-Length 값을 검사해서 req->content_length에 저장한다. 문제가 있으면 오류 코드
static int save_content_length(const char *value, http_request *req) {
    if (!is_all_digits(value)) {
        return 400;
    }
    if (strlen(value) > 12) {
        return 413;                 // 숫자가 너무 커서 계산하기 전에 이미 1MB 초과
    }

    unsigned long long length = strtoull(value, NULL, 10);
    if (length > MAX_BODY_LEN) {
        return 413;
    }
    req->content_length = (size_t)length;
    return 0;
}

int parse_header_line(const char *line, http_request *req) {
    char text[MAX_LINE];
    if (copy_without_newline(line, text, sizeof(text)) < 0) {
        return 431;
    }

    // "이름: 값" 을 ':' 기준으로 나눈다
    char *colon = strchr(text, ':');
    if (colon == NULL) {
        return 400;
    }
    *colon = '\0';
    char *name = text;
    char *value = colon + 1;

    // 값 앞의 공백 제거
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    // 이름 끝, 값 끝의 공백 제거
    size_t name_length = strlen(name);
    while (name_length > 0 && isspace((unsigned char)name[name_length - 1])) {
        name_length--;
        name[name_length] = '\0';
    }
    size_t value_length = strlen(value);
    while (value_length > 0 && isspace((unsigned char)value[value_length - 1])) {
        value_length--;
        value[value_length] = '\0';
    }

    if (name_length == 0) {
        return 400;
    }
    if (req->header_count >= MAX_HEADERS) {
        return 431;
    }
    if (name_length >= MAX_HEADER_NAME || value_length >= MAX_HEADER_VALUE) {
        return 431;
    }

    // 특별히 처리할 헤더
    if (strcasecmp(name, "Content-Length") == 0) {
        int error = save_content_length(value, req);
        if (error != 0) {
            return error;
        }
    } else if (strcasecmp(name, "Connection") == 0) {
        if (contains_ignore_case(value, "close")) {
            req->keep_alive = 0;
        } else if (contains_ignore_case(value, "keep-alive")) {
            req->keep_alive = 1;
        }
    }

    // 헤더 목록에 저장
    http_header *header = &req->headers[req->header_count];
    strcpy(header->name, name);
    strcpy(header->value, value);
    req->header_count++;
    return 0;
}

// ───────── 요청 전체 읽기 ─────────

int read_request(rio_t *rp, http_request *req) {
    char line[MAX_LINE];
    memset(req, 0, sizeof(*req));

    // 1. 요청 라인 한 줄
    ssize_t length = rio_readlineb(rp, line, sizeof(line));
    if (length <= 0) {
        return -1;                  // 연결이 끊겼거나 시간 초과
    }
    if (line[length - 1] != '\n') {
        return 414;                 // 한 줄이 버퍼보다 길어서 줄 끝이 없음
    }
    int error = parse_request_line(line, req);
    if (error != 0) {
        return error;
    }

    // 2. 빈 줄이 나올 때까지 헤더를 한 줄씩
    while (1) {
        length = rio_readlineb(rp, line, sizeof(line));
        if (length <= 0) {
            return -1;
        }
        if (line[length - 1] != '\n') {
            return 431;
        }
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
            break;                  // 빈 줄 = 헤더 끝
        }
        error = parse_header_line(line, req);
        if (error != 0) {
            return error;
        }
    }

    // 3. 규칙 검사
    if (strcmp(req->version, "HTTP/1.1") == 0 && get_header(req, "Host") == NULL) {
        return 400;                 // HTTP/1.1은 Host 헤더가 꼭 필요
    }
    const char *encoding = get_header(req, "Transfer-Encoding");
    if (encoding != NULL && contains_ignore_case(encoding, "chunked")) {
        return 501;                 // chunked 본문은 지원하지 않음
    }
    if (strcmp(req->method, "POST") == 0 && get_header(req, "Content-Length") == NULL) {
        return 411;                 // POST는 본문 길이를 알려야 함
    }

    // 4. 본문: Content-Length 바이트만큼 정확히 읽는다
    if (req->content_length > 0) {
        req->body = malloc(req->content_length + 1);    // +1은 끝 표시 '\0' 자리
        if (req->body == NULL) {
            return 500;
        }
        ssize_t got = rio_readnb(rp, req->body, req->content_length);
        if (got != (ssize_t)req->content_length) {
            free_request(req);
            return -1;              // 본문을 다 받기 전에 연결이 끊김
        }
        req->body[req->content_length] = '\0';
    }
    return 0;
}

// 이름이 name인 헤더의 값을 찾는다 (대소문자 구분 없음). 없으면 NULL
const char *get_header(const http_request *req, const char *name) {
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

// read_request가 malloc한 본문 메모리를 돌려준다
void free_request(http_request *req) {
    free(req->body);
    req->body = NULL;
}
