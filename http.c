#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#include "io.h"
#include "utils.h"
#include "http.h"
#include "utstring.h"


#define DEFAULT_MIMETYPE "application/octet-stream"
#define HTTP_STATUS_TEMPLATE "<h1>%s</h1>"
#define HTTP_STATUS_TEMPLATE_SIZE (sizeof(HTTP_STATUS_TEMPLATE) - 2 - 1)

#define HEADER_ENTRY(__enum, __str) \
    [__enum] = { .name=__str, .size=sizeof(__str) - 1 }


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


static const struct {
    char *name;
    size_t size;
} http_headers[] = {
    HEADER_ENTRY(H_RANGE,      "Range"),
    HEADER_ENTRY(H_CONNECTION, "Connection"),
    HEADER_ENTRY(H_IF_MATCH,   "If-Match"),
};


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


static enum http_status
get_file_stats(char *target, struct file_stats *st)
{
    struct stat st_buf;

    st->mime = get_url_mimetype(target);

    if ((st->fd = open(target, O_LARGEFILE | O_RDONLY | O_NONBLOCK)) < 0) {
        return (errno == EACCES) ? S_FORBIDDEN : S_NOT_FOUND;
    }

    if (stat(target, &st_buf) < 0) {
        return S_INTERNAL_ERROR;
    }

    if (S_ISDIR(st_buf.st_mode)) {
        return S_NOT_FOUND;
    }
    else if (!S_ISREG(st_buf.st_mode)) {
        return S_FORBIDDEN;
    }

    st->size = st_buf.st_size;
    sprintf(st->etag, "%ld-%ld", st_buf.st_mtim.tv_sec, st_buf.st_size);

    return S_OK;
}


static ALWAYS_INLINE char
decode_hex_digit(int ch)
{
    static const char hex_digit_tbl[256] = {
        ['0'] = 0,  ['1'] = 1,  ['2'] = 2,  ['3'] = 3,  ['4'] = 4,  ['5'] = 5,
        ['6'] = 6,  ['7'] = 7,  ['8'] = 8,  ['9'] = 9,  ['a'] = 10, ['b'] = 11,
        ['c'] = 12, ['d'] = 13, ['e'] = 14, ['f'] = 15, ['A'] = 10, ['B'] = 11,
        ['C'] = 12, ['D'] = 13, ['E'] = 14, ['F'] = 15,
    };

    return hex_digit_tbl[ch];
}


static void
url_decode(char *target)
{
    char *ch, *decoded;

    for (decoded = ch = target; *ch; ch++) {
        switch (*ch) {
        case '%':
            *decoded++ = decode_hex_digit(ch[1]) << 4 | decode_hex_digit(ch[2]);
            ch += 2;
            break;
        case '+':
            *decoded++ = ' ';
            break;
        default:
            *decoded++ = *ch;
            break;
        }
    }

    *decoded = '\0';
}


static enum http_status
parse_request(char *raw_content, struct http_request *r)
{
    int i;
    char *q, *p = raw_content;

    if (strncmp(p, "GET", sizeof("GET") - 1)) {
        return S_METHOD_NOT_ALLOWED;
    }

    p += sizeof("GET") - 1;

    if (UNLIKELY(*(p++) != ' ')) {
        return S_BAD_REQUEST;
    }

    /* skip / */
    p++;
    if (UNLIKELY(!(q = strchr(p, ' ')))) {
        return S_BAD_REQUEST;
    }

    *(q++) = '\0';
    r->target = p;
    url_decode(r->target);

    p = q;

    /* HTTP-VERSION */
    if (UNLIKELY(strncmp(p, "HTTP/1.", sizeof("HTTP/1.") - 1))) {
        return S_BAD_REQUEST;
    }

    p += sizeof("HTTP/1.") - 1;
    if (UNLIKELY(*p != '1' && *p != '0')) {
        return S_VERSION_NOT_SUPPORTED;
    }

    p++;

    /* check terminator */
    if (UNLIKELY(strncmp(p, "\r\n", sizeof("\r\n") - 1))) {
        return S_BAD_REQUEST;
    }

    p += sizeof("\r\n") - 1;

    while (strncmp(p, "\r\n", sizeof("\r\n") - 1)) {
        for (i = 0; i < HEADERS_COUNT; i++) {
            if (!strncmp(p, http_headers[i].name, http_headers[i].size)) {
                break;
            }
        }

        if (i == HEADERS_COUNT) {
            if (UNLIKELY(!(q = strchr(p, '\r')))) {
                return S_BAD_REQUEST;
            }

            p = q + 2;
            continue;
        }

        p += http_headers[i].size;

        /* a single colon must follow the field name */
        if (UNLIKELY(*p != ':')) {
            return S_BAD_REQUEST;
        }

        /* skip whitespace */
        for (++p; *p == ' ' || *p == '\t'; p++) ;

        /* extract field content */
        if (UNLIKELY(!(q = strchr(p, '\r')))) {
            return S_BAD_REQUEST;
        }

        *q = '\0';
        r->headers[i] = p;

        /* go to next line */
        p = q + 2;
    }

    return S_OK;
}


static enum conn_status
close_on_keep_alive(struct connection *conn)
{
    if (conn->keep_alive) {
        setup_read_io_step(conn, build_response);
        return C_CLOSE;
    }

    return C_RUN;
}


static void
build_http_status_step(enum http_status status, struct connection *conn)
{
    UT_string *str;
    utstring_new(str);
    utstring_reserve(str, 256);

    utstring_printf(str,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Length: %lu\r\n",
        status, http_status_str[status],
        strlen(http_status_str[status]) + HTTP_STATUS_TEMPLATE_SIZE);

    if (conn->keep_alive) {
        utstring_printf(str, "Connection: keep-alive\r\n");
    } else {
        utstring_printf(str, "Connection: close\r\n");
    }

    utstring_printf(str, "\r\n");
    utstring_printf(str, HTTP_STATUS_TEMPLATE, http_status_str[status]);

    setup_send_io_step(conn, str, close_on_keep_alive);
}


enum conn_status
build_response(struct connection *conn)
{
    UT_string *str;
    off_t lower, upper, content_length;
    char *p, *q;
    struct file_stats st = {0};
    enum http_status resp_status;
    struct http_request r = {0};
    struct read_meta *req;

    req = conn->steps->meta;

    resp_status = parse_request(req->data, &r);
    if (UNLIKELY(resp_status != S_OK)) {
        build_http_status_step(resp_status, conn);
        return C_RUN;
    }

    if (r.headers[H_CONNECTION] && !strcmp(r.headers[H_CONNECTION], "close")) {
        conn->keep_alive = 0;
    }

    resp_status = get_file_stats(r.target, &st);
    if (resp_status != S_OK) {
        build_http_status_step(resp_status, conn);
        return C_RUN;
    }

    if (r.headers[H_IF_MATCH] && !strcmp(st.etag, r.headers[H_IF_MATCH])) {
        build_http_status_step(S_NOT_MODIFIED, conn);
        return C_RUN;
    }

    lower = 0;
    upper = st.size - 1;
    content_length = st.size;
    resp_status = S_OK;
    if (r.headers[H_RANGE]) {
        p = r.headers[H_RANGE];

        if (UNLIKELY(strncmp(p, "bytes=", sizeof("bytes=") - 1))) {
            build_http_status_step(S_BAD_REQUEST, conn);
            return C_RUN;
        }

        p += sizeof("bytes=") - 1;

        if (UNLIKELY(!(q = strchr(p, '-')))) {
            build_http_status_step(S_BAD_REQUEST, conn);
            return C_RUN;
        }

        *(q++) = '\0';

        if (*p) {
            lower = strtoull(p, NULL, 10);
        }

        if (*q) {
            upper = strtoull(q, NULL, 10);
        }

        if (UNLIKELY(lower < 0 || upper < 0 || lower > upper)) {
            build_http_status_step(S_RANGE_NOT_SATISFIABLE, conn);
            return C_RUN;
        }

        upper = MIN(upper, st.size - 1);

        content_length = upper - lower + 1;
        resp_status = S_PARTIAL_CONTENT;
    }

    utstring_new(str);
    utstring_reserve(str, 256);

    utstring_printf(str,
            "HTTP/1.1 %d %s\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "ETag: \"%s\"\r\n",
            resp_status, http_status_str[resp_status],
            st.mime,
            (long long)content_length,
            st.etag);

    if (resp_status == S_PARTIAL_CONTENT) {
        utstring_printf(str,
            "Content-Range: bytes %lld-%lld/%lld\r\n",
            (long long)lower,
            (long long)upper,
            (long long)st.size);
    }

    if (conn->keep_alive) {
        utstring_printf(str, "Connection: keep-alive\r\n");
    } else {
        utstring_printf(str, "Connection: close\r\n");
    }

    utstring_printf(str, "\r\n");

    setup_send_io_step(conn, str, NULL);
    setup_sendfile_io_step(conn, st.fd, lower, upper + 1, content_length, close_on_keep_alive);

    return C_RUN;
}