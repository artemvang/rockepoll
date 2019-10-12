
#ifndef H_LIST
#define H_LIST

#define DL_APPEND_LEFT(__head, __item) \
do { \
    (__item)->next = __head; \
    (__item)->prev = NULL; \
    if (__head) (__head)->prev = conn; \
    __head = conn; \
} while (0)


#define DL_DELETE(__head, __item) \
do { \
    if (__head == __item) __head = (__item)->next; \
    if ((__item)->next) (__item)->next->prev = (__item)->prev; \
    if ((__item)->prev) (__item)->prev->next = (__item)->next; \
    free(__item); \
} while (0)


#define LL_APPEND_LEFT(__head, __item) \
do { \
    (__item)->next = __head; \
    __head = __item; \
} while (0)


#define LL_APPEND(__head, __item) \
do { \
    __typeof(__item) __tmp; \
    if (__head) { \
        __tmp = (__head); \
        while (__tmp->next) __tmp = __tmp->next; \
        __tmp->next=(__item); \
    } else { \
        (__head)=(__item); \
    } \
} while (0)


#define LL_MOVE_NEXT(__head, __cleanup_func) \
do { \
    __typeof(__head) __tmp; \
    __tmp = (__head)->next; \
    __cleanup_func; \
    free(__head); \
    (__head) = __tmp; \
} while (0)


#define LL_FREE(__head, __cleanup_func) \
do { \
    while (__head) LL_MOVE_NEXT(__head, __cleanup_func); \
} while (0)


#endif