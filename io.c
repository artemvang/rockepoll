#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>

#include "io.h"
#include "utils.h"
#include "handler.h"

#define REQ_BUF_SIZE 1024


static inline void
cleanup_request(void *meta)
{
    free(meta);
}


static enum io_step_status
read_request_io_step(struct connection *conn)
{
    ssize_t read_size;
    struct raw_request *req = conn->steps->meta;

    do {
        read_size = read(conn->fd, req->data + req->size, MIN(REQ_BUF_SIZE, MAX_REQ_SIZE - req->size));

        if (read_size < 1) {
            if (likely(read_size < 0 && errno == EAGAIN)) {
                return IO_AGAIN;
            }

            return IO_ERROR;
        }

        req->size += read_size;

    } while (read_size == REQ_BUF_SIZE && req->size < MAX_REQ_SIZE);

    if (unlikely(!req->size || req->size == MAX_REQ_SIZE)) {
        return IO_ERROR;
    } else {
        req->data[req->size] = '\0';
        build_response(conn);
    }

    return IO_OK;
}


void
setup_read_io_step(struct connection *conn)
{
    struct raw_request *req;

    conn->steps = xmalloc(sizeof(struct io_step));

    req = xmalloc(sizeof(struct raw_request));
    req->size = 0;
    conn->steps->meta = req;

    conn->steps->handler = read_request_io_step;
    conn->steps->cleanup = cleanup_request;
    conn->steps->next = NULL;
}


void
process_connection(struct connection *conn)
{
    int run = 1;
    enum io_step_status s;

    while (run && conn->steps) {
        s = (*conn->steps->handler)(conn);

        switch(s) {
        case IO_OK:
            LL_MOVE_NEXT(conn->steps);
            if (!conn->steps && conn->keep_alive) {
                setup_read_io_step(conn);
                run = 0;
            }

            if (!conn->steps) {
                conn->status = C_CLOSE;
                run = 0;
            }
            break;
        case IO_AGAIN:
            run = 0;
            break;
        case IO_ERROR:
            conn->status = C_CLOSE;
            run = 0;
            break;
        }
    }
}
