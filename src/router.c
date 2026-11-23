// [router.c] Reverse Proxy 라우팅 구현
#include "router.h"
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 공백·탭·줄바꿈으로 나뉜 다음 단어를 잘라서 돌려준다. 더 없으면 NULL
static char *next_token(char **cursor) {
    char *start = *cursor;

    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (*start == '\0') {
        return NULL;
    }

    char *end = start;
    while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') {
        end++;
    }
    if (*end != '\0') {
        *end = '\0';
        end++;
    }

    *cursor = end;
    return start;
}

// 문자열이 숫자로만 이루어져 있으면 1
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

// "host:port" 한 개를 backend 구조체로 만든다. 형식이 틀리면 -1
static int parse_backend(const char *text, backend *b) {
    const char *colon = strrchr(text, ':');
    if (colon == NULL || colon == text) {
        return -1;                  // ':'가 없거나 호스트 이름이 비어 있음
    }

    size_t host_length = colon - text;
    const char *port = colon + 1;
    size_t port_length = strlen(port);

    if (host_length >= sizeof(b->host)) {
        return -1;
    }
    if (port_length == 0 || port_length >= sizeof(b->port)) {
        return -1;
    }
    if (!is_all_digits(port)) {
        return -1;
    }
    int port_number = atoi(port);
    if (port_number < 1 || port_number > 65535) {
        return -1;
    }

    memcpy(b->host, text, host_length);
    b->host[host_length] = '\0';
    strcpy(b->port, port);
    return 0;
}

// 설정 파일 한 줄을 읽는다. 규칙이면 1, 빈 줄·주석이면 0, 형식이 틀리면 -1
int parse_route_line(const char *line, route *r) {
    char text[1024];
    if (strlen(line) >= sizeof(text)) {
        return -1;
    }
    strcpy(text, line);

    char *cursor = text;
    char *prefix = next_token(&cursor);
    if (prefix == NULL || prefix[0] == '#') {
        return 0;                   // 빈 줄이거나 주석
    }
    if (prefix[0] != '/' || strlen(prefix) >= sizeof(r->prefix)) {
        return -1;
    }

    memset(r, 0, sizeof(*r));
    strcpy(r->prefix, prefix);

    // 나머지 단어들은 전부 "host:port" 백엔드
    char *token = next_token(&cursor);
    while (token != NULL) {
        if (r->backend_count >= MAX_BACKENDS) {
            return -1;
        }
        if (parse_backend(token, &r->backends[r->backend_count]) != 0) {
            return -1;
        }
        r->backend_count++;
        token = next_token(&cursor);
    }

    if (r->backend_count == 0) {
        return -1;                  // 백엔드가 하나도 없음
    }
    return 1;
}

// 설정 파일 전체를 읽어서 table을 채운다. 성공 0, 실패 -1
int load_routes(const char *path, route_table *table) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        perror(path);
        return -1;
    }

    table->count = 0;
    char line[1024];
    int line_number = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;

        if (table->count >= MAX_ROUTES) {
            fprintf(stderr, "%s:%d: 규칙이 너무 많습니다 (최대 %d개)\n", path, line_number, MAX_ROUTES);
            fclose(file);
            return -1;
        }

        int result = parse_route_line(line, &table->routes[table->count]);
        if (result < 0) {
            fprintf(stderr, "%s:%d: 형식이 올바르지 않습니다\n", path, line_number);
            fclose(file);
            return -1;
        }
        if (result == 1) {
            table->count++;         // 규칙이 한 줄 늘었다 (빈 줄·주석이면 늘지 않음)
        }
    }

    fclose(file);
    return 0;
}

// path가 어떤 규칙의 prefix로 시작하는지 찾는다. 여러 개 맞으면 가장 긴 prefix를 고른다
route *match_route(route_table *table, const char *path) {
    route *best = NULL;
    size_t best_length = 0;

    for (int i = 0; i < table->count; i++) {
        size_t prefix_length = strlen(table->routes[i].prefix);
        int matches = (strncmp(path, table->routes[i].prefix, prefix_length) == 0);

        if (matches && prefix_length > best_length) {
            best = &table->routes[i];
            best_length = prefix_length;
        }
    }
    return best;
}

// Round-Robin: 호출할 때마다 백엔드를 차례대로 돌려준다 (1번, 2번, 1번, 2번, ...)
// 워커 스레드 여러 개가 동시에 부르므로, 카운터를 "끊김 없이 한 번에" 1 올리는 atomic 연산을 쓴다
// (일반 next++ 는 두 스레드가 동시에 하면 값이 꼬일 수 있다)
const backend *next_backend(route *r) {
    unsigned int turn = __atomic_fetch_add(&r->next, 1, __ATOMIC_RELAXED);   // 올리기 전의 값을 돌려줌
    int index = turn % r->backend_count;
    return &r->backends[index];
}
