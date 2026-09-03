/* Peer resolution for the native and preload APIs. The DPU answers every
 * question from the held topology generation; the host holds no registry file
 * and treats the returned id as an opaque node-local handle. Answers are
 * cached per key for one generation interval, so the cache is never staler
 * than the generation's own freshness bound. */
#define _GNU_SOURCE
#include "dmesh_core.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "doca/comch_common.h"

#define RESOLVE_MAX_ENTRIES 256
/* GENERATION_INTERVAL: re-resolve after this, or on any connection error. */
#define RESOLVE_TTL_NS 5000000000ull

struct resolve_ent {
    uint8_t  in_use;
    uint8_t  by_name;
    uint8_t  status;                     /* 0 meshed, 1 not-meshed */
    uint32_t addr;                       /* by_name == 0: ClusterIP, net order */
    uint16_t port;                       /* host order */
    char     name[128];                  /* by_name == 1 */
    int      svc;                        /* DPU-interned id when status == 0 */
    uint64_t expires_ns;
};

static struct resolve_ent g_ent[RESOLVE_MAX_ENTRIES];
static unsigned            g_cursor;
static pthread_mutex_t     g_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static struct resolve_ent *
cache_find(int by_name, const char *name, uint32_t addr, uint16_t port)
{
    uint64_t now = now_ns();
    for (unsigned i = 0; i < RESOLVE_MAX_ENTRIES; i++) {
        struct resolve_ent *e = &g_ent[i];
        if (!e->in_use || e->by_name != by_name || e->expires_ns <= now)
            continue;
        if (by_name ? strcmp(e->name, name) == 0
                    : (e->addr == addr && e->port == port))
            return e;
    }
    return NULL;
}

static void
cache_put(int by_name, const char *name, uint32_t addr, uint16_t port,
          int status, int svc)
{
    struct resolve_ent *e = cache_find(by_name, name ? name : "", addr, port);
    if (e == NULL) {
        e = &g_ent[g_cursor];
        g_cursor = (g_cursor + 1) % RESOLVE_MAX_ENTRIES;
    }
    memset(e, 0, sizeof(*e));
    e->in_use = 1;
    e->by_name = (uint8_t)by_name;
    e->status = (uint8_t)status;
    e->addr = addr;
    e->port = port;
    if (name != NULL)
        snprintf(e->name, sizeof(e->name), "%s", name);
    e->svc = svc;
    e->expires_ns = now_ns() + RESOLVE_TTL_NS;
}

static int
answer(int status, int svc)
{
    if (status == 0)
        return svc;
    errno = status == 1 ? ENOENT : EAGAIN;
    return -1;
}

int
dmesh_resolve_name_via(dpumesh_ctx_t *ctx, const char *name)
{
    if (ctx == NULL || name == NULL || *name == '\0' ||
        strlen(name) >= 128) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    struct resolve_ent *e = cache_find(1, name, 0, 0);
    if (e != NULL) {
        int rc = answer(e->status, e->svc);
        pthread_mutex_unlock(&g_mu);
        return rc;
    }
    pthread_mutex_unlock(&g_mu);

    struct dmesh_resolve_ack_msg ack;
    if (dpumesh_resolve(ctx, 1, name, 0, 0, &ack) != 0)
        return -1;                        /* errno from dpumesh_resolve, uncached */
    if (ack.status > 1)
        return answer(ack.status, -1);    /* no generation held: retry soon */
    pthread_mutex_lock(&g_mu);
    cache_put(1, name, 0, 0, ack.status, ack.interned_svc);
    pthread_mutex_unlock(&g_mu);
    return answer(ack.status, ack.interned_svc);
}

int
dmesh_resolve_addr_via(dpumesh_ctx_t *ctx, uint32_t ip_net, uint16_t port_host)
{
    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    struct resolve_ent *e = cache_find(0, "", ip_net, port_host);
    if (e != NULL) {
        int rc = answer(e->status, e->svc);
        pthread_mutex_unlock(&g_mu);
        return rc;
    }
    pthread_mutex_unlock(&g_mu);

    struct dmesh_resolve_ack_msg ack;
    if (dpumesh_resolve(ctx, 0, NULL, ip_net, port_host, &ack) != 0)
        return -1;
    if (ack.status > 1)
        return answer(ack.status, -1);
    pthread_mutex_lock(&g_mu);
    cache_put(0, NULL, ip_net, port_host, ack.status, ack.interned_svc);
    pthread_mutex_unlock(&g_mu);
    return answer(ack.status, ack.interned_svc);
}

void
dmesh_resolve_invalidate(uint32_t ip_net, uint16_t port_host)
{
    pthread_mutex_lock(&g_mu);
    struct resolve_ent *e = cache_find(0, "", ip_net, port_host);
    if (e != NULL)
        e->in_use = 0;
    pthread_mutex_unlock(&g_mu);
}

int
dmesh_config_listen_port(void)
{
    const char *e = getenv("DPUMESH_PORT");
    return (e && *e) ? atoi(e) : -1;     /* -1 = not a server */
}
