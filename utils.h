
#ifndef UTILS_H
#define UTILS_H


#define ALWAYS_INLINE inline __attribute__((always_inline))

#define UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#define LIKELY(expr) __builtin_expect(!!(expr), 1)

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