#include <stdlib.h>
#include <err.h>

#include "utils.h"


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
