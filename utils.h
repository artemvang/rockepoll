
#ifndef UTILS_H
#define UTILS_H


#define ALWAYS_INLINE inline __attribute__((always_inline))

#define UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#define LIKELY(expr) __builtin_expect(!!(expr), 1)

#if defined(__GNUC__) || defined(__INTEL_COMPILER)
# define unused __attribute__((__unused__))
#else
# define unused
#endif


/* command line arguments parsing section */
extern char *argv0;

#define ARGBEGIN                                                                               \
for (argv0 = *argv, argv++, argc--;                                                            \
        argv[0] && argv[0][0] == '-' && argv[0][1];                                            \
        argc--, argv++) {                                                                      \
    char argc_;                                                                                \
    char **argv_;                                                                              \
    int brk_;                                                                                  \
    if (argv[0][1] == '-' && argv[0][2] == '\0') {                                             \
        argv++;                                                                                \
        argc--;                                                                                \
        break;                                                                                 \
    }                                                                                          \
    for (brk_ = 0, argv[0]++, argv_ = argv;                                                    \
            argv[0][0] && !brk_;                                                               \
            argv[0]++) {                                                                       \
        if (argv_ != argv) break;                                                              \
        argc_ = argv[0][0];                                                                    \
        switch (argc_)

#define ARGEND      }}

#define ARGC()      argc_

#define EARGF(x)                                                                               \
((argv[0][1] == '\0' && argv[1] == NULL)?                                                      \
    ((x), abort(), (char *)0) :                                                                \
    (brk_ = 1, (argv[0][1] != '\0')?                                                           \
        (&argv[0][1]) :                                                                        \
        (argc--, argv++, argv[0])))

#define ARGF()                                                                                 \
((argv[0][1] == '\0' && argv[1] == NULL)?                                                      \
    (char *)0 :                                                                                \
    (brk_ = 1, (argv[0][1] != '\0')?                                                           \
        (&argv[0][1]) :                                                                        \
        (argc--, argv++, argv[0])))


#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))


void *xmalloc(const size_t size);
void *xrealloc(void *original, const size_t size);

#endif