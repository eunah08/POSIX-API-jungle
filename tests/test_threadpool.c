// [test_threadpool.c] 스레드 풀 단위 테스트
#include "test.h"
#include <pthread.h>
#include "threadpool.h"

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static int handled_count = 0;               // 처리된 작업 수
static int handled_sum = 0;                 // 처리된 fd 값의 합

// 작업: 받은 번호를 세어 둔다 (여러 스레드가 동시에 건드리므로 mutex로 보호)
static void count_task(int connfd) {
    pthread_mutex_lock(&counter_lock);
    handled_count++;
    handled_sum = handled_sum + connfd;
    pthread_mutex_unlock(&counter_lock);
}

// 작업 50개를 넣고 destroy하면, 큐에 남은 것까지 전부 처리된 뒤에 끝난다
static void test_all_tasks_handled(void) {
    threadpool *pool = threadpool_create(4, 64, count_task);
    CHECK(pool != NULL);

    int expected_sum = 0;
    for (int i = 1; i <= 50; i++) {
        CHECK(threadpool_submit(pool, i) == 0);
        expected_sum = expected_sum + i;
    }
    threadpool_destroy(pool);

    CHECK(handled_count == 50);
    CHECK(handled_sum == expected_sum);
}

// 잘못된 인자
static void test_create_invalid(void) {
    CHECK(threadpool_create(0, 8, count_task) == NULL);
    CHECK(threadpool_create(2, 0, count_task) == NULL);
    CHECK(threadpool_create(2, 8, NULL) == NULL);
}

static volatile int blocking_started = 0;   // 막힌 작업이 시작됐는지
static volatile int blocking_release = 0;   // 1이 되면 막힌 작업이 끝난다

// 작업: 풀어 줄 때까지 계속 붙잡고 있는다
static void blocking_task(int connfd) {
    (void)connfd;
    blocking_started = 1;
    while (!blocking_release) {
        usleep(1000);
    }
}

// 워커가 모두 바쁘고 큐도 가득 차면 submit이 -1을 돌려준다 (서버는 이때 503을 보낸다)
static void test_queue_full(void) {
    threadpool *pool = threadpool_create(1, 2, blocking_task);
    CHECK(pool != NULL);

    CHECK(threadpool_submit(pool, 1) == 0);
    while (!blocking_started) {             // 워커가 첫 작업을 꺼내 갈 때까지 기다림
        usleep(1000);
    }
    CHECK(threadpool_submit(pool, 2) == 0); // 큐 칸 1
    CHECK(threadpool_submit(pool, 3) == 0); // 큐 칸 2
    CHECK(threadpool_submit(pool, 4) == -1);// 가득 참

    blocking_release = 1;
    threadpool_destroy(pool);
}

int main(void) {
    test_all_tasks_handled();
    test_create_invalid();
    test_queue_full();
    return TEST_RESULT();
}
