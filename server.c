#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <err.h>

#include "arg.h"
#include "io.h"
#include "utils.h"


#define MAXFDS 1024
#define KEEP_ALIVE_TIMEOUT 60


char *argv0;

static int listenfd, epollfd, accepting = 1;
static volatile int loop = 1;

/* parameters from command line */
static int port = 7887; 
static int keep_alive = 1; 
static char *listen_addr = "127.0.0.1"; 


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


static int
accept_peers_loop(int maxfd, struct connection *connections[])
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
            if (LIKELY(errno != EAGAIN && errno != EWOULDBLOCK)) {
                warnx("accept4()");
            }
            break;
        } else {
            opt = 1;
            if (UNLIKELY(setsockopt(peerfd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt)))) {
                errx(1, "setsockopt(), can't set TCP_NODELAY for peer socket");
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
            if (UNLIKELY(epoll_ctl(epollfd, EPOLL_CTL_ADD, peerfd, &peer_event) < 0)) {
                warnx("epoll_ctl(), can't add peer socket to epoll");
                close(peerfd);
                continue;
            }

            maxfd = MAX(maxfd, peerfd);
            accepting = maxfd + 1 < MAXFDS;
        }
    }

    return maxfd;
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
    int                  nready, fd, i, maxfd = 0;
    time_t               now;
    struct epoll_event   ev, listen_event = {0};
    struct epoll_event   events[MAXFDS] = {0};
    struct connection   *conn;
    struct connection   *connections[MAXFDS] = {0};

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

    while (loop) {
        now = time(NULL);

        for (fd = i = epollfd + 1; fd <= maxfd; fd++) {
            conn = connections[fd];
            if (conn) {
                if (conn->status == C_CLOSE
                    || difftime(now, conn->last_active) > KEEP_ALIVE_TIMEOUT)
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

        maxfd = i;
        accepting = maxfd + 1 < MAXFDS;

        nready = epoll_wait(epollfd, events, MAXFDS, KEEP_ALIVE_TIMEOUT * 1000);
        if (UNLIKELY(nready < 0)) {
            warnx("epoll_wait()");
            continue;
        }

        while (nready) {
            ev = events[--nready];
            fd = ev.data.fd;
            conn = connections[fd];

            if (fd == listenfd) {
                maxfd = accept_peers_loop(maxfd, connections);
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
                conn->last_active = now;
            }
        }
    }

    close(epollfd);
    close(listenfd);

    return 0;
}
