/* Loopback test server, see tsrv.h. Test code: blocking I/O with timeouts in a forked
 * child, one connection at a time. */
#define _POSIX_C_SOURCE 200809L
#include "tsrv.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "fixtures/certs.h"

static const char *const kind_name[TS_N] = {"plain", "ec", "rsa", "wrong", "expired",
                                            "self", "other", "stall", "backlog"};

struct tsrv {
    pid_t pid;
    int ctl, rep;
    unsigned port[TS_N];
    /* parent-side report log */
    char *rbuf;
    size_t rlen, rcap;
    char **req;
    size_t *reqlen;
    size_t nreq, capreq;
};

/* ---- small helpers ---- */

static void sleep_us(unsigned us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static int write_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *p, size_t n)
{
    char *c = p;
    while (n) {
        ssize_t r = read(fd, c, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        c += r;
        n -= (size_t)r;
    }
    return 0;
}

static int listen_on(int backlog, unsigned *port)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, backlog) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &l) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    return fd;
}

/* ---- child: TLS ---- */

static mbedtls_entropy_context c_entropy;
static mbedtls_ctr_drbg_context c_drbg;
static mbedtls_ssl_config c_conf[TS_N];
static mbedtls_x509_crt c_crt[TS_N];
static mbedtls_pk_context c_key[TS_N];

static int c_tls_init(void)
{
    static const char *const certs[TS_N] = {NULL, fx_localhost_ec_pem, fx_localhost_rsa_pem,
                                            fx_wrong_host_pem, fx_expired_pem, fx_self_signed_pem,
                                            fx_other_leaf_pem, NULL, NULL};
    static const char *const keys[TS_N] = {NULL, fx_localhost_ec_key, fx_localhost_rsa_key,
                                           fx_wrong_host_key, fx_expired_key, fx_self_signed_key,
                                           fx_other_leaf_key, NULL, NULL};
    int k;
    /* A fork shares the parent's RNG state: start PSA and the DRBG afresh. */
    mbedtls_psa_crypto_free();
    if (psa_crypto_init() != PSA_SUCCESS)
        return -1;
    mbedtls_entropy_init(&c_entropy);
    mbedtls_ctr_drbg_init(&c_drbg);
    if (mbedtls_ctr_drbg_seed(&c_drbg, mbedtls_entropy_func, &c_entropy,
                              (const unsigned char *)"tsrv", 4) != 0)
        return -1;
    for (k = 0; k < TS_N; k++) {
        if (certs[k] == NULL)
            continue;
        mbedtls_x509_crt_init(&c_crt[k]);
        mbedtls_pk_init(&c_key[k]);
        mbedtls_ssl_config_init(&c_conf[k]);
        if (mbedtls_x509_crt_parse(&c_crt[k], (const unsigned char *)certs[k],
                                   strlen(certs[k]) + 1) != 0 ||
            (k == TS_OTHER && mbedtls_x509_crt_parse(&c_crt[k], (const unsigned char *)fx_other_ca_pem,
                                                     strlen(fx_other_ca_pem) + 1) != 0) ||
            mbedtls_pk_parse_key(&c_key[k], (const unsigned char *)keys[k], strlen(keys[k]) + 1,
                                 NULL, 0, mbedtls_ctr_drbg_random, &c_drbg) != 0 ||
            mbedtls_ssl_config_defaults(&c_conf[k], MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
            mbedtls_ssl_conf_own_cert(&c_conf[k], &c_crt[k], &c_key[k]) != 0)
            return -1;
        mbedtls_ssl_conf_rng(&c_conf[k], mbedtls_ctr_drbg_random, &c_drbg);
        mbedtls_ssl_conf_authmode(&c_conf[k], MBEDTLS_SSL_VERIFY_NONE);
    }
    return 0;
}

typedef struct cio {
    int fd;
    int tls;
    mbedtls_ssl_context ssl;
} cio;

static int c_send(void *ctx, const unsigned char *b, size_t n)
{
    ssize_t w = send(*(int *)ctx, b, n, MSG_NOSIGNAL);
    return w < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : (int)w;
}

static int c_recv(void *ctx, unsigned char *b, size_t n)
{
    ssize_t r = recv(*(int *)ctx, b, n, 0);
    return r < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : (int)r;
}

static long cio_read(cio *c, void *b, size_t n)
{
    if (c->tls) {
        int r = mbedtls_ssl_read(&c->ssl, b, n);
        return r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ? 0 : r;
    }
    return (long)recv(c->fd, b, n, 0);
}

static int cio_write(cio *c, const void *b, size_t n)
{
    const unsigned char *p = b;
    while (n) {
        long w;
        if (c->tls) {
            w = mbedtls_ssl_write(&c->ssl, p, n);
        } else
            w = (long)send(c->fd, p, n, MSG_NOSIGNAL);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ---- child: scripts ---- */

typedef struct script {
    char *path, *resp;
    size_t resp_len, piece;
    unsigned delay;
    int no_notify;
} script;

#define SCRIPTS_MAX 32
static script scripts[SCRIPTS_MAX];

static void c_ctl(int ctl)
{
    uint32_t h[5];
    char op;
    script sc;
    int i, free_i = -1;
    if (read_all(ctl, &op, 1) != 0 || op == 'Q')
        _exit(0);
    if (read_all(ctl, h, sizeof h) != 0)
        _exit(1);
    memset(&sc, 0, sizeof sc);
    sc.path = calloc(1, h[0] + 1);
    sc.resp = malloc(h[1] ? h[1] : 1);
    if (!sc.path || !sc.resp || read_all(ctl, sc.path, h[0]) != 0 ||
        read_all(ctl, sc.resp, h[1]) != 0)
        _exit(1);
    sc.resp_len = h[1];
    sc.piece = h[2];
    sc.delay = h[3];
    sc.no_notify = (int)h[4];
    for (i = 0; i < SCRIPTS_MAX; i++) {
        if (scripts[i].path && strcmp(scripts[i].path, sc.path) == 0) {
            free(scripts[i].path);
            free(scripts[i].resp);
            scripts[i] = sc;
            goto ack;
        }
        if (!scripts[i].path && free_i < 0)
            free_i = i;
    }
    if (free_i < 0)
        _exit(1);
    scripts[free_i] = sc;
ack:
    write_all(ctl, "k", 1);
}

/* ---- child: serving ---- */

static void send_pieces(cio *c, const char *p, size_t n, size_t piece, unsigned delay)
{
    while (n) {
        size_t k = piece && piece < n ? piece : n;
        if (cio_write(c, p, k) != 0)
            return;
        p += k;
        n -= k;
        if (n && delay)
            sleep_us(delay);
    }
}

static void wait_close(cio *c)
{
    char b[256];
    while (cio_read(c, b, sizeof b) > 0)
        ;
}

static size_t header_len(const char *h, size_t n)
{
    size_t i;
    for (i = 3; i < n; i++)
        if (h[i - 3] == '\r' && h[i - 2] == '\n' && h[i - 1] == '\r' && h[i] == '\n')
            return i + 1;
    return 0;
}

static unsigned long content_length(const char *h, size_t hl)
{
    size_t i;
    for (i = 0; i + 16 < hl; i++)
        if ((i == 0 || h[i - 1] == '\n') && strncasecmp(h + i, "content-length:", 15) == 0)
            return strtoul(h + i + 15, NULL, 10);
    return 0;
}

static void c_serve(int fd, int kind, int rep)
{
    static char req[256 * 1024];
    char resp[1024], path[2048];
    size_t n = 0, hl = 0, body;
    int no_notify = 0, i;
    cio c;
    struct timeval tv;

    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    i = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &i, sizeof i);
    memset(&c, 0, sizeof c);
    c.fd = fd;
    c.tls = kind != TS_PLAIN;
    if (c.tls) {
        mbedtls_ssl_init(&c.ssl);
        if (mbedtls_ssl_setup(&c.ssl, &c_conf[kind]) != 0)
            goto out;
        mbedtls_ssl_set_bio(&c.ssl, &c.fd, c_send, c_recv, NULL);
        if (mbedtls_ssl_handshake(&c.ssl) != 0)
            goto out; /* the client's verification failed or it went away */
    }
    for (;;) {
        long r;
        if (n == sizeof req)
            goto out;
        r = cio_read(&c, req + n, sizeof req - n);
        if (r <= 0)
            goto out;
        n += (size_t)r;
        if (!hl)
            hl = header_len(req, n);
        if (hl && n >= hl + content_length(req, hl))
            break;
    }
    body = content_length(req, hl);
    {
        char tag[32];
        uint32_t len;
        int tl = snprintf(tag, sizeof tag, "L=%s\n", kind_name[kind]);
        len = (uint32_t)(tl + hl + body);
        if (write_all(rep, &len, 4) != 0 || write_all(rep, tag, (size_t)tl) != 0 ||
            write_all(rep, req, hl + body) != 0)
            _exit(1);
    }
    {
        char *sp = memchr(req, ' ', hl), *e;
        size_t pl;
        if (sp == NULL)
            goto out;
        e = memchr(sp + 1, ' ', hl - (size_t)(sp + 1 - req));
        if (e == NULL)
            goto out;
        pl = (size_t)(e - sp - 1);
        if (pl >= sizeof path)
            goto out;
        memcpy(path, sp + 1, pl);
        path[pl] = 0;
    }
    for (i = 0; i < SCRIPTS_MAX; i++)
        if (scripts[i].path && strcmp(scripts[i].path, path) == 0) {
            send_pieces(&c, scripts[i].resp, scripts[i].resp_len, scripts[i].piece,
                        scripts[i].delay);
            no_notify = scripts[i].no_notify;
            goto done;
        }
    if (strcmp(path, "/ok") == 0) {
        static const char r[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                                "Content-Length: 5\r\n\r\nhello";
        cio_write(&c, r, sizeof r - 1);
    } else if (strcmp(path, "/echo") == 0) {
        int k = snprintf(resp, sizeof resp,
                         "HTTP/1.1 200 OK\r\nX-Method: %.*s\r\nContent-Length: %lu\r\n\r\n",
                         (int)(strchr(req, ' ') - req), req, (unsigned long)body);
        cio_write(&c, resp, (size_t)k);
        cio_write(&c, req + hl, body);
    } else if (strcmp(path, "/chunked") == 0) {
        static const char r[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                "5\r\nhello\r\n2;x=y\r\n, \r\n5\r\nworld\r\n0\r\nT: 1\r\n\r\n";
        send_pieces(&c, r, sizeof r - 1, 7, 0);
    } else if (strcmp(path, "/sse-bytes") == 0) {
        static const char b[] = TSRV_SSE_BYTES;
        int k = snprintf(resp, sizeof resp,
                         "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                         "Content-Length: %u\r\n\r\n", (unsigned)(sizeof b - 1));
        cio_write(&c, resp, (size_t)k);
        send_pieces(&c, b, sizeof b - 1, 1, 300);
    } else if (strcmp(path, "/close-delim") == 0 || strcmp(path, "/close-noclose") == 0) {
        static const char r[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nabcdef";
        cio_write(&c, r, sizeof r - 1);
        no_notify = path[7] == 'n';
    } else if (strncmp(path, "/r?c=", 5) == 0) {
        const char *l = strstr(path, "&l=");
        int k = snprintf(resp, sizeof resp,
                         "HTTP/1.1 %.3s Moved\r\nLocation: %s\r\nContent-Length: 5\r\n\r\nmoved",
                         path + 5, l ? l + 3 : "/");
        cio_write(&c, resp, (size_t)k);
    } else if (strncmp(path, "/chain/", 7) == 0) {
        int left = atoi(path + 7), k;
        if (left > 0)
            k = snprintf(resp, sizeof resp,
                         "HTTP/1.1 302 Found\r\nLocation: /chain/%d\r\nContent-Length: 0\r\n\r\n",
                         left - 1);
        else
            k = snprintf(resp, sizeof resp,
                         "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nend");
        cio_write(&c, resp, (size_t)k);
    } else if (strcmp(path, "/stall-body") == 0) {
        static const char r[] = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789";
        cio_write(&c, r, sizeof r - 1);
        wait_close(&c);
        no_notify = 1;
    } else if (strcmp(path, "/early-close") == 0) {
        static const char r[] = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789";
        cio_write(&c, r, sizeof r - 1);
    } else if (strcmp(path, "/gzip") == 0) {
        static const char r[] = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
                                "Content-Length: 3\r\n\r\nabc";
        cio_write(&c, r, sizeof r - 1);
    } else if (strcmp(path, "/big") == 0) {
        static char big[TSRV_BIG_LEN];
        size_t k;
        int m = snprintf(resp, sizeof resp, "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n",
                         TSRV_BIG_LEN);
        for (k = 0; k < TSRV_BIG_LEN; k++)
            big[k] = (char)(k * 7 % 251);
        cio_write(&c, resp, (size_t)m);
        send_pieces(&c, big, TSRV_BIG_LEN, 4096, 0);
    } else {
        static const char r[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\nnope";
        cio_write(&c, r, sizeof r - 1);
    }
done:
    if (c.tls && !no_notify)
        mbedtls_ssl_close_notify(&c.ssl);
out:
    if (c.tls)
        mbedtls_ssl_free(&c.ssl);
    shutdown(fd, SHUT_WR);
    close(fd);
}

#define STALL_MAX 64
#define BL_RING 8
#define BL_TICK_MS 2

/* Replace the oldest held connect to the backlog listener with a fresh nonblocking one. */
static void bl_refill(int ring[BL_RING], int *at, const struct sockaddr_in *a)
{
    int fd;
    if (ring[*at] >= 0)
        close(ring[*at]);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        (void)connect(fd, (const struct sockaddr *)a, sizeof *a);
    }
    ring[*at] = fd;
    *at = (*at + 1) % BL_RING;
}

static void child(int lfd[TS_N], unsigned port_backlog, int ctl, int rep)
{
    int stalled[STALL_MAX], nst = 0, pre[BL_RING], pre_at = 0, k;
    struct sockaddr_in a;

    signal(SIGPIPE, SIG_IGN);
    if (c_tls_init() != 0)
        _exit(3);
    /* Keep the backlog listener's accept queue full (listen(fd, 0)) and its SYN queue
     * "young", so a client connect stays pending. Kernels since 2017 drop a SYN on a full
     * accept queue; Linux 3.4 (Android 4.4) drops it only while more than one young
     * (never retransmitted, ~1 s) request waits, else it answers and the client reaches
     * ESTABLISHED. Hence BL_RING/2 connects up front, then one fresh connect per 2 ms tick that
     * replaces the oldest, so young requests never run out. */
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port_backlog);
    for (k = 0; k < BL_RING; k++)
        pre[k] = -1;
    for (k = 0; k < BL_RING / 2; k++)
        bl_refill(pre, &pre_at, &a);
    for (;;) {
        struct pollfd p[TS_N + 1];
        int n = 0, i;
        for (i = 0; i < TS_N; i++) {
            p[n].fd = i == TS_BACKLOG ? -1 : lfd[i];
            p[n].events = POLLIN;
            p[n].revents = 0;
            n++;
        }
        p[n].fd = ctl;
        p[n].events = POLLIN;
        p[n].revents = 0;
        n++;
        if (poll(p, (nfds_t)n, BL_TICK_MS) < 0) {
            if (errno == EINTR)
                continue;
            _exit(2);
        }
        bl_refill(pre, &pre_at, &a);
        if (p[TS_N].revents)
            c_ctl(ctl);
        for (i = 0; i < TS_N; i++) {
            int fd;
            if (!(p[i].revents & POLLIN))
                continue;
            fd = accept(lfd[i], NULL, NULL);
            if (fd < 0)
                continue;
            if (i == TS_STALL) {
                if (nst == STALL_MAX) {
                    close(stalled[0]);
                    memmove(stalled, stalled + 1, sizeof stalled[0] * (STALL_MAX - 1));
                    nst--;
                }
                stalled[nst++] = fd;
                continue;
            }
            c_serve(fd, i, rep);
        }
    }
}

/* ---- parent ---- */

tsrv *tsrv_start(void)
{
    tsrv *s = calloc(1, sizeof *s);
    int lfd[TS_N], ctl[2], rep[2], i;
    if (s == NULL)
        return NULL;
    for (i = 0; i < TS_N; i++) {
        lfd[i] = listen_on(i == TS_BACKLOG ? 0 : 16, &s->port[i]);
        if (lfd[i] < 0)
            return NULL;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ctl) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, rep) != 0)
        return NULL;
    fflush(NULL);
    s->pid = fork();
    if (s->pid < 0)
        return NULL;
    if (s->pid == 0) {
        close(ctl[0]);
        close(rep[0]);
        child(lfd, s->port[TS_BACKLOG], ctl[1], rep[1]);
        _exit(0);
    }
    close(ctl[1]);
    close(rep[1]);
    for (i = 0; i < TS_N; i++)
        if (i != TS_BACKLOG)
            close(lfd[i]);
    /* the backlog listener must stay open in the parent too: its queue is the point */
    s->ctl = ctl[0];
    s->rep = rep[0];
    fcntl(s->rep, F_SETFL, O_NONBLOCK);
    return s;
}

void tsrv_stop(tsrv *s)
{
    size_t i;
    if (s == NULL)
        return;
    write_all(s->ctl, "Q", 1);
    close(s->ctl);
    kill(s->pid, SIGTERM);
    waitpid(s->pid, NULL, 0);
    close(s->rep);
    for (i = 0; i < s->nreq; i++)
        free(s->req[i]);
    free(s->req);
    free(s->reqlen);
    free(s->rbuf);
    free(s);
}

unsigned tsrv_port(const tsrv *s, int listener)
{
    return s->port[listener];
}

int tsrv_script(tsrv *s, const char *path, const char *resp, size_t resp_len, size_t piece,
                unsigned delay_us, int no_notify)
{
    uint32_t h[5];
    char ack;
    h[0] = (uint32_t)strlen(path);
    h[1] = (uint32_t)resp_len;
    h[2] = (uint32_t)piece;
    h[3] = delay_us;
    h[4] = (uint32_t)no_notify;
    if (write_all(s->ctl, "S", 1) != 0 || write_all(s->ctl, h, sizeof h) != 0 ||
        write_all(s->ctl, path, h[0]) != 0 || write_all(s->ctl, resp, resp_len) != 0 ||
        read_all(s->ctl, &ack, 1) != 0)
        return -1;
    return 0;
}

void tsrv_pump(tsrv *s)
{
    for (;;) {
        ssize_t r;
        uint32_t len;
        if (s->rcap - s->rlen < 65536) {
            size_t cap = s->rcap ? s->rcap * 2 : 262144;
            char *nb = realloc(s->rbuf, cap);
            if (nb == NULL)
                return;
            s->rbuf = nb;
            s->rcap = cap;
        }
        r = read(s->rep, s->rbuf + s->rlen, s->rcap - s->rlen);
        if (r > 0)
            s->rlen += (size_t)r;
        while (s->rlen >= 4) {
            memcpy(&len, s->rbuf, 4);
            if (s->rlen < 4 + (size_t)len)
                break;
            if (s->nreq == s->capreq) {
                size_t cap = s->capreq ? s->capreq * 2 : 64;
                s->req = realloc(s->req, cap * sizeof *s->req);
                s->reqlen = realloc(s->reqlen, cap * sizeof *s->reqlen);
                s->capreq = cap;
            }
            s->req[s->nreq] = malloc((size_t)len + 1);
            memcpy(s->req[s->nreq], s->rbuf + 4, len);
            s->req[s->nreq][len] = 0;
            s->reqlen[s->nreq++] = len;
            memmove(s->rbuf, s->rbuf + 4 + len, s->rlen - 4 - len);
            s->rlen -= 4 + (size_t)len;
        }
        if (r <= 0)
            return;
    }
}

size_t tsrv_wait(tsrv *s, size_t n, int ms)
{
    int waited = 0;
    tsrv_pump(s);
    while (s->nreq < n && waited < ms) {
        struct pollfd p;
        p.fd = s->rep;
        p.events = POLLIN;
        poll(&p, 1, 10);
        waited += 10;
        tsrv_pump(s);
    }
    return s->nreq;
}

size_t tsrv_count(const tsrv *s)
{
    return s->nreq;
}

const char *tsrv_req(const tsrv *s, size_t i, size_t *len)
{
    if (i >= s->nreq)
        return NULL;
    if (len)
        *len = s->reqlen[i];
    return s->req[i];
}

void tsrv_clear(tsrv *s)
{
    size_t i;
    tsrv_pump(s);
    for (i = 0; i < s->nreq; i++)
        free(s->req[i]);
    s->nreq = 0;
}

static const char *write_tmp(const char *pem, char *path, size_t cap)
{
    const char *dir = getenv("TMPDIR");
    int fd;
    snprintf(path, cap, "%s/canlin-ca-XXXXXX", dir && *dir ? dir : "/tmp");
    fd = mkstemp(path);
    if (fd < 0 || write_all(fd, pem, strlen(pem)) != 0) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    close(fd);
    return path;
}

static char ca_path[256], other_path[256];

static void rm_tmp(void)
{
    if (ca_path[0])
        unlink(ca_path);
    if (other_path[0])
        unlink(other_path);
}

const char *tsrv_ca_file(void)
{
    if (!ca_path[0]) {
        if (write_tmp(fx_ca_pem, ca_path, sizeof ca_path) == NULL)
            return NULL;
        atexit(rm_tmp);
    }
    return ca_path;
}

const char *tsrv_other_ca_file(void)
{
    if (!other_path[0] && write_tmp(fx_other_ca_pem, other_path, sizeof other_path) == NULL)
        return NULL;
    return other_path;
}
