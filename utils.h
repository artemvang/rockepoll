
#ifndef UTILS_H
#define UTILS_H


#define ALWAYS_INLINE inline __attribute__((always_inline))

#define UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#define LIKELY(expr) __builtin_expect(!!(expr), 1)

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))


void *xmalloc(const size_t size);
void *xrealloc(void *original, const size_t size);

#endif