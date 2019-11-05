#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <err.h>

#include "thpool.h"
#include "utils.h"


#define MAX_THREADS 32


static int
thpool_free(struct thpool *pool)
{
    if (!pool) {
        return -1;
    }

    if (pool->threads) {
        free(pool->threads);
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->notify);
    }

    free(pool);    
    return 0;
}

static void *
worker(struct thpool *pool)
{
    struct task *t;

    for(;;) {
        pthread_mutex_lock(&pool->lock);

        while (!pool->queue && !pool->shutdown) {
            pthread_cond_wait(&pool->notify, &pool->lock);
        }

        if (pool->shutdown) {
            break;
        }

        pool->threads_working++;
        t = pool->queue;
        pool->queue = t->next;

        pthread_mutex_unlock(&pool->lock);

        (*t->func)(t->args);
        free(t);

        pthread_mutex_lock(&pool->lock);
        pool->threads_working--;
        if (!pool->queue) {
            pthread_cond_signal(&pool->all_idle);
        }
        pthread_mutex_unlock(&pool->lock);
    }

    pthread_mutex_unlock(&pool->lock);
    pthread_exit(NULL);

    return NULL;
}


void
thpool_destroy(struct thpool *pool)
{
    int i;

    if (!pool) {
        return;
    }

    pthread_mutex_lock(&pool->lock);

    pool->shutdown = 1;

    pthread_cond_broadcast(&pool->notify);

    pthread_mutex_unlock(&pool->lock);

    for (i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    thpool_free(pool);
}


struct thpool *
thpool_create(int thread_count)
{
    int i;
    struct thpool *pool;

    if(thread_count <= 0 || thread_count > MAX_THREADS) {
        return NULL;
    }

    pool = malloc(sizeof(struct thpool));
    pool->thread_count = 0;
    pool->queue = NULL;
    pool->shutdown = 0;
    pool->threads_working = 0;

    pool->threads = xmalloc(sizeof(pthread_t) * thread_count);

    if (pthread_mutex_init(&pool->lock, NULL)) {
        err(1, "pthread_mutex_init()");
    }

    if (pthread_cond_init(&pool->notify, NULL)) {
        err(1, "pthread_cond_init()");
    }

    if (pthread_cond_init(&pool->all_idle, NULL)) {
        err(1, "pthread_cond_init()");
    }

    for (i = 0; i < thread_count; i++) {
        if (pthread_create(pool->threads + i, NULL, (void *(*)(void *))worker, pool)) {
            thpool_destroy(pool);
            return NULL;
        }
        pool->thread_count++;
    }

    return pool;
}


void
thpool_wait(struct thpool *pool)
{
    pthread_mutex_lock(&pool->lock);
    while (pool->queue || pool->threads_working) {
        pthread_cond_wait(&pool->all_idle, &pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);
}


int
thpool_add(struct thpool *pool, void (*func)(void *), void *args)
{
    struct task *t;

    pthread_mutex_lock(&pool->lock);

    if (!pool->shutdown) {
        t = xmalloc(sizeof(struct task));
        t->func = func;
        t->args = args;

        t->next = pool->queue;
        pool->queue = t;

        pthread_cond_signal(&pool->notify);
    }

    pthread_mutex_unlock(&pool->lock);

    return 0;
}