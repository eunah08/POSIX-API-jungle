// [adder.c] CGI 예제 — 두 수를 더한 결과를 출력 (동적 요청 처리 데모)
// 서버가 stdout을 pipe로 연결해 두므로 printf가 곧 응답 내용이 된다 (상태 라인은 서버가 먼저 보낸다)
//   GET  /cgi-bin/adder?a=1&b=2     → QUERY_STRING에서 읽기
//   POST /cgi-bin/adder  (a=1&b=2)  → stdin에서 CONTENT_LENGTH만큼 읽기
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// "a=1&b=2" 같은 문자열에서 이름(name)에 해당하는 숫자를 찾아 result에 넣는다
// 찾았고 숫자가 맞으면 1, 아니면 0
static int get_number(const char *params, const char *name, long *result) {
    char key[32];
    snprintf(key, sizeof(key), "%s=", name);            // 예: "a="

    // "a="이 나오는 곳을 찾는다. 맨 앞이거나 '&' 바로 뒤여야 진짜 이름이다 ("data=" 속의 "a=" 같은 오인 방지)
    const char *found = strstr(params, key);
    while (found != NULL) {
        if (found == params || found[-1] == '&') {
            break;
        }
        found = strstr(found + 1, key);
    }
    if (found == NULL) {
        return 0;
    }

    // '=' 뒤의 글자를 숫자로 바꾼다
    const char *number_text = found + strlen(key);
    char *end;
    *result = strtol(number_text, &end, 10);

    if (end == number_text) {
        return 0;                   // 숫자가 하나도 없음
    }
    if (*end != '&' && *end != '\0') {
        return 0;                   // 숫자 뒤에 이상한 글자가 있음 ("12abc")
    }
    return 1;
}

int main(void) {
    char params[1024] = "";
    const char *method = getenv("REQUEST_METHOD");

    // 1. 입력 읽기: POST는 본문(stdin), 그 외에는 주소의 ?뒤 부분(QUERY_STRING)
    if (method != NULL && strcmp(method, "POST") == 0) {
        const char *length_text = getenv("CONTENT_LENGTH");
        size_t length = 0;
        if (length_text != NULL) {
            length = atol(length_text);
        }
        if (length >= sizeof(params)) {
            length = sizeof(params) - 1;
        }
        size_t got = fread(params, 1, length, stdin);
        params[got] = '\0';
    } else {
        const char *query = getenv("QUERY_STRING");
        if (query != NULL) {
            snprintf(params, sizeof(params), "%s", query);
        }
    }

    // 2. 계산해서 응답 내용 만들기
    char body[256];
    long a;
    long b;
    if (get_number(params, "a", &a) && get_number(params, "b", &b)) {
        snprintf(body, sizeof(body), "%ld + %ld = %ld\n", a, b, a + b);
    } else {
        snprintf(body, sizeof(body), "usage: a=<number>&b=<number>\n");
    }

    // 3. 헤더 + 빈 줄 + 본문 출력 (이 출력이 그대로 클라이언트에게 전달된다)
    printf("Content-Type: text/plain\r\n");
    printf("Content-Length: %zu\r\n\r\n", strlen(body));
    printf("%s", body);
    return 0;
}
