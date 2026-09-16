/* The runtime-side crypto adapter against a fake owner on a real UNIX socket:
 * framing, completions, backpressure, and what the manager is told when the
 * owner disappears and comes back. No hardware. */
#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "doca/peer_crypto_ipc.h"

struct owner {
    char path[108];
    int listen_fd;
    atomic_int stop, drop_client, hold;
    atomic_uint served;
    pthread_t thread;
};
static void nap(void) { struct timespec t = {.tv_nsec = 2000000}; nanosleep(&t, NULL); }
static void *owner_main(void *v)
{
    struct owner *o = v;
    while (!atomic_load(&o->stop)) {
        struct pollfd lp = {.fd = o->listen_fd, .events = POLLIN};
        if (poll(&lp, 1, 20) <= 0) continue;
        int fd = accept(o->listen_fd, NULL, NULL);
        if (fd < 0) continue;
        uint8_t frame[PEER_CRYPTO_IPC_FRAME_MAX]; size_t len;
        assert(!peer_crypto_ipc_hello_encode(3, 1, frame, sizeof(frame), &len));
        assert(send(fd, frame, len, MSG_NOSIGNAL) == (ssize_t)len);
        uint8_t rx[PEER_CRYPTO_IPC_FRAME_MAX * 2]; size_t rx_len = 0;
        while (!atomic_load(&o->stop) && !atomic_load(&o->drop_client)) {
            struct pollfd cp = {.fd = fd, .events = POLLIN};
            if (poll(&cp, 1, 20) <= 0) continue;
            ssize_t n = recv(fd, rx + rx_len, sizeof(rx) - rx_len, 0);
            if (n <= 0) break;
            rx_len += (size_t)n;
            for (;;) {
                uint16_t type; uint32_t body_len;
                int rc = peer_crypto_ipc_header_decode(rx, rx_len, &type, &body_len);
                assert(rc >= 0);
                if (rc > 0 || rx_len < PEER_CRYPTO_IPC_HEADER_LEN + body_len) break;
                assert(type == PEER_CRYPTO_IPC_REQUEST);
                struct peer_crypto_request r;
                assert(!peer_crypto_ipc_request_decode(rx + PEER_CRYPTO_IPC_HEADER_LEN, body_len, &r));
                while (atomic_load(&o->hold) && !atomic_load(&o->drop_client)) nap();
                if (!atomic_load(&o->drop_client)) {
                    struct peer_crypto_completion d = {.token = r.token, .operation = r.operation,
                        .action = r.action, .epoch = r.binding.epoch,
                        .status = r.action == PEER_CRYPTO_INSTALL_TX && r.material[0] == 0xee ? -1 : 0};
                    memcpy(d.session, r.binding.session, 16); memcpy(d.association, r.binding.association, 16);
                    assert(!peer_crypto_ipc_completion_encode(&d, frame, sizeof(frame), &len));
                    assert(send(fd, frame, len, MSG_NOSIGNAL) == (ssize_t)len);
                    atomic_fetch_add(&o->served, 1);
                }
                size_t whole = PEER_CRYPTO_IPC_HEADER_LEN + body_len;
                memmove(rx, rx + whole, rx_len - whole); rx_len -= whole;
            }
        }
        close(fd);
        atomic_store(&o->drop_client, 0);
    }
    return NULL;
}
static struct peer_crypto_request request(uint64_t op, enum peer_crypto_action action, uint8_t mark)
{
    struct peer_crypto_request r = {.token = {.manager = 7, .generation = 3, .slot = 1}, .operation = op,
        .action = action, .lane_count = 2, .local_endpoint = 1};
    r.binding.epoch = 2; r.binding.spi[0] = 300; r.binding.spi[1] = 400; r.old_epoch = 1;
    memset(r.binding.session, 5, 16); memset(r.binding.association, 6, 16);
    memset(r.material, mark, 20);
    for (unsigned i = 0; i < 2; i++) {
        r.lanes[i].id = i + 1; r.lanes[i].qpn[0] = 0x100 + i; r.lanes[i].qpn[1] = 0x200 + i;
        r.lanes[i].gid[0][15] = 1; r.lanes[i].gid[1][15] = 2;
    }
    return r;
}
static int wait_completion(struct peer_crypto_adapter *a, struct peer_crypto_completion *d)
{
    for (unsigned i = 0; i < 2000; i++) {
        if (a->poll(a->ctx, d) == 1) return 1;
        struct pollfd p = {.fd = a->fd(a->ctx), .events = POLLIN};
        (void)poll(&p, 1, 2);
    }
    return 0;
}
static void codec(void)
{
    uint8_t buf[PEER_CRYPTO_IPC_FRAME_MAX]; size_t len;
    struct peer_crypto_request r = request(9, PEER_CRYPTO_INSTALL_RX, 0x11), back;
    assert(!peer_crypto_ipc_request_encode(&r, buf, sizeof(buf), &len));
    uint16_t type; uint32_t body_len;
    assert(!peer_crypto_ipc_header_decode(buf, len, &type, &body_len) && type == PEER_CRYPTO_IPC_REQUEST);
    assert(len == PEER_CRYPTO_IPC_HEADER_LEN + body_len);
    assert(!peer_crypto_ipc_request_decode(buf + PEER_CRYPTO_IPC_HEADER_LEN, body_len, &back));
    assert(!memcmp(&back.token, &r.token, sizeof(r.token)) && back.operation == 9 && back.action == r.action);
    assert(back.lane_count == 2 && back.lanes[1].qpn[1] == 0x201 && back.lanes[1].gid[1][15] == 2);
    assert(!memcmp(back.material, r.material, 20) && back.binding.spi[1] == 400 && back.old_epoch == 1);
    assert(peer_crypto_ipc_request_decode(buf + PEER_CRYPTO_IPC_HEADER_LEN, body_len - 1, &back) == -1);
    buf[4] = 9; assert(peer_crypto_ipc_header_decode(buf, len, &type, &body_len) == -1);
    struct peer_crypto_completion d = {.token = r.token, .operation = 9, .action = r.action, .epoch = 2, .status = -2}, e;
    assert(!peer_crypto_ipc_completion_encode(&d, buf, sizeof(buf), &len));
    assert(!peer_crypto_ipc_header_decode(buf, len, &type, &body_len) && type == PEER_CRYPTO_IPC_COMPLETION);
    assert(!peer_crypto_ipc_completion_decode(buf + PEER_CRYPTO_IPC_HEADER_LEN, body_len, &e) && e.status == -2);
    d.status = 1; assert(peer_crypto_ipc_completion_encode(&d, buf, sizeof(buf), &len) == -1);
}
int main(void)
{
    codec();
    struct owner o = {0};
    snprintf(o.path, sizeof(o.path), "/tmp/dpumesh-crypto-ipc-test-%ld.sock", (long)getpid());
    unlink(o.path);
    o.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX}; strcpy(addr.sun_path, o.path);
    assert(o.listen_fd >= 0 && !bind(o.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) && !listen(o.listen_fd, 4));
    assert(!pthread_create(&o.thread, NULL, owner_main, &o));

    struct peer_crypto_ipc *ipc; char error[128];
    assert(!peer_crypto_ipc_new(o.path, &ipc, error, sizeof(error)));
    struct peer_crypto_adapter a = peer_crypto_ipc_adapter(ipc);
    for (unsigned i = 0; i < 500 && !a.healthy(a.ctx); i++) nap();
    assert(a.healthy(a.ctx));
    /* Round trips, in order, with a refusal the owner decides. */
    struct peer_crypto_request r1 = request(1, PEER_CRYPTO_INSTALL_RX, 0x11), r2 = request(2, PEER_CRYPTO_INSTALL_TX, 0xee);
    assert(a.submit(a.ctx, &r1) == 1 && a.submit(a.ctx, &r2) == 1);
    struct peer_crypto_completion d;
    assert(wait_completion(&a, &d) && d.operation == 1 && d.status == 0 && d.action == PEER_CRYPTO_INSTALL_RX);
    assert(wait_completion(&a, &d) && d.operation == 2 && d.status == -1 && d.epoch == 2);
    assert(!a.poll(a.ctx, &d));
    /* The owner vanishes with a request outstanding: unknown outcome, then
     * unhealthy, then a reconnect the next request rides on. */
    atomic_store(&o.hold, 1);
    struct peer_crypto_request r3 = request(3, PEER_CRYPTO_BLOCK, 0);
    assert(a.submit(a.ctx, &r3) == 1);
    for (unsigned i = 0; i < 50; i++) nap();
    atomic_store(&o.drop_client, 1); atomic_store(&o.hold, 0);
    int got = wait_completion(&a, &d);
    if (!got || d.operation != 3 || d.status != -2 || d.action != PEER_CRYPTO_BLOCK)
        fprintf(stderr, "owner-loss: got=%d op=%llu status=%d action=%d\n", got,
                (unsigned long long)d.operation, d.status, (int)d.action);
    assert(got && d.operation == 3 && d.status == -2 && d.action == PEER_CRYPTO_BLOCK);
    assert(!a.healthy(a.ctx));
    assert(a.submit(a.ctx, &r3) == -1);
    for (unsigned i = 0; i < 1500 && !a.healthy(a.ctx); i++) nap();
    assert(a.healthy(a.ctx));
    struct peer_crypto_request r4 = request(4, PEER_CRYPTO_REMOVE_ALL, 0);
    assert(a.submit(a.ctx, &r4) == 1);
    assert(wait_completion(&a, &d) && d.operation == 4 && d.status == 0);
    /* Bounded outstanding requests: the 257th is backpressure, not loss. */
    atomic_store(&o.hold, 1);
    unsigned accepted = 0;
    for (unsigned i = 0; i < 300; i++) {
        struct peer_crypto_request r = request(100 + i, PEER_CRYPTO_INSTALL_RX, 0x22);
        int rc = a.submit(a.ctx, &r);
        assert(rc >= 0);
        if (rc == 1) accepted++; else break;
    }
    assert(accepted >= 8 && accepted <= 256);
    atomic_store(&o.hold, 0);
    for (unsigned i = 0; i < accepted; i++) assert(wait_completion(&a, &d) && d.status == 0);
    assert(atomic_load(&o.served) == 2 + 1 + accepted);
    atomic_store(&o.stop, 1); pthread_join(o.thread, NULL);
    close(o.listen_fd); unlink(o.path);
    peer_crypto_ipc_free(ipc);
    puts("peer_crypto_ipc_test: PASS (codec, round trip, refusal, owner loss -> unknown outcome, reconnect, backpressure)");
    return 0;
}
