
#ifndef PARSER_H
#define PARSER_H


#define MAX_TARGET_SIZE 1024 * 4

#define MAPPING_ENTRY(enm, str)                                               \
    [enm] = { .name=str, .size=sizeof(str) - 1 }

#define ENUM_MAPPING(s_name)                                                  \
    static const struct {                                                     \
        char *name;                                                           \
        size_t size;                                                          \
    } s_name[] =

enum http_header {
    H_RANGE,
    H_IF_MATCH,
    H_CONNECTION,
    H_USER_AGENT,
    H_ACCEPT_ENCODING,
    HEADERS_COUNT,
};

enum http_method {M_GET, M_POST, M_OPTIONS, M_DELETE, M_HEAD, M_PATCH, HTTP_METHODS_COUNT};
enum http_version {V10, V11, V20};

struct http_request {
    enum http_method method;
    enum http_version version;
    char *target;
    char *headers[HEADERS_COUNT];
};


ENUM_MAPPING(http_headers) {
    MAPPING_ENTRY(H_RANGE,      "Range"),
    MAPPING_ENTRY(H_CONNECTION, "Connection"),
    MAPPING_ENTRY(H_IF_MATCH,   "If-Match"),
    MAPPING_ENTRY(H_USER_AGENT, "User-Agent"),
    MAPPING_ENTRY(H_ACCEPT_ENCODING, "Accept-Encoding")
};


ENUM_MAPPING(http_methods) {
    MAPPING_ENTRY(M_GET,     "GET"),
    MAPPING_ENTRY(M_POST,    "POST"),
    MAPPING_ENTRY(M_DELETE,  "DELETE"),
    MAPPING_ENTRY(M_OPTIONS, "OPTIONS"),
    MAPPING_ENTRY(M_HEAD,    "HEAD"),
    MAPPING_ENTRY(M_PATCH,    "PATCH")
};


ENUM_MAPPING(http_versions) {
    MAPPING_ENTRY(V10, "1.0"),
    MAPPING_ENTRY(V11, "1.1"),
    MAPPING_ENTRY(V20, "1.2"),
};


int parse_request(char *data, struct http_request *r);

#endif