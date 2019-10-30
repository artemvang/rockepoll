#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <err.h>

#include "log.h"


static int quiet = 0;


void
log_setup(int quiet_mode)
{
    quiet = quiet_mode;
}


void
log_log(const char *format, ...)
{
    va_list va;

    if (!quiet) {
        va_start(va, format);
        vprintf(format, va);
        va_end(va);
    }
}