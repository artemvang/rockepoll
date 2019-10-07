#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <time.h>
#include <err.h>

#include "arg.h"
#include "io.h"
#include "utils.h"


#define MAXFDS 1024


char *argv0;

static int listenfd, epollfd;
static int accepting = 1;
static int max_fd = 0;
static volatile int loop = 1;
static struct connection **connections;

/* defaults */
static int port = 7887;
static char *listen_addr = "127.0.0.1";
static int keep_alive = 0;


static void
create_listen_socket()
{
    int opt;
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
accept_peers_loop()
{
    int                  peerfd, opt;
    struct sockaddr_in   connection_addr;
    struct connection   *conn;
    struct epoll_event   peer_event = {0};
    socklen_t            connection_addr_len = sizeof(connection_addr);
    time_t               last_active;

    last_active = time(NULL);

    while (accepting) {
        peerfd = accept4(listenfd,
                         (struct sockaddr *)&connection_addr,
                         &connection_addr_len, SOCK_NONBLOCK);

        if (peerfd < 0) {
            if (likely(errno != EAGAIN && errno != EWOULDBLOCK)) {
                warnx("accept4()");
            }
            break;
        } else {
            opt = 1;
            if (setsockopt(peerfd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
                errx(1, "setsockopt()");
            }

            conn = xmalloc(sizeof(struct connection));
            conn->fd = peerfd;
            conn->last_active = last_active;
            conn->status = C_RUN;
            conn->keep_alive = keep_alive;
            conn->steps = NULL;

            setup_read_io_step(conn);
                
            connections[peerfd] = conn;

            peer_event.data.fd = peerfd;
            peer_event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
            if (unlikely(epoll_ctl(epollfd, EPOLL_CTL_ADD, peerfd, &peer_event) < 0)) {
                warnx("epoll_ctl(), can't add peer socket to epoll");
                close(peerfd);
                continue;
            }

            max_fd = MAX(max_fd, peerfd);
            accepting = max_fd + 1 < MAXFDS;
        }
    }
}

static inline void
int_handler(int dummy unused)
{
    loop = 0;
}


static inline void
usage()
{
    printf("usage: %s [-l listen] [-p port] [-k] [-h]\n", argv0);
    exit(0);
}


int
main(int argc, char *argv[])
{
    int nready, fd, i;
    struct epoll_event ev, listen_event = {0};
    struct epoll_event *events;
    struct connection *conn;

    signal(SIGINT, int_handler);
    signal(SIGPIPE, SIG_IGN);

    ARGBEGIN {
    case 'p':
        port = atoi(EARGF(usage()));
        break;
    case 'l':
        listen_addr = EARGF(usage());
        break;
    case 'k':
        keep_alive = 1;
        break;
    case 'h':
        usage();
        break;
    default:
        break;
    } ARGEND;

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

    events = xmalloc(sizeof(struct epoll_event) * MAXFDS);
    connections = xmalloc(sizeof(struct connection *) * MAXFDS);

    while (loop) {
        time_t now = time(NULL);

        for (fd = i = 5; fd <= max_fd; fd++) {
            conn = connections[fd];
            if (conn)  {
                if (conn->status == C_CLOSE ||
                    difftime(now, conn->last_active) > 60)
                {
                    close(fd);
                    LL_CLEAN(conn->steps);
                    free(conn);
                    connections[fd] = NULL;
                } else {
                    i = fd;
                }
            }
        }

        max_fd = i;
        accepting = max_fd + 1 < MAXFDS;

        nready = epoll_wait(epollfd, events, MAXFDS, 60000);
        if (unlikely(nready < 0)) {
            warnx("epoll_wait()");
            continue;
        }

        while (nready) {
            ev = events[--nready];
            fd = ev.data.fd;
            conn = connections[fd];

            if (fd == listenfd) {
                accept_peers_loop();
            }
            else if (
                ev.events & EPOLLHUP ||
                ev.events & EPOLLERR ||
                ev.events & EPOLLRDHUP)
            {
                conn->status = C_CLOSE;
            }
            else {
                process_connection(conn);
                conn->last_active = time(NULL);
            }
        }
    }

    free(events);
    free(connections);

    close(epollfd);
    close(listenfd);

    return 0;
}
