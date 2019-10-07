#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/socket.h>

#include "io.h"
#include "utils.h"
#include "http.h"
#include "murmurhash.h"

#define DEFAULT_MIMETYPE "application/octet-stream"

#define HEADER_ENTRY(__enum, __str) \
    [__enum] = {.name=__str, .size=sizeof(__str) - 1}

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
    unsigned long hs;

    st->mime = get_url_mimetype(target);

    if ((st->fd = open(target, O_RDONLY | O_NONBLOCK)) < 0) {
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
    hs = murmurhash((char *)&st_buf, sizeof(st_buf), 1337);

    sprintf(st->hash, "%lu", hs);

    return S_OK;
}


static inline __attribute__((always_inline)) char
decode_hex_digit(char ch)
{
    static const char hex_digit_tbl[256] = {
        ['0'] = 0,  ['1'] = 1,  ['2'] = 2,  ['3'] = 3,  ['4'] = 4,  ['5'] = 5,
        ['6'] = 6,  ['7'] = 7,  ['8'] = 8,  ['9'] = 9,  ['a'] = 10, ['b'] = 11,
        ['c'] = 12, ['d'] = 13, ['e'] = 14, ['f'] = 15, ['A'] = 10, ['B'] = 11,
        ['C'] = 12, ['D'] = 13, ['E'] = 14, ['F'] = 15,
    };

    return hex_digit_tbl[(unsigned char)ch];
}


static void
url_decode(char *target)
{
    char tmp;
    char *ch, *decoded;

    for (decoded = ch = target; *ch; ch++) {
        if (*ch == '%') {
            tmp = (char)(decode_hex_digit(ch[1]) << 4 | decode_hex_digit(ch[2]));

            *decoded++ = tmp;
            ch += 2;
        } else if (*ch == '+') {
            *decoded++ = ' ';
        } else {
            *decoded++ = *ch;
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

    if (unlikely(*(p++) != ' ')) {
        return S_BAD_REQUEST;
    }

    /* skip / */
    p++;
    if (unlikely(!(q = strchr(p, ' ')))) {
        return S_BAD_REQUEST;
    }

    *(q++) = '\0';
    r->target = p;
    url_decode(r->target);

    p = q;

    /* HTTP-VERSION */
    if (unlikely(strncmp(p, "HTTP/1.", sizeof("HTTP/1.") - 1))) {
        return S_BAD_REQUEST;
    }

    p += sizeof("HTTP/1.") - 1;
    if (unlikely(*p != '1' && *p != '0')) {
        return S_VERSION_NOT_SUPPORTED;
    }

    p++;

    /* check terminator */
    if (unlikely(strncmp(p, "\r\n", sizeof("\r\n") - 1))) {
        return S_BAD_REQUEST;
    }

    p += sizeof("\r\n") - 1;

    while (unlikely(strncmp(p, "\r\n", sizeof("\r\n") - 1))) {
        for (i = 0; i < HEADERS_COUNT; i++) {
            if (!strncmp(p, http_headers[i].name, http_headers[i].size)) {
                break;
            }
        }

        if (i == HEADERS_COUNT) {
            if (unlikely(!(q = strchr(p, '\r')))) {
                return S_BAD_REQUEST;
            }

            p = q + 2;
            continue;
        }

        p += http_headers[i].size;

        /* a single colon must follow the field name */
        if (unlikely(*p != ':')) {
            return S_BAD_REQUEST;
        }

        /* skip whitespace */
        for (++p; *p == ' ' || *p == '\t'; p++) ;

        /* extract field content */
        if (unlikely(!(q = strchr(p, '\r')))) {
            return S_BAD_REQUEST;
        }

        *q = '\0';
        r->headers[i] = p;

        /* go to next line */
        p = q + 2;
    }

    return S_OK;
}


static void
build_http_status_step(enum http_status status, struct connection *conn)
{
    char *data;
    size_t wrote_size;

    wrote_size = asprintf(&data,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-Length: %d\r\n"
            "%s"
            "\r\n"
            "<h1>%s</h1>",
            status, http_status_str[status],
            (int)strlen(http_status_str[status]) + 9,
            (conn->keep_alive) ? "Connection: keep-alive\r\n" : "Connection: close\r\n",
            http_status_str[status]
    );

    setup_send_io_step(conn, data, wrote_size);
}


void
build_response(struct connection *conn)
{
    size_t wrote_size;
    off_t lower, upper, content_length;
    char *p, *q;
    char content_range[128] = {0};
    struct file_stats st = {0};
    enum http_status resp_status;
    struct http_request r = {0};
    struct read_meta *req;

    req = conn->steps->meta;

    resp_status = parse_request(req->data, &r);
    if (unlikely(resp_status != S_OK)) {
        build_http_status_step(resp_status, conn);
        return;
    }

    if (r.headers[H_CONNECTION] && !strcmp(r.headers[H_CONNECTION], "close")) {
        conn->keep_alive = 0;
    }

    resp_status = get_file_stats(r.target, &st);
    if (resp_status != S_OK) {
        build_http_status_step(resp_status, conn);
        return;
    }

    if (r.headers[H_IF_MATCH] && !strcmp(st.hash, r.headers[H_IF_MATCH])) {
        build_http_status_step(S_NOT_MODIFIED, conn);
        return;
    }

    lower = 0;
    upper = st.size - 1;
    content_length = st.size;
    resp_status = S_OK;
    if (r.headers[H_RANGE]) {
        p = r.headers[H_RANGE];

        if (unlikely(strncmp(p, "bytes=", sizeof("bytes=") - 1))) {
            build_http_status_step(S_BAD_REQUEST, conn);
            return;
        }

        p += sizeof("bytes=") - 1;

        if (unlikely(!(q = strchr(p, '-')))) {
            build_http_status_step(S_BAD_REQUEST, conn);
            return;
        }

        *(q++) = '\0';

        if (*p) {
            lower = strtoull(p, NULL, 10);
        }

        if (*q) {
            upper = strtoull(q, NULL, 10);
        }

        if (unlikely(lower < 0 || upper < 0 || lower > upper)) {
            build_http_status_step(S_RANGE_NOT_SATISFIABLE, conn);
            return;
        }

        upper = MIN(upper, st.size - 1);

        content_length = upper - lower + 1;
        resp_status = S_PARTIAL_CONTENT;
    }

    if (resp_status == S_PARTIAL_CONTENT) {
        sprintf(content_range,
                "Content-Range: bytes %lld-%lld/%lld\r\n",
                (long long)lower,
                (long long)upper,
                (long long)st.size
        );
    }

    wrote_size = asprintf(&p,
            "HTTP/1.1 %d %s\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "ETag: \"%s\"\r\n"
            "%s"
            "%s"
            "\r\n",
            resp_status, http_status_str[resp_status],
            st.mime,
            (long long)content_length,
            st.hash,
            content_range,
            (conn->keep_alive) ? "Connection: keep-alive\r\n" : "Connection: close\r\n"
    );

    setup_send_io_step(conn, p, wrote_size);
    setup_sendfile_io_step(conn, st.fd, lower, upper + 1, content_length);
}