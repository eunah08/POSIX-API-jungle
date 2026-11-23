// [threadpool.c] 스레드 풀 구현
// 큐는 여러 스레드가 동시에 건드리므로 mutex로 보호하고, 비었을 때는 조건 변수로 잠든다
#include "threadpool.h"
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

struct threadpool {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;   // 큐에 작업이 들어오면 워커를 깨움
    int *queue;                 // 원형 큐 (connfd 배열)
    int capacity, head, tail, count;
    pthread_t *threads;
    int nthreads;
    int stopping;               // 1이면 새 작업 거부, 큐가 비면 워커 종료
    task_fn fn;
};

// 워커: 큐에서 연결 하나를 꺼내 fn으로 처리하는 일을 반복한다
static void *worker(void *arg) {
    threadpool *pool = arg;

    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (pool->count == 0 && !pool->stopping) {
            pthread_cond_wait(&pool->not_empty, &pool->lock);
        }
        if (pool->count == 0) {                 // 종료 중이고 남은 작업도 없음
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }
        int connfd = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        pthread_mutex_unlock(&pool->lock);

        pool->fn(connfd);                       // 락 밖에서 처리해야 다른 워커가 동시에 일한다
    }
}

threadpool *threadpool_create(int nthreads, int queue_size, task_fn fn) {
    if (nthreads <= 0 || queue_size <= 0 || fn == NULL) return NULL;

    threadpool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) return NULL;
    pool->queue = malloc(sizeof(int) * (size_t)queue_size);
    pool->threads = malloc(sizeof(pthread_t) * (size_t)nthreads);
    if (pool->queue == NULL || pool->threads == NULL) {
        free(pool->queue);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    pool->capacity = queue_size;
    pool->fn = fn;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->not_empty, NULL);

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker, pool) != 0) break;
        pool->nthreads++;
    }
    if (pool->nthreads != nthreads) {           // 스레드를 다 못 만들었으면 만든 것만 정리하고 실패
        threadpool_destroy(pool);
        return NULL;
    }
    return pool;
}

int threadpool_submit(threadpool *pool, int connfd) {
    pthread_mutex_lock(&pool->lock);
    if (pool->stopping || pool->count == pool->capacity) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }
    pool->queue[pool->tail] = connfd;
    pool->tail = (pool->tail + 1) % pool->capacity;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

void threadpool_destroy(threadpool *pool) {
    pthread_mutex_lock(&pool->lock);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->nthreads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->not_empty);
    free(pool->queue);
    free(pool->threads);
    free(pool);
}
