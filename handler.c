#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include "io.h"
#include "log.h"
#include "utils.h"
#include "parser.h"
#include "handler.h"
#include "config.h"


#define ETAG_SIZE 64
#define SENDFILE_MIN_SIZE 1024 * 8
#define HEADERS_SIZE 256
#define HTTP_STATUS_FORMAT_SIZE (sizeof(HTTP_STATUS_FORMAT) - 2 - 1)
#define LOG_MESSAGE_FORMAT "%s \"%s\" %d %lu \"%s\"\n"
#define REQUEST_LINE_FORMAT "%s /%s HTTP/%s"


enum http_status {
    S_OK                     = 200,
    S_PARTIAL_CONTENT        = 206,
    S_NOT_FOUND              = 404,
    S_METHOD_NOT_ALLOWED     = 405,
    S_RANGE_NOT_SATISFIABLE  = 416,
    S_BAD_REQUEST            = 400,
    S_FORBIDDEN              = 403,
    S_REQUEST_TOO_LARGE      = 413,
    S_INTERNAL_ERROR         = 500,
    S_VERSION_NOT_SUPPORTED  = 505,
    S_NOT_MODIFIED           = 304,
};

enum file_status {F_EXISTS, F_FORBIDDEN, F_NOT_FOUND, F_INTERNAL_ERROR};

struct file_meta {
    int fd, is_directory;
    ino_t inode;
    char *mime;
    size_t size;
    char etag[ETAG_SIZE];
};


static const char *http_status_str[] = {
    [S_OK]                     = "OK",
    [S_NOT_FOUND]              = "Not Found",
    [S_METHOD_NOT_ALLOWED]     = "Method Not Allowed",
    [S_PARTIAL_CONTENT]        = "Partial Content",
    [S_RANGE_NOT_SATISFIABLE]  = "Range Not Satisfiable",
    [S_BAD_REQUEST]            = "Bad Request",
    [S_FORBIDDEN]              = "Forbidden",
    [S_REQUEST_TOO_LARGE]      = "Request Too Large",
    [S_INTERNAL_ERROR]         = "Internal Server Error",
    [S_VERSION_NOT_SUPPORTED]  = "HTTP Version not supported",
    [S_NOT_MODIFIED]           = "Not Modified",
};


static void
log_new_connection(const struct connection *conn,
                   const struct http_request *req,
                   enum http_status status,
                   size_t content_lenght)
{
    char *user_agent = "-";
    char *request_line = "-";

    if (status != S_BAD_REQUEST) {
        if (req->headers[H_USER_AGENT]) {
            user_agent = req->headers[H_USER_AGENT];
        }

        request_line = xmalloc(strlen(req->target) + 32);
        sprintf(request_line, REQUEST_LINE_FORMAT,
                http_methods[req->method].name,
                req->target,
                http_versions[req->version].name);
    }

    log_log(&conn->last_active,
            LOG_MESSAGE_FORMAT,
            conn->ip, request_line,
            status, content_lenght,user_agent);

    if (status != S_BAD_REQUEST) {
        free(request_line);
    }
}


static char *
get_url_mimetype(const char *url)
{
    size_t i;
    char *mimetype = DEFAULT_MIMETYPE;
    char *extension = strrchr(url, '.');

    if (!extension || extension == url) {
        return mimetype;
    }

    extension++;

    i = sizeof(mimes) / sizeof(*mimes) - 1;
    while (i--) {
        if (!strcmp(extension, mimes[i].ext)) {
            mimetype = mimes[i].type;
            break;
        }
    }

    return mimetype;
}


static enum file_status
gather_file_meta(const char *target, struct file_meta *file_meta)
{
    char *mimetype;
    int fd, is_dir;
    size_t target_size, orig_target_size;
    struct stat st_buf;
    char target_tmp[MAX_TARGET_SIZE] = {0};

    target_size = orig_target_size = strlen(target);
    memcpy(target_tmp, target, target_size + 1);

    for (;;) {
        fd = open(target_tmp, O_LARGEFILE | O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            return (errno == EACCES) ? F_FORBIDDEN : F_NOT_FOUND;
        } else if (fstat(fd, &st_buf) < 0) {
            return F_INTERNAL_ERROR;
        }

        is_dir = S_ISDIR(st_buf.st_mode);

        if (!S_ISREG(st_buf.st_mode) && !is_dir) {
            return F_FORBIDDEN;
        }

        if (!is_dir) {
            mimetype = get_url_mimetype(target_tmp);
            break;
        }

        memcpy(target_tmp + target_size, "/" INDEX_PAGE, sizeof("/" INDEX_PAGE));
        target_size += sizeof("/" INDEX_PAGE) - 1;
        close(fd);
    }

    file_meta->fd = fd;
    file_meta->is_directory = is_dir;
    file_meta->mime = mimetype;
    file_meta->size = st_buf.st_size;
    file_meta->inode = st_buf.st_ino;
    sprintf(file_meta->etag, "%ld-%ld", st_buf.st_mtim.tv_sec, st_buf.st_size);

    return F_EXISTS;
}


static inline enum conn_status
close_on_keep_alive(struct connection *conn)
{
    if (conn->keep_alive) {
        setup_read_io_step(&conn->steps, build_response);
        return C_RUN;
    }

    return C_CLOSE;
}


static void
build_http_status_step(enum http_status st, struct connection *conn,
                       const struct http_request *req)
{
    char *data;
    size_t content_length, size;

    content_length = strlen(http_status_str[st]) + HTTP_STATUS_FORMAT_SIZE;
    data = xmalloc(HEADERS_SIZE);

    size = sprintf(
        data,
        "HTTP/1.1 %d %s\r\n"
        "Server: rockepoll\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Length: %zu\r\n", st, http_status_str[st], content_length);

    if (conn->keep_alive) {
        size += sprintf(data + size, "Connection: keep-alive\r\n\r\n");
    } else {
        size += sprintf(data + size, "Connection: close\r\n\r\n");
    }

    size += sprintf(data + size, HTTP_STATUS_FORMAT, http_status_str[st]);

    setup_write_io_step(&conn->steps, data, 0, size, close_on_keep_alive);

    log_new_connection(conn, req, st, content_length);
}


void
init_handler(const char *conf_root_dir, int conf_chroot)
{
    xchdir(conf_root_dir);
    if (conf_chroot) {
        xchroot(conf_root_dir);
    }
}


enum conn_status
build_response(struct connection *conn)
{
    int st;
    char *data, *p;
    struct http_request req = {0};
    struct file_meta file_meta = {0};
    size_t lower, upper, content_length, size;

    st = parse_request(((struct read_meta *)conn->steps->meta)->data, &req);
    if (st) {
        build_http_status_step(S_BAD_REQUEST, conn, &req);
        return C_RUN;
    }

    if (req.method != M_GET && req.method != M_HEAD) {
        build_http_status_step(S_METHOD_NOT_ALLOWED, conn, &req);
        return C_RUN;
    }

    if (req.headers[H_CONNECTION] && !strcmp(req.headers[H_CONNECTION], "close")) {
        conn->keep_alive = 0;
    }

    if (*req.target == '\0') {
        req.target = ".";
    }

    switch (gather_file_meta(req.target, &file_meta)) {
    case F_FORBIDDEN:
        build_http_status_step(S_FORBIDDEN, conn, &req);
        return C_RUN;
        break;
    case F_NOT_FOUND:
        build_http_status_step(S_NOT_FOUND, conn, &req);
        return C_RUN;
        break;
    case F_INTERNAL_ERROR:
        build_http_status_step(S_INTERNAL_ERROR, conn, &req);
        return C_RUN;
        break;
    default:
        break;
    }

    if (file_meta.is_directory) {
        close(file_meta.fd);
        // TODO: create files listings
        build_http_status_step(S_NOT_FOUND, conn, &req);
        return C_RUN;
    }

    if (req.headers[H_IF_MATCH] && !strcmp(file_meta.etag, req.headers[H_IF_MATCH])) {
        build_http_status_step(S_NOT_MODIFIED, conn, &req);
        return C_RUN;
    }

    lower = 0;
    upper = file_meta.size - 1;
    content_length = file_meta.size;
    st = S_OK;
    if (req.headers[H_RANGE]) {
        data = req.headers[H_RANGE];

        if (strncmp(data, "bytes=", sizeof("bytes=") - 1)) {
            build_http_status_step(S_BAD_REQUEST, conn, &req);
            return C_RUN;
        }

        data += sizeof("bytes=") - 1;

        if (!(p = strchr(data, '-'))) {
            build_http_status_step(S_BAD_REQUEST, conn, &req);
            return C_RUN;
        }

        *(p++) = '\0';

        if (*data) {
            lower = strtoull(data, NULL, 10);
        }

        if (*p) {
            upper = strtoull(p, NULL, 10);
        }

        if (lower > upper) {
            build_http_status_step(S_RANGE_NOT_SATISFIABLE, conn, &req);
            return C_RUN;
        }

        upper = MIN(upper, file_meta.size - 1);

        content_length = upper - lower + 1;
        st = S_PARTIAL_CONTENT;
    }

    data = xmalloc(HEADERS_SIZE + (content_length < SENDFILE_MIN_SIZE) * content_length);

    size = sprintf(
        data,
        "HTTP/1.1 %d %s\r\n"
        "Server: rockepoll\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "ETag: \"%s\"\r\n"
        "Connection: %s\r\n",
        st, http_status_str[st], file_meta.mime,
        content_length, file_meta.etag,
        conn->keep_alive ? "keep-alive" : "close");

    if (st == S_PARTIAL_CONTENT) {
        size += sprintf(data + size,
                        "Content-Range: bytes %zu-%zu/%zu\r\n",
                        lower, upper, file_meta.size);
    }

    size += sprintf(data + size, "\r\n");

    if (req.method == M_GET) {
        if (content_length < SENDFILE_MIN_SIZE) {
            if (lower) {
                lseek(file_meta.fd, lower, SEEK_SET);
            }
            size += read(file_meta.fd, data + size, content_length);

            setup_write_io_step(&conn->steps,
                                data,
                                0, size,
                                close_on_keep_alive);
            close(file_meta.fd);
        } else {
            setup_write_io_step(&conn->steps, data, 1, size, NULL);
            setup_sendfile_io_step(&conn->steps,
                                   file_meta.fd, lower, upper + 1, content_length,
                                   close_on_keep_alive);
        }
    }

    log_new_connection(conn, &req, st, content_length);

    return C_RUN;
}