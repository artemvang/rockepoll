#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <stdio.h>
#include <err.h>

#include "io.h"
#include "http/handler.h"
#include "utils.h"
#include "log.h"
#include "utlist.h"

#define MAXFDS 128
#define SERVER_FD_COUNT 8
#define KEEP_ALIVE_TIMEOUT 5

#if defined(__GNUC__) || defined(__INTEL_COMPILER)
# define UNUSED __attribute__((__unused__))
#else
# define UNUSED
#endif

#define CLOSE_CONN(__connections, __conn, __peers_count)                                                      \
do {                                                                                           \
    close((__conn)->fd);                                                                       \
    FREE_IO_STEPS((__conn)->steps);                                                            \
    DL_DELETE(__connections, __conn);                                                          \
    free(__conn);                                                                              \
    __peers_count--;                                                                           \
} while (0)


static char *argv0;

/* command line parameters */
static int   port = 7887;
static int   keep_alive = 0;
static int   quiet = 0;
static char *listen_addr = "127.0.0.1";

static int listenfd, epollfd, peers_count = 0;
static int loop = 1;


static void
create_listen_socket()
{
    int    opt;
    struct sockaddr_in addr;

    listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listenfd < 0) {
        errx(1, "socket()");
    }

    opt = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        errx(1, "setsockopt()");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(listen_addr);

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        errx(1, "bind(), port %d", port);
    }

    if (listen(listenfd, -1) < 0) {
        errx(1, "listen()");
    }
}


static void
accept_peers_loop(struct connection **connections, struct connection *fd2connection[])
{
    int                  peerfd, opt;
    struct sockaddr_in   connection_addr;
    struct connection   *conn;
    struct epoll_event   peer_event = {0};
    socklen_t            connection_addr_len = sizeof(connection_addr);
    time_t               last_active;

    last_active = time(NULL);

    while (peers_count < MAXFDS) {
        peerfd = accept4(listenfd,
                         (struct sockaddr *)&connection_addr,
                         &connection_addr_len, SOCK_NONBLOCK);

        if (peerfd < 0) {
            if (LIKELY(errno != EAGAIN && errno != EWOULDBLOCK)) {
                warnx("accept4()");
            }
            break;
        } else {
            opt = 1;
            if (UNLIKELY(setsockopt(peerfd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt)))) {
                errx(1, "setsockopt(), can't set TCP_NODELAY for peer socket");
            }

            peer_event.data.fd = peerfd;
            peer_event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
            if (UNLIKELY(epoll_ctl(epollfd, EPOLL_CTL_ADD, peerfd, &peer_event) < 0)) {
                warnx("epoll_ctl(), can't add peer socket to epoll");
                close(peerfd);
                continue;
            }

            conn = xmalloc(sizeof(struct connection));

            memset(conn->ip, 0, sizeof(conn->ip));
            strcpy(conn->ip, inet_ntoa(connection_addr.sin_addr));
            conn->fd = peerfd;
            conn->last_active = last_active;
            conn->status = C_RUN;
            conn->keep_alive = keep_alive;
            conn->steps = NULL;
            setup_read_io_step(conn, IO_FLAG_NONE, build_response);

            DL_APPEND(*connections, conn);    
            fd2connection[peerfd] = conn;

            peers_count++;
        }
    }
}


static void
usage()
{
    printf("usage: %s [--addr addr] [--port port] [--quiet] [--keep-alive]\n", argv0);
    exit(0);
}


static void
parse_args(int argc, char *argv[])
{
    int i;
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
        else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        }
        else if (!strcmp(argv[i], "--keep-alive")) {
            keep_alive = 1;
        }
        else {
            errx(1, "unknown argument `%s'", argv[i]);
        }
    }
}


static void
int_handler(int dummy UNUSED)
{
    loop = 0;
}


int
main(int argc, char *argv[])
{
    int                  nready, fd;
    time_t               now;
    struct epoll_event   ev, listen_event = {0};
    struct epoll_event   events[MAXFDS + SERVER_FD_COUNT] = {0};
    struct connection   *tmp_conn, *conn, *connections = NULL;
    struct connection   *fd2connection[MAXFDS + SERVER_FD_COUNT] = {0};

    signal(SIGINT, int_handler);
    signal(SIGPIPE, SIG_IGN);

    parse_args(argc, argv);

    log_setup(quiet);

    create_listen_socket();

    epollfd = epoll_create1(0);
    if (epollfd < 0) {
        errx(1, "epoll_create1()");
    }

    listen_event.data.fd = listenfd;
    listen_event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &listen_event) < 0) {
        errx(1, "epoll_ctl(), can't add listen socket to epoll");
    }

    printf("listening on http://%s:%d/\n", listen_addr, port);
    while (loop) {
        now = time(NULL);

        DL_FOREACH_SAFE(connections, conn, tmp_conn) {
            if (difftime(now, conn->last_active) > KEEP_ALIVE_TIMEOUT) {
                CLOSE_CONN(connections, conn, peers_count);
            }
        }

        nready = epoll_wait(epollfd, events, MAXFDS, KEEP_ALIVE_TIMEOUT * 1000);
        if (UNLIKELY(nready < 0)) {
            warnx("epoll_wait()");
            continue;
        }

        while (nready) {
            ev = events[--nready];
            fd = ev.data.fd;
            conn = fd2connection[fd];

            if (fd == listenfd) {
                accept_peers_loop(&connections, fd2connection);
            }
            else if (
                ev.events & EPOLLHUP ||
                ev.events & EPOLLERR ||
                ev.events & EPOLLRDHUP)
            {
                CLOSE_CONN(connections, conn, peers_count);
            }
            else {
                process_connection(conn);
                if (conn->status == C_CLOSE) {
                    CLOSE_CONN(connections, conn, peers_count);
                } else {
                    conn->last_active = now;
                }
            }
        }
    }

    DL_FOREACH_SAFE(connections, conn, tmp_conn) {
        CLOSE_CONN(connections, conn, peers_count);
    }

    close(epollfd);
    close(listenfd);

    return 0;
}
