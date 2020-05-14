#include <string.h>

#include "utils.h"
#include "parser.h"


#define ENDS(c) ((c) == '/' || (c) == '\0')


static ALWAYS_INLINE char
decode_hex(int ch)
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
decode_target(char *target)
{
    char *ch, *decoded;

    for (decoded = ch = target; *ch; ch++) {
        switch (*ch) {
        case '%':
            *decoded++ = decode_hex(ch[1]) << 4 | decode_hex(ch[2]);
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

static char *
remove_target_dots(char *url)
{
    char *src = url, *dst;

    for (; *src && *src != '?'; ++src) {
        if (*src == '/') {
            if (src[1] == '/') {
                break;
            }
            else if (src[1] == '.') {
                if (ENDS(src[2])) {
                    break;
                }
                else if (src[2] == '.' && ENDS(src[3])) {
                    break;
                }
            }
        }
    }

    dst = src;
    while (*src && *src != '?') {
        if (*src != '/') {
            *dst++ = *src++;
        }
        else if (*++src == '/') {
            ;
        }
        else if (*src != '.') {
            *dst++ = '/';
        }
        else if (ENDS(src[1])) {
            ++src;
        }
        else if (src[1] == '.' && ENDS(src[2])) {
            src += 2;
            if (dst == url) {
                return NULL;
            } else {
                while (*--dst != '/' && dst > url);
            }
        } else {
            *dst++ = '/';
        }
    }

    if (dst == url) {
        ++dst;
    }
    *dst = '\0';

    return url;
}


int
parse_request(char *data, struct http_request *req)
{
    int i;
    char *q, *p = data;

    for (i = M_GET; i < HTTP_METHODS_COUNT; i++) {
        if (!strncmp(p, http_methods[i].name, http_methods[i].size)) {
            req->method = i;
            p += http_methods[i].size;
            break;
        }
    }

    if (i == HTTP_METHODS_COUNT) {
        return -1;
    }

    if (*p++ != ' ') {
        return -1;
    }

    if (!(q = strchr(p, ' '))) {
        return -1;
    }
    if (q - p >= MAX_TARGET_SIZE) {
        return -1;
    }

    *q++ = '\0';

    decode_target(p);
    if (!(p = remove_target_dots(p))) {
        return -1;
    }
    // skip /
    req->target = ++p;

    p = q;

    /* HTTP-VERSION */
    if (strncmp(p, "HTTP/", sizeof("HTTP/") - 1)) {
        return -1;
    }

    p += sizeof("HTTP/") - 1;

    switch (*p++) {
    case '1':
        if (*p++ == '.') {
            switch(*p++) {
            case '0':
                req->version = V10;
                break;
            case '1':
                req->version = V11;
                break;
            default:
                return -1;
                break;
            }
        } else {
            return -1;
        }
        break;
    case '2':
        if (*p++ == '.') {
            switch(*p++) {
            case '0':
                req->version = V20;
                break;
            default:
                return -1;
                break;
            }
        } else {
            return -1;
        }
        break;
    default:
        return -1;
        break;
    }

    /* check terminator */
    if (strncmp(p, "\r\n", sizeof("\r\n") - 1)) {
        return -1;
    }

    p += sizeof("\r\n") - 1;

    while (strncmp(p, "\r\n", sizeof("\r\n") - 1)) {
        for (i = 0; i < HEADERS_COUNT; i++) {
            if (!strncmp(p, http_headers[i].name, http_headers[i].size)) {
                p += http_headers[i].size;
                break;
            }
        }

        if (i == HEADERS_COUNT) {
            if (!(q = strchr(p, '\r'))) {
                return -1;
            }

            p = q + 2;
            continue;
        }

        /* a single colon must follow the field name */
        if (*p != ':') {
            return -1;
        }

        /* skip whitespace */
        for (++p; *p == ' ' || *p == '\t'; p++) ;

        /* extract field content */
        if (!(q = strchr(p, '\r'))) {
            return -1;
        }

        *q = '\0';
        req->headers[i] = p;

        /* go to next line */
        p = q + sizeof("\r\n") - 1;
    }

    return 0;
}
