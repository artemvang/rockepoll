#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <err.h>

#include "utils.h"


char *argv0;

/* global parameters from command line */
int   logfd        = -1;
int   port         = 7887;
int   keep_alive   = 0;
char *listen_addr  = "127.0.0.1";


void *
xmalloc(const size_t size)
{
    void *ptr = malloc(size);
    if (!ptr) {
        errx(1, "malloc(), can't allocate %zu bytes", size);
    }
    return ptr;
}


void *
xrealloc(void *original, const size_t size)
{
    void *ptr = realloc(original, size);
    if (!ptr) {
        errx(1, "realloc(), can't reallocate %zu bytes", size);
    }
    return ptr;
}


static void
prepare_logfile(const char *logfile)
{
    if (logfile) {
        logfd = open(logfile, O_RDWR | O_CREAT);
        if (logfd < 0) {
            errx(1, "open(), file %s", logfile);
        }

        chmod(logfile, S_IRUSR|S_IWUSR|S_IRGRP);
    } else {
        logfd = -1;
    }
}


void
parse_args(int argc, char *argv[])
{
    int i;
    char *logfile = NULL;

    argv0 = argv[0];

    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage();
    }

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port")) {
            if (++i >= argc) {
                errx(1, "missing number after --port");
            }
            port = atoi(argv[i]);
        }
        else if (!strcmp(argv[i], "--addr")) {
            if (++i >= argc) {
                errx(1, "missing ip after --addr");
            }
            listen_addr = argv[i];
        }
        else if (!strcmp(argv[i], "--log")) {
            if (++i >= argc) {
                errx(1, "missing filename after --log");
            }
            logfile = argv[i];
        }
        else if (!strcmp(argv[i], "--keepalive")) {
            keep_alive = 1;
        }
        else {
            errx(1, "unknown argument `%s'", argv[i]);
        }
    }

    prepare_logfile(logfile);

    printf("listening on http://%s:%d/\n", listen_addr, port);
}


void
usage()
{
    printf("usage: %s [--addr addr] [--port port] [--log file] [--keepalive]\n", argv0);
    exit(0);
}