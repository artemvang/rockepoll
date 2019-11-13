#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <err.h>

#include "io.h"
#include "log.h"
#include "utils.h"
#include "utlist.h"
#include "handler.h"


#define MAXFDS 128
#define MAX_THREADS 32
#define KEEP_ALIVE_TIMEOUT 5 /* in seconds */
#define EPOLL_WAIT_TIMEOUT 10 /* in milliseconds */

#if defined(__GNUC__) || defined(__INTEL_COMPILER)
# define UNUSED __attribute__((__unused__))
#else
# define UNUSED
#endif

#define CLOSE_CONN(connections, conn)                                         \
do {                                                                          \
    close((conn)->fd);                                                        \
    cleanup_steps((conn)->steps);                                             \
    DL_DELETE(connections, conn);                                             \
    free(conn);                                                               \
} while (0)


/* command line parameters */
static int   port = 7887;
static int   threads_count = 1;
static int   keep_alive = 0;
static int   quiet = 0;
static char *listen_addr = "127.0.0.1";
static char *root_dir = ".";

static volatile int loop = 1;


static void
accept_peers_loop(struct connection **connections,
                  int listenfd, int epollfd, time_t now)
{
    int                  peerfd, opt;
    struct connection   *conn;
    struct sockaddr_in   conn_addr;
    struct epoll_event   peer_event = {0};
    socklen_t            conn_addr_len = sizeof(conn_addr);

    for (;;) {
        peerfd = accept4(listenfd,
                         (struct sockaddr *)&conn_addr, &conn_addr_len,
                         SOCK_NONBLOCK);

        if (peerfd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                warn("accept4()");
            }
            break;
        } else {
            opt = 1;
            if (setsockopt(peerfd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
                warn("setsockopt(), SOL_TCP, TCP_NODELAY");
            }

            conn = xmalloc(sizeof(struct connection));

            memset(conn->ip, 0, sizeof(conn->ip));
            strcpy(conn->ip, inet_ntoa(conn_addr.sin_addr));
            conn->fd = peerfd;
            conn->last_active = now;
            conn->status = C_RUN;
            conn->keep_alive = keep_alive;
            conn->steps = NULL;
            conn->next = NULL;
            conn->prev = NULL;
            setup_read_io_step(&conn->steps, build_response);

            DL_APPEND(*connections, conn);

            peer_event.data.ptr = conn;
            peer_event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
            if (epoll_ctl(epollfd, EPOLL_CTL_ADD, peerfd, &peer_event) < 0) {
                warn("epoll_ctl()");
                CLOSE_CONN(*connections, conn);
            }
        }
    }
}


static void *
run_worker()
{
    int                  i, epollfd, listenfd;
    time_t               now;
    struct epoll_event   ev = {0};
    struct epoll_event   events[MAXFDS] = {0};
    struct connection   *tmp_conn, *conn, *connections = NULL;

    listenfd = create_listen_socket(listen_addr, port);

    if ((epollfd = epoll_create1(0)) < 0) {
        err(1, "epoll_create1()");
    }

    ev.data.ptr = &listenfd;
    ev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev) < 0) {
        err(1, "epoll_ctl()");
    }

    while (loop) {
        now = time(NULL);

        DL_FOREACH_SAFE(connections, conn, tmp_conn) {
            if (conn->status == C_CLOSE ||
                difftime(now, conn->last_active) > KEEP_ALIVE_TIMEOUT)
            {
                CLOSE_CONN(connections, conn);
            }
        }

        i = epoll_wait(epollfd, events, MAXFDS, EPOLL_WAIT_TIMEOUT);
        if (i < 0) {
            warn("epoll_wait()");
            continue;
        }


        while (i) {
            ev = events[--i];
            conn = ev.data.ptr;

            /* In this case conn does not reference to connection's struct,
             * but references to address of listenfd variable. It works because
             * connection's struct first element is fd, so dereferencing gives
             * in both cases fd variable
             */
            if (conn->fd  == listenfd) {
                accept_peers_loop(&connections, listenfd, epollfd, now);
            } else if (
                ev.events & EPOLLHUP ||
                ev.events & EPOLLERR ||
                ev.events & EPOLLRDHUP)
            {
                CLOSE_CONN(connections, conn);
            } else {
                process_connection(conn);
                conn->last_active = now;
            }
        }
    }

    DL_FOREACH_SAFE(connections, conn, tmp_conn) {
        CLOSE_CONN(connections, conn);
    }

    close(listenfd);
    close(epollfd);

    return NULL;
}


static void
run_server()
{
    int i;
    pthread_t *threads;

    if (threads_count == 1) {
        run_worker();
    } else {
        threads = xmalloc(sizeof(pthread_t) * threads_count);
        for (i = 0; i < threads_count; i++) {
            pthread_create(threads + i, NULL, run_worker, NULL);
        }

        for (i = 0; i < threads_count; i++) {
            pthread_join(threads[i], NULL); 
        }

        free(threads);
    }
}


static void
sigint_handler(int dummy UNUSED)
{
    loop = 0;
}


static void
usage(const char *argv0)
{
    printf("usage: %s path "
           "[--addr addr] "
           "[--port port] "
           "[--quiet] "
           "[--keep-alive] "
           "[--threads N]\n", argv0);
}


static void
parse_args(int argc, char *argv[])
{
    int i;
    size_t len;
    char *next = NULL;

    if (argc < 2 || (argc == 2 && !strcmp(argv[1], "--help"))) {
        usage(argv[0]);
        exit(0);
    }

    if (getuid() == 0) {
        port = 80;
    }

    root_dir = argv[1];
    /* Strip ending slash */
    len = strlen(root_dir);
    if (len > 1) {
        if (root_dir[len - 1] == '/') {
            root_dir[len - 1] = '\0';
        }
    }

    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--port")) {
            if (++i >= argc) {
                errx(1, "missing number after --port");
            }
            port = strtol(argv[i], &next, 10);
            if (next == argv[i] || *next != '\0') {
                errx(1, "invalid argument `%s'", argv[i]);
            }
        }
        else if (!strcmp(argv[i], "--addr")) {
            if (++i >= argc) {
                errx(1, "missing ip after --addr");
            }
            listen_addr = argv[i];
        }
        else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        }
        else if (!strcmp(argv[i], "--keep-alive")) {
            keep_alive = 1;
        }
        else if (!strcmp(argv[i], "--threads")) {
            if (++i >= argc) {
                errx(1, "missing number after --threads");
            }
            threads_count = strtol(argv[i], &next, 10);
            if (next == argv[i] || *next != '\0') {
                errx(1, "invalid argument `%s'", argv[i]);
            }
            else if (threads_count >= MAX_THREADS) {
                errx(1, "too large amount of threads (%d >= %d)",
                     threads_count, MAX_THREADS);
            }
        }
        else {
            errx(1, "unknown argument `%s'", argv[i]);
        }
    }
}


int
main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sigint_handler);

    parse_args(argc, argv);

    init_logger(quiet);
    init_handler(root_dir);

    printf("listening on http://%s:%d/\n", listen_addr, port);
    run_server();

    return 0;
}
