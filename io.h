
#ifndef IO_H
#define IO_H

#include <sys/types.h>
#include <time.h>
#include <stdlib.h>


#define MAX_REQ_SIZE 4096


#define LL_PUSH(head,item)       \
do { \
    __typeof(item) _tmp; \
    if (head) { \
        _tmp = (head); \
        while (_tmp->next) _tmp = _tmp->next; \
        _tmp->next=(item); \
    } else { \
        (head)=(item); \
    } \
} while (0)

#define LL_MOVE_NEXT(head) \
do { \
    __typeof(head) _tmp; \
    _tmp = (head)->next; \
    (*(head)->cleanup)((head)->meta); \
    free(head); \
    (head) = _tmp; \
} while (0)

#define LL_CLEAN(head)       \
do {                               \
    while (head) LL_MOVE_NEXT(head); \
} while (0)


enum io_step_status {IO_OK, IO_AGAIN, IO_ERROR};
enum conn_status {C_RUN, C_CLOSE};


struct send_meta {
    size_t size;
    char *data;
};


struct sendfile_meta {
    off_t start_offset, end_offset, size;
    int infd;
};


struct read_meta {
    size_t size;
    char data[MAX_REQ_SIZE];
};


struct connection;

struct io_step {
    void *meta;
    enum io_step_status (*action)(struct connection *conn);
    void (*cleanup)(void *meta);
    struct io_step *next;
};


struct connection {
    int fd, status, keep_alive;
    time_t last_active;
    struct io_step *steps;
};


void process_connection(struct connection *conn);
void setup_read_io_step(struct connection *conn);
void setup_send_io_step(struct connection *conn, char *data, size_t size);
void setup_sendfile_io_step(struct connection *conn, int infd, off_t lower, off_t upper, off_t size);


#endif