/* Drives a crypto owner over its socket with fixture values: install an RX and
 * a TX SA for one synthetic association on two lanes, rekey to a second epoch,
 * retire the old one, block, remove everything. Prints every completion. No
 * QP exists and no packet flows; this exercises the owner's dynamic
 * SA/rule installation and removal on real hardware, nothing more. */
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <openssl/rand.h>
#include "doca/peer_crypto_ipc.h"

static int wait_completion(struct peer_crypto_adapter *a, struct peer_crypto_completion *d, unsigned ms)
{
    for (unsigned i = 0; i < ms / 5; i++) {
        if (a->poll(a->ctx, d) == 1) return 1;
        struct pollfd p = {.fd = a->fd(a->ctx), .events = POLLIN};
        (void)poll(&p, 1, 5);
    }
    return 0;
}
static const char *action_name(enum peer_crypto_action a)
{
    static const char *names[] = {"?", "INSTALL_RX", "INSTALL_TX", "ENABLE_QPS", "QUIESCE", "RESUME",
                                  "REMOVE_OLD", "BLOCK", "DESTROY_QPS", "REMOVE_ALL"};
    return a >= 1 && a <= 9 ? names[a] : "?";
}
int main(int argc, char **argv)
{
    if (argc < 5 || argc > 7) {
        fprintf(stderr, "usage: %s SOCKET LOCAL_IPV4 PEER_IPV4 LOCAL_ENDPOINT(0|1) [STEP_DELAY_MS] [STEPS]\n"
                        "  STEPS: comma list of rN (INSTALL_RX epoch N), tN (INSTALL_TX), oN (REMOVE_OLD of N),\n"
                        "         b (BLOCK), a (REMOVE_ALL); default r1,t1,r2,t2,o1,b,a\n", argv[0]);
        return 2;
    }
    unsigned delay_ms = argc >= 6 ? (unsigned)atoi(argv[5]) : 0;
    const char *plan = argc == 7 ? argv[6] : "r1,t1,r2,t2,o1,b,a";
    struct in_addr local, peer;
    unsigned endpoint = (unsigned)atoi(argv[4]);
    if (inet_pton(AF_INET, argv[2], &local) != 1 || inet_pton(AF_INET, argv[3], &peer) != 1 || endpoint > 1) return 2;
    struct peer_crypto_ipc *ipc; char error[128];
    if (peer_crypto_ipc_new(argv[1], &ipc, error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 1; }
    struct peer_crypto_adapter a = peer_crypto_ipc_adapter(ipc);
    for (unsigned i = 0; i < 400 && !a.healthy(a.ctx); i++) { struct timespec t = {.tv_nsec = 5000000}; nanosleep(&t, NULL); }
    if (!a.healthy(a.ctx)) { fprintf(stderr, "owner did not answer on %s\n", argv[1]); return 1; }
    printf("OWNER_CONNECTED\n");
    struct peer_crypto_request r = {.token = {.manager = 0x51, .generation = 1, .slot = 0}, .lane_count = 2,
                                    .local_endpoint = endpoint};
    r.binding.epoch = 1; r.binding.spi[0] = 0x1000 + endpoint; r.binding.spi[1] = 0x2000 + endpoint;
    RAND_bytes(r.binding.session, 16); RAND_bytes(r.binding.association, 16);
    for (unsigned i = 0; i < 2; i++) {
        r.lanes[i].id = i + 1; r.lanes[i].generation = 1; r.lanes[i].mtu = 1024;
        r.lanes[i].qpn[endpoint] = 0x100 + i; r.lanes[i].qpn[1-endpoint] = 0x200 + i;
        uint8_t *g = r.lanes[i].gid[endpoint]; memset(g, 0, 16); g[10] = g[11] = 0xff; memcpy(g + 12, &local, 4);
        g = r.lanes[i].gid[1-endpoint]; memset(g, 0, 16); g[10] = g[11] = 0xff; memcpy(g + 12, &peer, 4);
    }
    struct { enum peer_crypto_action action; uint64_t epoch, old; } steps[32];
    unsigned nsteps = 0;
    uint64_t current = 1;
    for (const char *p = plan; *p && nsteps < 32; ) {
        char kind = *p++; unsigned n = 0;
        while (*p >= '0' && *p <= '9') n = n * 10 + (unsigned)(*p++ - '0');
        if (*p == ',') p++;
        if (kind == 'r' || kind == 't') current = n ? n : current;
        steps[nsteps].epoch = current; steps[nsteps].old = 0;
        switch (kind) {
        case 'r': steps[nsteps].action = PEER_CRYPTO_INSTALL_RX; break;
        case 't': steps[nsteps].action = PEER_CRYPTO_INSTALL_TX; break;
        case 'o': steps[nsteps].action = PEER_CRYPTO_REMOVE_OLD; steps[nsteps].old = n; break;
        case 'b': steps[nsteps].action = PEER_CRYPTO_BLOCK; break;
        case 'a': steps[nsteps].action = PEER_CRYPTO_REMOVE_ALL; break;
        default: fprintf(stderr, "bad step %c\n", kind); return 2;
        }
        nsteps++;
    }
    int failures = 0;
    for (unsigned s = 0; s < nsteps; s++) {
        r.operation = s + 1; r.action = steps[s].action; r.binding.epoch = steps[s].epoch; r.old_epoch = steps[s].old;
        r.binding.spi[0] = 0x1000 * (uint32_t)steps[s].epoch + endpoint; r.binding.spi[1] = 0x1000 * (uint32_t)steps[s].epoch + 0x800 + endpoint;
        if (getenv("PROBE_RANDOM_SPI")) {   /* per run: the association bytes make the SPIs unique */
            r.binding.spi[0] = 0x100000 + ((uint32_t)r.binding.association[0] << 8 | r.binding.association[1]) * 4 + (uint32_t)steps[s].epoch;
            r.binding.spi[1] = 0x200000 + ((uint32_t)r.binding.association[2] << 8 | r.binding.association[3]) * 4 + (uint32_t)steps[s].epoch;
        }
        RAND_bytes(r.material, 20);
        if (delay_ms) { struct timespec t = {.tv_sec = delay_ms / 1000, .tv_nsec = (delay_ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
        int rc = a.submit(a.ctx, &r);
        struct peer_crypto_completion d = {0};
        int got = rc == 1 && wait_completion(&a, &d, 20000);
        printf("STEP %u %s epoch=%llu submit=%d status=%d%s\n", s + 1, action_name(steps[s].action),
               (unsigned long long)steps[s].epoch, rc, got ? d.status : 99, got ? "" : " (no completion)");
        if (!got || d.status) failures++;
    }
    peer_crypto_ipc_free(ipc);
    printf("OWNER_PROBE_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
