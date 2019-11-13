
#ifndef HANDLER_H
#define HANDLER_H

#include "io.h"

enum conn_status build_response(struct connection *conn);
void init_handler(const char *root_dir);

#endif
