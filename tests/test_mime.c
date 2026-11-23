// [test_mime.c] MIME 타입 매핑 단위 테스트
#include "test.h"
#include "mime.h"

// html, css, js, png, jpg, jpeg 기본 매핑
static void test_known_types(void) {
    CHECK_STR(mime_type("/index.html"), "text/html");
    CHECK_STR(mime_type("/style.css"), "text/css");
    CHECK_STR(mime_type("/app.js"), "application/javascript");
    CHECK_STR(mime_type("/a.png"), "image/png");
    CHECK_STR(mime_type("/a.jpg"), "image/jpeg");
    CHECK_STR(mime_type("/a.jpeg"), "image/jpeg");
    CHECK_STR(mime_type("/a.json"), "application/json");
}

// 대문자 확장자(".HTML")도 같은 결과
static void test_case_insensitive(void) {
    CHECK_STR(mime_type("/INDEX.HTML"), "text/html");
    CHECK_STR(mime_type("/a.JpG"), "image/jpeg");
}

// 확장자 없음, 모르는 확장자, "/dir.v1/file"(디렉터리의 '.') → application/octet-stream
static void test_unknown_types(void) {
    CHECK_STR(mime_type("/Makefile"), "application/octet-stream");
    CHECK_STR(mime_type("/a.unknown"), "application/octet-stream");
    CHECK_STR(mime_type("/dir.v1/file"), "application/octet-stream");
    CHECK_STR(mime_type("/dir.html/file"), "application/octet-stream");
}

int main(void) {
    test_known_types();
    test_case_insensitive();
    test_unknown_types();
    return TEST_RESULT();
}
