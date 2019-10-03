#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "io.h"
#include "utils.h"
#include "handler.h"


void
cleanup_lol_meta(void *meta)
{
    free(meta);
}


enum io_step_status
write_lol(struct connection *conn)
{
    ssize_t write_size;
    char *r;

    r = conn->steps->meta;
    write_size = write(conn->fd, r, strlen(r));
    if (write_size < 0) {
        if (errno == EAGAIN) {
            return IO_AGAIN;
        }

        return IO_ERROR;
    }

    return IO_OK;
}


void
build_response(struct connection *conn)
{
    struct io_step *step;
    char *r;

    step = xmalloc(sizeof(struct io_step));

    r = xmalloc(512);
    strcpy(r,
           "HTTP/1.1 200 OK\r\n"
           "Connection: keep-alive\r\n"
           "Keep-Alive: timeout=60\r\n"
           "Content-Length: 12\r\n"
           "\r\n"
           "<h1>LOL</h1>");

    step->meta = r;
    step->handler = write_lol;
    step->cleanup = cleanup_lol_meta;
    step->next = NULL;

    LL_PUSH(conn->steps, step);
}