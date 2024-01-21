#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <deque>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <vector>

struct Work {
    void (*f)(void *) = NULL;
    void *arg = NULL;
};

struct TheadPool {
    std::vector<pthread_t> threads;
    std::deque<Work> queue;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
};

void thread_pool_init(TheadPool *tp, size_t num_threads);
void thread_pool_queue(TheadPool *tp, void (*f)(void *), void *arg);

#endif // THREAD_POOL_H