
#ifndef IO_H
#define IO_H

#include <sys/types.h>
#include <time.h>
#include <stdlib.h>
#include <netinet/tcp.h>

#include "utlist.h"


#define MAX_REQ_SIZE 4096
#define IO_FLAG_NONE 0
#define IO_FLAG_SEND_CORK MSG_MORE

#define MOVE_NEXT_AND_CLEAN(__head)                                                            \
do {                                                                                           \
    struct io_step *__step = __head;                                                           \
    (*(__step)->clean)((__step)->meta);                                                        \
    LL_DELETE(__head, __step);                                                                 \
    free(__step);                                                                              \
} while (0)

#define FREE_IO_STEPS(__head)                                                                  \
do {                                                                                           \
    struct io_step *__elt, *__tmp;                                                             \
    LL_FOREACH_SAFE(__head, __elt, __tmp) {                                                    \
        (*(__elt)->clean)((__elt)->meta);                                                      \
        free(__elt);                                                                           \
    }                                                                                          \
} while (0)


enum io_step_status {IO_OK, IO_AGAIN, IO_ERROR};
enum conn_status {C_RUN, C_CLOSE};


struct send_meta {
    char *data;
    size_t size;
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
    int io_flags;
    void *meta;
    enum io_step_status (*step)(struct connection *conn);
    enum conn_status (*handler)(struct connection *conn);
    void (*clean)(void *meta);
    struct io_step *next;
};


struct connection {
    int fd, keep_alive;
    enum conn_status status;
    time_t last_active;
    char ip[16];
    struct io_step *steps;
    struct connection *next;
    struct connection *prev;
};


void process_connection(struct connection *conn);

void setup_read_io_step(struct connection *conn,
                        int io_flags,
                        enum conn_status (*process_result)(struct connection *conn));

void setup_send_io_step(struct connection *conn,
                        int io_flags,
                        char *data, size_t size,
                        enum conn_status (*process_result)(struct connection *conn));

void setup_sendfile_io_step(struct connection *conn,
                            int io_flags,
                            int infd, off_t lower, off_t upper, off_t size,
                            enum conn_status (*process_result)(struct connection *conn));


#endif