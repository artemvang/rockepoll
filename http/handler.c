#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../io.h"
#include "../utils.h"
#include "../log.h"
#include "handler.h"
#include "parser.h"


#define ETAG_SIZE 64
#define INDEX_PAGE "index.html"
#define DEFAULT_MIMETYPE "application/octet-stream"
#define HTTP_STATUS_FORMAT "<h1>%s</h1>"
#define HTTP_STATUS_FORMAT_SIZE (sizeof(HTTP_STATUS_FORMAT) - 2 - 1)
#define LOG_MESSAGE_FORMAT "%s [%s] \"%s\" %d %lu \"%s\"\n"
#define REQUEST_LINE_FORMAT "%s /%s HTTP/%s"
#define TIMESTAMP_FORMAT "%a, %d/%b/%Y %H:%M:%S GMT"


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
    { "vtt",   "text/plain; charset=utf-8" },
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
log_new_connection(const struct connection *conn,
                   const struct http_request *req,
                   enum http_status status,
                   size_t content_lenght)
{
    struct tm *tm;
    char timestamp[32] = {0};
    char *user_agent = "-";
    char *request_line = "-";

    if (LIKELY(status != S_BAD_REQUEST)) {
        if (req->headers[H_USER_AGENT]) {
            user_agent = req->headers[H_USER_AGENT];
        }

        request_line = xmalloc(strlen(req->target) + 32);
        sprintf(request_line, REQUEST_LINE_FORMAT,
                http_methods[req->method].name,
                req->target,
                http_versions[req->version].name);
    }

    tm = gmtime(&conn->last_active);
    strftime(timestamp, sizeof(timestamp), TIMESTAMP_FORMAT, tm);

    log_log(LOG_MESSAGE_FORMAT,
            conn->ip, timestamp, request_line,
            status, content_lenght,user_agent);

    if (LIKELY(status != S_BAD_REQUEST)) {
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
    struct stat st_buf;


    if ((file_meta->fd = open(target, O_LARGEFILE | O_RDONLY | O_NONBLOCK)) < 0) {
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

    file_meta->size = st_buf.st_size;
    file_meta->mime = get_url_mimetype(target);
    sprintf(file_meta->etag, "%ld-%ld", st_buf.st_mtim.tv_sec, st_buf.st_size);

    return F_EXISTS;
}


static inline enum conn_status
close_on_keep_alive(struct connection *conn)
{
    if (conn->keep_alive) {
        setup_read_io_step(&(conn->steps), build_response);
        return C_RUN;
    }

    return C_CLOSE;
}


static void
build_http_status_step(enum http_status st, struct connection *conn, const struct http_request *req)
{
    char *data;
    size_t content_length, size;

    content_length = strlen(http_status_str[st]) + HTTP_STATUS_FORMAT_SIZE;
    data = xmalloc(256);
    size = 0;

    size += sprintf(data + size, "HTTP/1.1 %d %s\r\n", st, http_status_str[st]);
    size += sprintf(data + size, "Server: rockepoll\r\n");
    size += sprintf(data + size, "Accept-Ranges: bytes\r\n");
    size += sprintf(data + size, "Content-Length: %lu\r\n", content_length);
    if (conn->keep_alive) {
        size += sprintf(data + size, "Connection: keep-alive\r\n\r\n");
    } else {
        size += sprintf(data + size, "Connection: close\r\n\r\n");
    }

    size += sprintf(data + size, HTTP_STATUS_FORMAT, http_status_str[st]);

    setup_write_io_step(&(conn->steps), data, 0, size, close_on_keep_alive);

    log_new_connection(conn, req, st, content_length);
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
    if (UNLIKELY(st)) {
        build_http_status_step(S_BAD_REQUEST, conn, &req);
        return C_RUN;
    }

    if (req.method != M_GET) {
        build_http_status_step(S_METHOD_NOT_ALLOWED, conn, &req);
        return C_RUN;
    }

    if (req.headers[H_CONNECTION] && !strcmp(req.headers[H_CONNECTION], "close")) {
        conn->keep_alive = 0;
    }

    if (*req.target == '\0') {
        req.target = INDEX_PAGE;
    }

    switch(gather_file_meta(req.target, &file_meta)) {
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

        if (UNLIKELY(strncmp(data, "bytes=", sizeof("bytes=") - 1))) {
            build_http_status_step(S_BAD_REQUEST, conn, &req);
            return C_RUN;
        }

        data += sizeof("bytes=") - 1;

        if (UNLIKELY(!(p = strchr(data, '-')))) {
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

        if (UNLIKELY(lower > upper)) {
            build_http_status_step(S_RANGE_NOT_SATISFIABLE, conn, &req);
            return C_RUN;
        }

        upper = MIN(upper, file_meta.size - 1);

        content_length = upper - lower + 1;
        st = S_PARTIAL_CONTENT;
    }

    data = xmalloc(256);
    size = 0;

    size += sprintf(data + size, "HTTP/1.1 %d %s\r\n", st, http_status_str[st]);
    size += sprintf(data + size, "Server: rockepoll\r\n");
    size += sprintf(data + size, "Accept-Ranges: bytes\r\n");
    size += sprintf(data + size, "Content-Type: %s\r\n", file_meta.mime);
    size += sprintf(data + size, "Content-Length: %lu\r\n", content_length);
    size += sprintf(data + size, "ETag: \"%s\"\r\n", file_meta.etag);

    if (st == S_PARTIAL_CONTENT) {
        size += sprintf(data + size, "Content-Range: bytes %lu-%lu/%lu\r\n", lower, upper, file_meta.size);
    }

    if (conn->keep_alive) {
        size += sprintf(data + size, "Connection: keep-alive\r\n\r\n");
    } else {
        size += sprintf(data + size, "Connection: close\r\n\r\n");
    }

    setup_write_io_step(&(conn->steps), data, 1, size, NULL);
    setup_sendfile_io_step(&(conn->steps),
                           file_meta.fd, lower, upper + 1, content_length, close_on_keep_alive);

    log_new_connection(conn, &req, st, content_length);

    return C_RUN;
}