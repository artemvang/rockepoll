
#ifndef THPOOL_H
#define THPOOL_H

#include <pthread.h>


struct task {
    void (*func)(void *);
    void *args;
    struct task *next;
};


struct thpool {
    pthread_mutex_t lock;
    pthread_cond_t notify, all_idle;
    pthread_t *threads;
    struct task *queue;
    int thread_count, threads_working;
    int shutdown;
};


struct thpool *thpool_create(int thread_count);
void thpool_wait(struct thpool *pool);
int thpool_add(struct thpool *pool, void (*func)(void *), void *args);
void thpool_destroy(struct thpool *pool);

#endif
