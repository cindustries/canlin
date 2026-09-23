/* Nonblocking TCP connect and I/O (HTTP transport spec §2, §4). POSIX. */
#define _POSIX_C_SOURCE 200809L
#include "net_int.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* platforms without it need SO_NOSIGPIPE (not a v1 target) */
#endif

int net_sock_connect(const canlin_net_addr *a, int *pending, int *err)
{
    int fd, fl, one = 1;

    *pending = 0;
    fd = socket(a->sa.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        *err = errno;
        return -1;
    }
    fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        *err = errno;
        close(fd);
        return -1;
    }
    /* Request pieces and small SSE events go out without Nagle delay. */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (const struct sockaddr *)&a->sa, a->len) == 0)
        return fd;
    if (errno == EINPROGRESS || errno == EINTR) {
        *pending = 1;
        return fd;
    }
    *err = errno;
    close(fd);
    return -1;
}

int net_sock_check(int fd, int *err)
{
    struct pollfd p;
    int e = 0, rc;
    socklen_t l = sizeof e;

    p.fd = fd;
    p.events = POLLOUT;
    p.revents = 0;
    do
        rc = poll(&p, 1, 0);
    while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        *err = errno;
        return -1;
    }
    if (rc == 0)
        return 0;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l) < 0) {
        *err = errno;
        return -1;
    }
    if (e != 0) {
        *err = e;
        return -1;
    }
    if (p.revents & (POLLERR | POLLHUP)) {
        *err = ECONNREFUSED;
        return -1;
    }
    return 1;
}

long net_sock_send(int fd, const void *buf, size_t n, int *err)
{
    ssize_t r;
    do
        r = send(fd, buf, n, MSG_NOSIGNAL);
    while (r < 0 && errno == EINTR);
    if (r >= 0)
        return (long)r;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return 0;
    *err = errno;
    return NET_ERR;
}

long net_sock_recv(int fd, void *buf, size_t n, int *err)
{
    ssize_t r;
    do
        r = recv(fd, buf, n, 0);
    while (r < 0 && errno == EINTR);
    if (r >= 0)
        return (long)r;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return NET_AGAIN;
    *err = errno;
    return NET_ERR;
}

void net_sock_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

const char *net_sock_strerror(int err)
{
    switch (err) {
    case ECONNREFUSED:
        return "connection refused";
    case ENETUNREACH:
    case EHOSTUNREACH:
        return "host unreachable";
    case ECONNRESET:
        return "connection reset";
    case EPIPE:
        return "connection closed by the peer";
    case ETIMEDOUT:
        return "timed out";
    case EAFNOSUPPORT:
    case EPROTONOSUPPORT:
        return "address family not supported";
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
        return "out of resources";
    default:
        return "failed";
    }
}
