#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <err.h>

#include "io.h"
#include "http/handler.h"
#include "utils.h"
#include "log.h"
#include "utlist.h"
#include "thpool.h"

#define MAXFDS 1024 * 4
#define KEEP_ALIVE_TIMEOUT 5

#if defined(__GNUC__) || defined(__INTEL_COMPILER)
# define UNUSED __attribute__((__unused__))
#else
# define UNUSED
#endif

#define CLOSE_CONN(conn)                                                                       \
do {                                                                                           \
    close((conn)->fd);                                                                         \
    cleanup_steps((conn)->steps);                                                              \
    DL_DELETE(connections, conn);                                                              \
    free(conn);                                                                                \
} while (0)


/* command line parameters */
static int   port = 7887;
static int   threads = 1;
static int   keep_alive = 0;
static int   change_root = 0;
static int   quiet = 0;
static char *listen_addr = "127.0.0.1";
static char *wwwroot = ".";

static int listenfd, epollfd;
static int loop = 1;


static void
create_listen_socket()
{
    int    opt;
    struct sockaddr_in addr;

    listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listenfd < 0) {
        err(1, "socket(), SOCK_STREAM | SOCK_NONBLOCK");
    }

    opt = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        err(1, "setsockopt(), SOL_SOCKET, SO_REUSEADDR");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(listen_addr);

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        err(1, "bind(), `%d'", port);
    }

    if (listen(listenfd, -1) < 0) {
        err(1, "listen()");
    }
}


static void
accept_peers_loop(struct connection **connections, time_t now)
{
    int                  peerfd, opt;
    struct connection   *conn;
    struct sockaddr_in   connection_addr;
    struct epoll_event   peer_event = {0};
    socklen_t            connection_addr_len = sizeof(connection_addr);

    while (1) {
        peerfd = accept4(listenfd,
                         (struct sockaddr *)&connection_addr,
                         &connection_addr_len, SOCK_NONBLOCK);

        if (peerfd < 0) {
            if (UNLIKELY(errno != EAGAIN && errno != EWOULDBLOCK)) {
                warn("accept4()");
            }
            break;
        } else {
            opt = 1;
            if (UNLIKELY(setsockopt(peerfd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt)))) {
                warn("setsockopt(), SOL_TCP, TCP_NODELAY");
            }

            conn = xmalloc(sizeof(struct connection));

            memset(conn->ip, 0, sizeof(conn->ip));
            strcpy(conn->ip, inet_ntoa(connection_addr.sin_addr));
            conn->fd = peerfd;
            conn->last_active = now;
            conn->status = C_RUN;
            conn->keep_alive = keep_alive;
            conn->steps = NULL;
            conn->next = NULL;
            conn->prev = NULL;
            setup_read_io_step(&(conn->steps), build_response);

            DL_APPEND(*connections, conn);

            peer_event.data.ptr = conn;
            peer_event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
            if (UNLIKELY(epoll_ctl(epollfd, EPOLL_CTL_ADD, peerfd, &peer_event) < 0)) {
                warn("epoll_ctl()");
                close(peerfd);
                continue;
            }
        }
    }
}


static void
usage(const char *argv0)
{
    printf("usage: %s path [--addr addr] [--port port] [--quiet] [--keep-alive] [--chroot]\n", argv0);   
}


static void
parse_args(int argc, char *argv[])
{
    int i;
    size_t len;

    if (argc < 2 || (argc == 2 && !strcmp(argv[1], "--help"))) {
        usage(argv[0]);
        exit(0);
    }

    if (getuid() == 0) {
        port = 80;
    }

    wwwroot = argv[1];
    /* Strip ending slash */
    len = strlen(wwwroot);
    if (len > 1) {
        if (wwwroot[len - 1] == '/') {
            wwwroot[len - 1] = '\0';
        }
    }

    for (i = 2; i < argc; i++) {
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
        else if (!strcmp(argv[i], "--chroot")) {
            change_root = 1;
        }
        else if (!strcmp(argv[i], "--threads")) {
            if (++i >= argc) {
                errx(1, "missing number after --threads");
            }
            threads = atoi(argv[i]);
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
    int                  i;
    time_t               now;
    struct epoll_event   ev = {0};
    struct epoll_event   events[MAXFDS] = {0};
    struct thpool       *pool;
    struct connection   *tmp_conn, *conn, *connections = NULL;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, int_handler);

    parse_args(argc, argv);

    if (change_root) {
        i = chroot(wwwroot);
        if (i < 0) {
            err(1, "chroot(), `%s'", wwwroot);
        }
    } else {
        i = chdir(wwwroot);
        if (i < 0) {
            err(1, "chdir(), `%s'", wwwroot);
        }
    }

    log_setup(quiet);
    create_listen_socket();

    epollfd = epoll_create1(0);
    if (epollfd < 0) {
        err(1, "epoll_create1()");
    }

    ev.data.ptr = &listenfd;
    ev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev) < 0) {
        err(1, "epoll_ctl()");
    }

    pool = thpool_create(threads);

    printf("listening on http://%s:%d/\n", listen_addr, port);
    while (loop) {
        now = time(NULL);

        DL_FOREACH_SAFE(connections, conn, tmp_conn) {
            if (conn->status == C_CLOSE || difftime(now, conn->last_active) > KEEP_ALIVE_TIMEOUT) {
                CLOSE_CONN(conn);
            }
        }

        i = epoll_wait(epollfd, events, MAXFDS, KEEP_ALIVE_TIMEOUT * 1000);
        if (UNLIKELY(i < 0)) {
            warn("epoll_wait()");
            continue;
        }


        while (i) {
            ev = events[--i];
            conn = ev.data.ptr;

            /*
                In this case conn does not reference to connection's struct,
                but references to address of listenfd variable. It works because
                connection's struct first element is fd, so dereferencing gives
                in both cases fd variable
            */
            if (conn->fd  == listenfd) {
                accept_peers_loop(&connections, now);
            } else if (
                ev.events & EPOLLHUP ||
                ev.events & EPOLLERR ||
                ev.events & EPOLLRDHUP)
            {
                CLOSE_CONN(conn);
            } else {
                thpool_add(pool, (void (*)(void *))process_connection, conn);
                conn->last_active = now;
            }
        }

        thpool_wait(pool);
    }

    thpool_destroy(pool);

    DL_FOREACH_SAFE(connections, conn, tmp_conn) {
        CLOSE_CONN(conn);
    }

    close(epollfd);
    close(listenfd);

    return 0;
}
