#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#include "../io.h"
#include "../utils.h"
#include "../log.h"
#include "handler.h"
#include "parser.h"


#define ETAG_SIZE 64
#define DEFAULT_MIMETYPE "application/octet-stream"
#define HTTP_STATUS_TEMPLATE "<h1>%s</h1>"
#define HTTP_STATUS_TEMPLATE_SIZE (sizeof(HTTP_STATUS_TEMPLATE) - 2 - 1)


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

struct file_stats {
    int fd;
    char *mime;
    size_t size;
    char etag[ETAG_SIZE];
};


static const struct {
    char *ext;
    char *type;
} mimes[] = {
    { "xml",   "application/xml; charset=utf-8" },
    { "xhtml", "application/xhtml+xml; charset=utf-8" },
    { "html",  "text/html; charset=utf-8" },
    { "htm",   "text/html; charset=utf-8" },
    { "css",   "text/css; charset=utf-8" },
    { "txt",   "text/plain; charset=utf-8" },
    { "md",    "text/plain; charset=utf-8" },
    { "c",     "text/plain; charset=utf-8" },
    { "h",     "text/plain; charset=utf-8" },
    { "gz",    "application/x-gtar" },
    { "tar",   "application/tar" },
    { "pdf",   "application/pdf" },
    { "png",   "image/png" },
    { "gif",   "image/gif" },
    { "jpeg",  "image/jpg" },
    { "jpg",   "image/jpg" },
    { "iso",   "application/x-iso9660-image" },
    { "webp",  "image/webp" },
    { "svg",   "image/svg+xml; charset=utf-8" },
    { "flac",  "audio/flac" },
    { "mp3",   "audio/mpeg" },
    { "ogg",   "audio/ogg" },
    { "mp4",   "video/mp4" },
    { "ogv",   "video/ogg" },
    { "webm",  "video/webm" },
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
log_new_connection(struct connection *conn, struct http_request *r, enum http_status status)
{
    char *user_agent;

    if (status != S_BAD_REQUEST) {
        user_agent = r->headers[H_USER_AGENT];
        log_log("%s %ld \"%s /%s HTTP/%s\" %d \"%s\"\n",
            conn->ip, conn->last_active,
            http_methods[r->method].name, r->target, http_versions[r->version].name,
            status, (user_agent) ? user_agent : "");
    } else {
        log_log("%s %ld \"-\" %d \"-\"\n", conn->ip, conn->last_active, status);
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
gather_file_stats(char *target, struct file_stats *st)
{
    struct stat st_buf;


    if ((st->fd = open(target, O_LARGEFILE | O_RDONLY | O_NONBLOCK)) < 0) {
        return (errno == EACCES) ? F_FORBIDDEN : F_NOT_FOUND;
    }


    if (UNLIKELY(stat(target, &st_buf) < 0)) {
        return F_INTERNAL_ERROR;
    }

    if (S_ISDIR(st_buf.st_mode)) {
        return F_NOT_FOUND;
    }
    else if (!S_ISREG(st_buf.st_mode)) {
        return F_FORBIDDEN;
    }

    st->size = st_buf.st_size;
    st->mime = get_url_mimetype(target);
    sprintf(st->etag, "%ld-%ld", st_buf.st_mtim.tv_sec, st_buf.st_size);

    return F_EXISTS;
}


static enum conn_status
close_on_keep_alive(struct connection *conn)
{
    if (conn->keep_alive) {
        setup_read_io_step(conn, IO_FLAG_NONE, build_response);
        return C_RUN;
    }

    return C_CLOSE;
}


static void
build_http_status_step(enum http_status status, struct connection *conn, struct http_request *r)
{
    size_t content_length, size;
    char *data;

    content_length = strlen(http_status_str[status]) + HTTP_STATUS_TEMPLATE_SIZE;
    data = xmalloc(256);
    size = 0;

    size += sprintf(data + size, "HTTP/1.1 %d %s\r\n", status, http_status_str[status]);
    size += sprintf(data + size, "Server: rockepoll\r\n");
    size += sprintf(data + size, "Accept-Ranges: bytes\r\n");
    size += sprintf(data + size, "Content-Length: %lu\r\n", content_length);
    if (conn->keep_alive) {
        size += sprintf(data + size, "Connection: keep-alive\r\n\r\n");
    } else {
        size += sprintf(data + size, "Connection: close\r\n\r\n");
    }

    size += sprintf(data + size, HTTP_STATUS_TEMPLATE, http_status_str[status]);

    setup_send_io_step(conn,
                       IO_FLAG_NONE,
                       data, size,
                       close_on_keep_alive);

    log_new_connection(conn, r, status);
}


enum conn_status
build_response(struct connection *conn)
{
    size_t lower, upper, content_length, size;
    char *p, *q;
    struct file_stats st = {0};
    int status;
    struct http_request r = {0};
    struct read_meta *req;

    req = conn->steps->meta;

    status = parse_request(req->data, &r);
    if (UNLIKELY(status)) {
        build_http_status_step(S_BAD_REQUEST, conn, &r);
        return C_RUN;
    }

    if (r.method != M_GET) {
        build_http_status_step(S_METHOD_NOT_ALLOWED, conn, &r);
        return C_RUN;
    }

    if (r.headers[H_CONNECTION] && !strcmp(r.headers[H_CONNECTION], "close")) {
        conn->keep_alive = 0;
    }

    switch(gather_file_stats(r.target, &st)) {
    case F_FORBIDDEN:
        build_http_status_step(S_FORBIDDEN, conn, &r);
        return C_RUN;
        break;
    case F_NOT_FOUND:
        build_http_status_step(S_NOT_FOUND, conn, &r);
        return C_RUN;
        break;
    case F_INTERNAL_ERROR:
        build_http_status_step(S_INTERNAL_ERROR, conn, &r);
        return C_RUN;
        break;
    default:
        break;
    }

    if (r.headers[H_IF_MATCH] && !strcmp(st.etag, r.headers[H_IF_MATCH])) {
        build_http_status_step(S_NOT_MODIFIED, conn, &r);
        return C_RUN;
    }

    lower = 0;
    upper = st.size - 1;
    content_length = st.size;
    status = S_OK;
    if (r.headers[H_RANGE]) {
        p = r.headers[H_RANGE];

        if (UNLIKELY(strncmp(p, "bytes=", sizeof("bytes=") - 1))) {
            build_http_status_step(S_BAD_REQUEST, conn, &r);
            return C_RUN;
        }

        p += sizeof("bytes=") - 1;

        if (UNLIKELY(!(q = strchr(p, '-')))) {
            build_http_status_step(S_BAD_REQUEST, conn, &r);
            return C_RUN;
        }

        *(q++) = '\0';

        if (*p) {
            lower = strtoull(p, NULL, 10);
        }

        if (*q) {
            upper = strtoull(q, NULL, 10);
        }

        if (UNLIKELY(lower > upper)) {
            build_http_status_step(S_RANGE_NOT_SATISFIABLE, conn, &r);
            return C_RUN;
        }

        upper = MIN(upper, st.size - 1);

        content_length = upper - lower + 1;
        status = S_PARTIAL_CONTENT;
    }

    p = xmalloc(256);
    size = 0;

    size += sprintf(p + size, "HTTP/1.1 %d %s\r\n", status, http_status_str[status]);
    size += sprintf(p + size, "Server: rockepoll\r\n");
    size += sprintf(p + size, "Accept-Ranges: bytes\r\n");
    size += sprintf(p + size, "Content-Type: %s\r\n", st.mime);
    size += sprintf(p + size, "Content-Length: %lu\r\n", content_length);
    size += sprintf(p + size, "ETag: \"%s\"\r\n", st.etag);

    if (status == S_PARTIAL_CONTENT) {
        size += sprintf(p + size,
            "Content-Range: bytes %lu-%lu/%lu\r\n",
            lower, upper, st.size);
    }

    if (conn->keep_alive) {
        size += sprintf(p + size, "Connection: keep-alive\r\n\r\n");
    } else {
        size += sprintf(p + size, "Connection: close\r\n\r\n");
    }

    setup_send_io_step(conn,
                       IO_FLAG_SEND_CORK,
                       p, size, NULL);


    setup_sendfile_io_step(conn,
                           IO_FLAG_NONE,
                           st.fd, lower, upper + 1, content_length, close_on_keep_alive);

    log_new_connection(conn, &r, status);

    return C_RUN;
}