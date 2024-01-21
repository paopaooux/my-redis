#include "thread_pool.h"
#include <assert.h>

void *worker(void *arg) {
    TheadPool *tp = (TheadPool *)arg;
    while (true) {
        pthread_mutex_lock(&tp->mutex);
        // 等待条件:一个非空队列
        while (tp->queue.empty()) {
            pthread_cond_wait(&tp->not_empty, &tp->mutex);
        }

        Work work = tp->queue.front();
        tp->queue.pop_front();
        pthread_mutex_unlock(&tp->mutex);

        work.f(work.arg);
    }
    return NULL;
}

void thread_pool_init(TheadPool *tp, size_t num_threads) {
    assert(num_threads > 0);

    int ret = pthread_mutex_init(&tp->mutex, NULL);
    assert(ret == 0);
    ret = pthread_cond_init(&tp->not_empty, NULL);
    assert(ret == 0);

    tp->threads.resize(num_threads);
    for (size_t i = 0; i < num_threads; i++) {
        int ret = pthread_create(&tp->threads[i], NULL, &worker, tp);
        assert(ret == 0);
    }
}

void thread_pool_queue(TheadPool *tp, void (*f)(void *), void *arg) {
    Work work;
    work.f = f;
    work.arg = arg;

    pthread_mutex_lock(&tp->mutex);
    tp->queue.push_back(work);
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mutex);
}
