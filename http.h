
#ifndef HANDLER_H
#define HANDLER_H

#include "io.h"


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


enum http_header {
    H_RANGE,
    H_IF_MATCH,
    H_CONNECTION,
    HEADERS_COUNT,
};


struct http_request {
    char *target;

    char *headers[HEADERS_COUNT];
};


struct file_stats {
    int fd;
    char *mime;
    off_t size;
    char etag[64];
};


void build_response(struct connection *conn);


#endif