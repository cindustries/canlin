/* Thin Mbed TLS 3.6 wrapper (HTTP transport spec §5): one shared client configuration
 * with the trust store and RNG, loaded lazily before the first handshake; nonblocking
 * connections over our own socket callbacks.
 *
 * Verification is always required (chain, host name via SNI + name check, validity
 * dates); there is no switch to turn it off. Dates are checked here against the
 * configured wall clock (MBEDTLS_HAVE_TIME_DATE is off in canlin_mbedtls_config.h), so
 * a failure that consists only of date flags is reported as NET_TLS_E_CLOCK. */
#define _POSIX_C_SOURCE 200809L
#include "net_int.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h" /* error codes only; MBEDTLS_NET_C is off */
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

struct net_tls {
    char *ca_file;
    canlin_http_wall_fn wall;
    void *wall_ctx;
    int ready;            /* RNG seeded, config set up, trust loaded */
    int rng_ok, conf_ok;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    mbedtls_ssl_config conf;
};

struct net_tls_conn {
    mbedtls_ssl_context ssl;
    net_tls *t;
    int fd;
    short events;
};

static const uint16_t groups[] = {MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
                                  MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
                                  MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1, 0};

/* ---- wall clock and validity dates ---- */

static int64_t wall_default(void *ctx)
{
    (void)ctx;
    return (int64_t)time(NULL);
}

/* Seconds since the epoch to UTC civil time without time_t (32-bit time_t safe). */
static void civil(int64_t t, mbedtls_x509_time *o)
{
    int64_t days = t / 86400, rem = t % 86400, z, era, doe, yoe, doy, mp, y;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    o->hour = (int)(rem / 3600);
    o->min = (int)(rem % 3600 / 60);
    o->sec = (int)(rem % 60);
    z = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    o->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    o->mon = (int)(mp < 10 ? mp + 3 : mp - 9);
    o->year = (int)(y + (o->mon <= 2));
}

static int tcmp(const mbedtls_x509_time *a, const mbedtls_x509_time *b)
{
    const int x[6] = {a->year, a->mon, a->day, a->hour, a->min, a->sec};
    const int y[6] = {b->year, b->mon, b->day, b->hour, b->min, b->sec};
    int i;
    for (i = 0; i < 6; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

static int verify_dates(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    net_tls *t = ctx;
    mbedtls_x509_time now;
    (void)depth;
    civil(t->wall(t->wall_ctx), &now);
    if (tcmp(&now, &crt->valid_from) < 0)
        *flags |= MBEDTLS_X509_BADCERT_FUTURE;
    if (tcmp(&now, &crt->valid_to) > 0)
        *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
    return 0;
}

/* ---- shared context ---- */

net_tls *net_tls_new(const char *ca_file, canlin_http_wall_fn wall, void *wall_ctx)
{
    net_tls *t = calloc(1, sizeof *t);
    size_t n = strlen(ca_file);
    if (t == NULL)
        return NULL;
    t->ca_file = malloc(n + 1);
    if (t->ca_file == NULL) {
        free(t);
        return NULL;
    }
    memcpy(t->ca_file, ca_file, n + 1);
    t->wall = wall ? wall : wall_default;
    t->wall_ctx = wall_ctx;
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_ssl_config_init(&t->conf);
    return t;
}

void net_tls_free(net_tls *t)
{
    if (t == NULL)
        return;
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    free(t->ca_file);
    free(t);
}

int net_tls_trust_loaded(const net_tls *t)
{
    return t && t->ready;
}

/* RNG and configuration once; the trust bundle until it loads (a bundle that appears
 * later, e.g. after a mount, is picked up by the next connection). */
static int ensure_ready(net_tls *t)
{
    static const unsigned char pers[] = "canlin-http";
    int rc;
    if (t->ready)
        return NET_TLS_OK;
    if (!t->rng_ok) {
        if (psa_crypto_init() != PSA_SUCCESS)
            return NET_TLS_E_TLS;
        if (mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, pers,
                                  sizeof pers - 1) != 0)
            return NET_TLS_E_TLS;
        t->rng_ok = 1;
    }
    if (!t->conf_ok) {
        if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0)
            return NET_TLS_E_TLS;
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
        mbedtls_ssl_conf_verify(&t->conf, verify_dates, t);
        mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_groups(&t->conf, groups);
        t->conf_ok = 1;
    }
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_x509_crt_init(&t->ca);
    /* > 0: some certificates in the bundle did not parse; the rest are usable. */
    rc = mbedtls_x509_crt_parse_file(&t->ca, t->ca_file);
    if (rc < 0 || t->ca.version == 0) {
        mbedtls_x509_crt_free(&t->ca);
        mbedtls_x509_crt_init(&t->ca);
        return NET_TLS_E_TRUST;
    }
    mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
    t->ready = 1;
    return NET_TLS_OK;
}

/* ---- socket callbacks ---- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    net_tls_conn *c = ctx;
    int err = 0;
    long r = net_sock_send(c->fd, buf, len, &err);
    if (r > 0)
        return (int)r;
    if (r == 0)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    net_tls_conn *c = ctx;
    int err = 0;
    long r = net_sock_recv(c->fd, buf, len, &err);
    if (r >= 0)
        return (int)r; /* 0: EOF, Mbed TLS reports it as unclean close */
    if (r == NET_AGAIN)
        return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* ---- connections ---- */

net_tls_conn *net_tls_conn_new(net_tls *t, int fd, const char *host, int *err)
{
    net_tls_conn *c;
    int rc = ensure_ready(t);
    if (rc != NET_TLS_OK) {
        *err = rc;
        return NULL;
    }
    c = calloc(1, sizeof *c);
    if (c == NULL) {
        *err = NET_TLS_E_TLS;
        return NULL;
    }
    c->t = t;
    c->fd = fd;
    c->events = POLLOUT;
    mbedtls_ssl_init(&c->ssl);
    if (mbedtls_ssl_setup(&c->ssl, &t->conf) != 0 ||
        mbedtls_ssl_set_hostname(&c->ssl, host) != 0) {
        mbedtls_ssl_free(&c->ssl);
        free(c);
        *err = NET_TLS_E_TLS;
        return NULL;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    return c;
}

void net_tls_conn_free(net_tls_conn *c)
{
    if (c == NULL)
        return;
    mbedtls_ssl_free(&c->ssl); /* zeroises keys and record buffers */
    free(c);
}

static int want(net_tls_conn *c, int rc)
{
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
        c->events = POLLIN;
        return NET_TLS_WANT_READ;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        c->events = POLLOUT;
        return NET_TLS_WANT_WRITE;
    }
    return 0;
}

int net_tls_handshake(net_tls_conn *c)
{
    int rc = mbedtls_ssl_handshake(&c->ssl), w;
    uint32_t flags;
    if (rc == 0) {
        c->events = POLLIN;
        return NET_TLS_OK;
    }
    if ((w = want(c, rc)) != 0)
        return w;
    flags = mbedtls_ssl_get_verify_result(&c->ssl);
    if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED ||
        (flags != 0 && flags != (uint32_t)-1)) {
        const uint32_t dates = MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE;
        if (flags != 0 && flags != (uint32_t)-1 && (flags & ~dates) == 0)
            return NET_TLS_E_CLOCK;
        return NET_TLS_E_VERIFY;
    }
    if (rc == MBEDTLS_ERR_NET_RECV_FAILED || rc == MBEDTLS_ERR_NET_SEND_FAILED)
        return NET_TLS_E_RESET;
    return NET_TLS_E_TLS;
}

long net_tls_read(net_tls_conn *c, void *buf, size_t n)
{
    int rc = mbedtls_ssl_read(&c->ssl, buf, n), w;
    if (rc > 0)
        return rc;
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return 0;
    if (rc == 0)
        return NET_TLS_E_EOF;
    if ((w = want(c, rc)) != 0)
        return w;
    if (rc == MBEDTLS_ERR_NET_RECV_FAILED || rc == MBEDTLS_ERR_NET_SEND_FAILED)
        return NET_TLS_E_RESET;
    return NET_TLS_E_TLS;
}

long net_tls_write(net_tls_conn *c, const void *buf, size_t n)
{
    int rc = mbedtls_ssl_write(&c->ssl, buf, n), w;
    if (rc > 0)
        return rc;
    if ((w = want(c, rc)) != 0)
        return w;
    if (rc == MBEDTLS_ERR_NET_RECV_FAILED || rc == MBEDTLS_ERR_NET_SEND_FAILED)
        return NET_TLS_E_RESET;
    return NET_TLS_E_TLS;
}

int net_tls_pending(net_tls_conn *c)
{
    return mbedtls_ssl_get_bytes_avail(&c->ssl) > 0 || mbedtls_ssl_check_pending(&c->ssl);
}

short net_tls_events(const net_tls_conn *c)
{
    return c->events;
}
