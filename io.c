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
#include "http.h"

#define REQ_BUF_SIZE 1024
#define SENDFILE_CHUNK_SIZE 1024 * 512


#define BUILD_IO_STEP(__step_type) \
    do { \
        struct io_step *__step = xmalloc(sizeof(struct io_step)); \
        __step->meta = meta; \
        __step->action = perform_ ## __step_type ## _action; \
        __step->cleanup = cleanup_ ## __step_type ## _action; \
        __step->next = NULL; \
        LL_PUSH(conn->steps, __step); \
    } while(0);


static inline void
cleanup_read_action(void *meta)
{
    free(meta);
}


static inline void
cleanup_send_action(void *meta)
{
    struct send_meta *h = meta;
    free(h->data);
    free(meta);
}

static inline void
cleanup_sendfile_action(void *meta)
{   
    struct sendfile_meta *f = meta;
    close(f->infd);
    free(meta);
}


static enum io_step_status
perform_sendfile_action(struct connection *conn)
{   
    off_t size;
    ssize_t sent_len;
    struct sendfile_meta *meta;

    meta = conn->steps->meta;
    do {
        size = MIN(SENDFILE_CHUNK_SIZE, meta->size);
        sent_len = sendfile(conn->fd, meta->infd, &(meta->start_offset), size);
        if (unlikely(sent_len < 0)) {
            if (likely(errno == EAGAIN)) {
                return IO_AGAIN;
            }

            return IO_ERROR;
        }

        meta->size -= sent_len;
    } while (meta->start_offset < meta->end_offset);

    return IO_OK;
}


static enum io_step_status
perform_send_action(struct connection *conn)
{
    ssize_t write_size;
    struct send_meta *meta;

    meta = conn->steps->meta;
    write_size = send(conn->fd, meta->data, meta->size, 0);
    if (unlikely(write_size < 0)) {
        if (likely(errno == EAGAIN)) {
            return IO_AGAIN;
        }

        return IO_ERROR;
    }

    return IO_OK;
}


static enum io_step_status
perform_read_action(struct connection *conn)
{
    ssize_t read_size;
    struct read_meta *req;

    req = conn->steps->meta;
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


inline __attribute__((always_inline)) void
setup_sendfile_io_step(struct connection *conn, int infd, off_t lower, off_t upper, off_t size)
{
    struct sendfile_meta *meta = xmalloc(sizeof(struct sendfile_meta));
    meta->infd = infd;
    meta->start_offset = lower;
    meta->end_offset = upper;
    meta->size = size;

    BUILD_IO_STEP(sendfile);
}


inline __attribute__((always_inline)) void
setup_send_io_step(struct connection *conn, char* data, size_t size) {
    struct send_meta *meta = xmalloc(sizeof(struct send_meta));
    meta->data = data;
    meta->size = size;

    BUILD_IO_STEP(send);
}


inline __attribute__((always_inline)) void
setup_read_io_step(struct connection *conn)
{
    struct read_meta *meta = xmalloc(sizeof(struct read_meta));
    meta->size = 0;

    BUILD_IO_STEP(read);
}


void
process_connection(struct connection *conn)
{
    int run = 1;
    enum io_step_status s;

    while (run && conn->steps) {
        s = (*conn->steps->action)(conn);

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
