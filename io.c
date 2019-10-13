#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <fcntl.h>

#include "io.h"
#include "utils.h"
#include "utlist.h"
#include "utstring.h"

#define REQ_BUF_SIZE 1024
#define SENDFILE_CHUNK_SIZE 1024 * 512


#define BUILD_IO_STEP(__step_type)                                                             \
do {                                                                                           \
    struct io_step *__step = xmalloc(sizeof(struct io_step));                                  \
    __step->io_flags = io_flags;                                                               \
    __step->meta = meta;                                                                       \
    __step->step = make_ ## __step_type ## _step;                                              \
    __step->handle = handler;                                                                  \
    __step->clean = clean_ ## __step_type ## _step;                                            \
    __step->next = NULL;                                                                       \
    LL_APPEND(conn->steps, __step);                                                            \
} while(0)


static ALWAYS_INLINE void
clean_read_step(void *meta)
{
    free(meta);
}


static ALWAYS_INLINE void
clean_send_step(void *meta)
{
    struct send_meta *h = meta;
    utstring_free(h->data);
    free(meta);
}


static ALWAYS_INLINE void
clean_sendfile_step(void *meta)
{   
    struct sendfile_meta *f = meta;
    close(f->infd);
    free(meta);
}


static enum io_step_status
make_sendfile_step(struct connection *conn)
{
    off_t size;
    ssize_t sent_len;
    struct sendfile_meta *meta;

    meta = conn->steps->meta;

    do {
        size = MIN(SENDFILE_CHUNK_SIZE, meta->size);
        sent_len = sendfile(conn->fd, meta->infd, &(meta->start_offset), size);
        if (sent_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return IO_AGAIN;
            }

            return IO_ERROR;
        }

        meta->size -= sent_len;
    } while (meta->start_offset < meta->end_offset);

    return IO_OK;
}


static enum io_step_status
make_send_step(struct connection *conn)
{
    int flags;
    ssize_t write_size;
    struct send_meta *meta;

    flags = conn->steps->io_flags;
    meta = conn->steps->meta;

    write_size = send(conn->fd, utstring_body(meta->data), utstring_len(meta->data), flags);
    if (write_size < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return IO_AGAIN;
        }

        return IO_ERROR;
    }

    return IO_OK;
}


static enum io_step_status
make_read_step(struct connection *conn)
{
    ssize_t read_size;
    struct read_meta *req;

    req = conn->steps->meta;
    do {
        read_size = read(conn->fd, req->data + req->size, MIN(REQ_BUF_SIZE, MAX_REQ_SIZE - req->size));

        if (read_size < 1) {
            if (read_size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return IO_AGAIN;
            }

            return IO_ERROR;
        }

        req->size += read_size;

    } while (read_size == REQ_BUF_SIZE && req->size < MAX_REQ_SIZE);

    if (UNLIKELY(!req->size || req->size == MAX_REQ_SIZE)) {
        return IO_ERROR;
    }
    req->data[req->size] = '\0';

    return IO_OK;
}


ALWAYS_INLINE void
setup_sendfile_io_step(struct connection *conn,
                       int io_flags,
                       int infd, off_t lower, off_t upper, off_t size,
                       enum conn_status (*handler)(struct connection *conn))
{
    struct sendfile_meta *meta = xmalloc(sizeof(struct sendfile_meta));
    meta->infd = infd;
    meta->start_offset = lower;
    meta->end_offset = upper;
    meta->size = size;

    BUILD_IO_STEP(sendfile);
}


ALWAYS_INLINE void
setup_send_io_step(struct connection *conn,
                   int io_flags,
                   UT_string *str,
                   enum conn_status (*handler)(struct connection *conn))
{
    struct send_meta *meta = xmalloc(sizeof(struct send_meta));
    meta->data = str;

    BUILD_IO_STEP(send);
}


ALWAYS_INLINE void
setup_read_io_step(struct connection *conn,
                   int io_flags, 
                   enum conn_status (*handler)(struct connection *conn))
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
    struct io_step *steps_head;

    while (run && conn->steps) {
        steps_head = conn->steps;
        s = (*steps_head->step)(conn);

        switch(s) {
        case IO_OK:
            if (*steps_head->handle && (*steps_head->handle)(conn) == C_CLOSE) {
                run = 0;
            }
            MOVE_NEXT_AND_CLEAN(steps_head);
            if (!steps_head) {
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

        conn->steps = steps_head;
    }
}
