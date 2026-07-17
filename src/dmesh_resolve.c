/*
 * dmesh_resolve.c — DPUmesh name/identity resolver (NAMING.md Phase 0).
 *
 * ONE table, both façades. This file is the whole of the "provenance moves from
 * the user to a control plane" change on the host side: it answers the four
 * questions the transport asks about identity, and it takes a *name* where the
 * caller used to type an *integer*.
 *
 *   identity()      — this node's own service, from DPUMESH_SERVICE (a name)
 *   listen_port()   — this node's meshed listen port, from DPUMESH_PORT
 *   resolve_name()  — a PEER's service_id, by k8s Service name (native API)
 *   resolve_addr()  — a PEER's service_id, by ClusterIP:port (the shim)
 *
 * The table is a file — a ConfigMap mounted at /etc/dpumesh/registry in
 * production, or DPUMESH_CONFIG=<path> for the bench harness playing controller
 * (NAMING.md §5). Lines are "IP:port name svc"; blank / non-parsing lines
 * (e.g. '#' comments) are skipped. Loaded ONCE and then immutable — live
 * inotify reload is NAMING.md Phase 3, so post-load reads need no lock.
 *
 * The integer service_id is BORN here (resolve_*), and appears nowhere above
 * this layer: not in <dpumesh/dmesh.h>, not in app code, not in YAML.
 */
#define _GNU_SOURCE
#include "dmesh_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>

#define RESOLVE_MAX_ENTRIES 256
#define RESOLVE_NAME_MAX    64
#define RESOLVE_DEFAULT_PATH "/etc/dpumesh/registry"

struct resolve_ent {
    uint32_t addr;                       /* ClusterIP, net order; 0 = name-only entry */
    uint16_t port;                       /* host order */
    int      svc;                        /* interned service_id ([0,127]) */
    char     name[RESOLVE_NAME_MAX];
};

static struct resolve_ent g_ent[RESOLVE_MAX_ENTRIES];
static int                g_ent_n;
static int                g_loaded;      /* load-once guard */
static pthread_mutex_t    g_load_mu = PTHREAD_MUTEX_INITIALIZER;

static void add_entry(uint32_t addr, uint16_t port, const char *name, int svc) {
    if (g_ent_n >= RESOLVE_MAX_ENTRIES) return;
    struct resolve_ent *e = &g_ent[g_ent_n++];
    e->addr = addr;
    e->port = port;
    e->svc  = svc;
    snprintf(e->name, sizeof e->name, "%s", name ? name : "");
}

/* Parse "IP:port name svc" lines. A row with a placeholder IP (0.0.0.0) is
 * name-only — legal for a native peer that is never dialed by ClusterIP. */
/* Returns 1 if the file was opened + parsed, 0 if absent (caller warns). */
static int load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;                    /* absent file → caller warns; empty table */
    char ln[256], ip[64], name[RESOLVE_NAME_MAX];
    int  port, svc;
    while (fgets(ln, sizeof ln, f)) {
        struct in_addr a;
        if (sscanf(ln, " %63[^:]:%d %63s %d", ip, &port, name, &svc) != 4)
            continue;                    /* blank / comment / malformed → skip */
        if (inet_pton(AF_INET, ip, &a) != 1)
            continue;                    /* '#' comments land here too */
        add_entry(a.s_addr, (uint16_t)port, name, svc);
    }
    fclose(f);
    return 1;
}

int dmesh_config_load(const char *path) {
    pthread_mutex_lock(&g_load_mu);
    if (!g_loaded) {
        const char *p = path;
        if (!p) p = getenv("DPUMESH_CONFIG");
        if (!p || !*p) p = RESOLVE_DEFAULT_PATH;
        if (!load_file(p))
            fprintf(stderr, "[dpumesh] WARNING: registry file not found at '%s' (set "
                    "$DPUMESH_CONFIG or mount the ConfigMap at /etc/dpumesh/registry) — "
                    "name/addr resolution will return ENOENT for every peer.\n", p);
        g_loaded = 1;                    /* idempotent: even an absent file counts as loaded */
    }
    pthread_mutex_unlock(&g_load_mu);
    return g_ent_n;
}

int dmesh_resolve_name(const char *name) {
    if (!name || !*name) { errno = ENOENT; return -1; }
    if (!g_loaded) dmesh_config_load(NULL);
    for (int i = 0; i < g_ent_n; i++)
        if (strcmp(g_ent[i].name, name) == 0)
            return g_ent[i].svc;
    errno = ENOENT;
    return -1;
}

int dmesh_resolve_addr(uint32_t ip_net, uint16_t port_host) {
    if (!g_loaded) dmesh_config_load(NULL);
    for (int i = 0; i < g_ent_n; i++)
        if (g_ent[i].port == port_host && g_ent[i].addr == ip_net && g_ent[i].addr != 0)
            return g_ent[i].svc;
    return -1;                           /* not meshed → kernel TCP */
}

int dmesh_config_listen_port(void) {
    const char *e = getenv("DPUMESH_PORT");
    return (e && *e) ? atoi(e) : -1;     /* -1 = not a server */
}

int dmesh_config_identity(void) {
    const char *svc = getenv("DPUMESH_SERVICE");
    if (!svc || !*svc) return DMESH_SVC_NONE;   /* unset = INTENTIONAL pure client */
    int id = dmesh_resolve_name(svc);
    if (id < 0) {
        /* SET but not in the registry: the operator meant this to be a server of `svc`,
         * but it will come up as a pure client and be UNREACHABLE. Surface it loudly
         * instead of degrading silently — the table is load-once, so it won't self-heal. */
        fprintf(stderr, "[dpumesh] WARNING: $DPUMESH_SERVICE='%s' is NOT in the registry — "
                "coming up as a pure CLIENT (unreachable as a server). Check the registry "
                "lists this name.\n", svc);
        return DMESH_SVC_NONE;
    }
    return id;
}
