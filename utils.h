
#ifndef UTILS_H
#define UTILS_H

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))


#if defined(__GNUC__) || defined(__INTEL_COMPILER)
# define unused __attribute__((__unused__))
#else
# define unused
#endif


void *xmalloc(const size_t size);
void *xrealloc(void *original, const size_t size);

#endif