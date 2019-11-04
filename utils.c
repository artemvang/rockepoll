#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <err.h>

#include "utils.h"


inline void *
xmalloc(const size_t size)
{
    void *ptr = malloc(size);
    if (UNLIKELY(!ptr)) {
        err(1, "malloc(), can't allocate %zu bytes", size);
    }
    return ptr;
}


inline void *
xrealloc(void *original, const size_t size)
{
    void *ptr = realloc(original, size);
    if (UNLIKELY(!ptr)) {
        err(1, "realloc(), can't reallocate %zu bytes", size);
    }
    return ptr;
}