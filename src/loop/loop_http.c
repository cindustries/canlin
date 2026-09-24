/* canlin_http as a native loop source (native loop spec §2, HTTP transport spec §3). */
#include "canlin/http.h"
#include "canlin/loop.h"

static size_t http_pollfds(void *ctx, struct pollfd *out, size_t max, canlin_ev_time *deadline)
{
    return canlin_http_pollfds(ctx, out, max, deadline);
}

static void http_step(void *ctx, canlin_ev_time now)
{
    canlin_http_step(ctx, now);
}

canlin_loop_source canlin_loop_http_source(struct canlin_http *h)
{
    canlin_loop_source s;
    s.pollfds = http_pollfds;
    s.step = http_step;
    s.ctx = h;
    return s;
}
