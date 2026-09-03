/* Host-only contract coverage for the LD_PRELOAD/native boundary. The production
 * shim is included directly so the test exercises its real POSIX state machine;
 * native calls are deterministic fakes and require no DOCA device. */
#define DMESH_PRELOAD_TEST 1
#include "../src/facade/dmesh_preload.c"

#include <assert.h>
#include <stdatomic.h>

static dmesh_channel_t fake_channel = {
    .pod_id = 1,
    .slot_size = 8,
    .block_size = 4,
};
static dmesh_qp_t fake_qp = {
    .ep = &fake_channel,
    .role = DMESH_ROLE_CLIENT,
    .local_port = 7,
};
static uint8_t fake_reservation[64];
static uint8_t fake_posted[256];
static atomic_int fake_alloc_calls;
static atomic_int fake_fail_allocs;
static atomic_int fake_fail_forever;
static atomic_int fake_post_calls;
static atomic_int fake_flush_calls;
static atomic_int fake_fail_fin;
static atomic_int fake_inflight;
static atomic_int fake_release_calls;
static atomic_int fake_suppress_depth;
static atomic_int fake_suppress_calls;
static atomic_int fake_poll_active;
static atomic_int fake_poll_max_active;
static atomic_int fake_poll_delay_us;
static atomic_int fake_destroy_calls;
static atomic_int fake_destroy_during_poll;
static size_t fake_posted_len;
static dmesh_event_t fake_events[16];
static int fake_event_count;
static int fake_event_pos;

static void fake_reset(void) {
    atomic_store(&fake_alloc_calls, 0);
    atomic_store(&fake_fail_allocs, 0);
    atomic_store(&fake_fail_forever, 0);
    atomic_store(&fake_post_calls, 0);
    atomic_store(&fake_flush_calls, 0);
    atomic_store(&fake_fail_fin, 0);
    atomic_store(&fake_inflight, 0);
    atomic_store(&fake_release_calls, 0);
    atomic_store(&fake_suppress_depth, 0);
    atomic_store(&fake_suppress_calls, 0);
    atomic_store(&fake_poll_active, 0);
    atomic_store(&fake_poll_max_active, 0);
    atomic_store(&fake_poll_delay_us, 0);
    atomic_store(&fake_destroy_calls, 0);
    atomic_store(&fake_destroy_during_poll, 0);
    fake_posted_len = 0;
    fake_event_count = 0;
    fake_event_pos = 0;
    memset(fake_posted, 0, sizeof(fake_posted));
}

/* ---- Native API fakes required by the included production shim. ---- */
int dmesh_config_listen_port(void) { return -1; }
int dmesh_resolve_addr_via(dpumesh_ctx_t *ctx, uint32_t ip_net,
                           uint16_t port_host) {
    (void)ctx; (void)ip_net; (void)port_host;
    errno = ENOENT;
    return -1;
}
void dmesh_resolve_invalidate(uint32_t ip_net, uint16_t port_host) {
    (void)ip_net; (void)port_host;
}
dmesh_channel_t *dmesh_create_channel(void) { return &fake_channel; }
int dmesh_destroy_channel(dmesh_channel_t *channel) { (void)channel; return 0; }
int dmesh_pod_id(dmesh_channel_t *channel) { return channel->pod_id; }
int dmesh_msg_max(dmesh_channel_t *channel) { return channel->slot_size; }
void dpumesh_get_wait_split(dpumesh_ctx_t *ctx, unsigned long long *window,
                            unsigned long long *pool) {
    (void)ctx;
    if (window) *window = 6;
    if (pool) *pool = 7;
}
void dmesh_get_tx_stats(dmesh_channel_t *channel, dmesh_tx_stats_t *out) {
    (void)channel;
    *out = (dmesh_tx_stats_t){
        .pool_grabs = 1,
        .pool_returns = 2,
        .recycle_hits = 3,
        .grow_waits = 4,
        .block_pads = 5,
    };
}
dmesh_eq_t *dmesh_create_eq(dmesh_channel_t *channel) {
    (void)channel; return (dmesh_eq_t *)(uintptr_t)1;
}
int dmesh_destroy_eq(dmesh_eq_t *eq) { (void)eq; return 0; }
int dmesh_eq_fd(dmesh_eq_t *eq) { (void)eq; return -1; }
int64_t dmesh_eq_next_deadline_ns(dmesh_eq_t *eq) { (void)eq; return -1; }
void dmesh_eq_suppress_notify(dmesh_eq_t *eq, int delta) {
    (void)eq;
    atomic_fetch_add(&fake_suppress_depth, delta);
    atomic_fetch_add(&fake_suppress_calls, 1);
}
dmesh_qp_t *dmesh_qp_open(dmesh_eq_t *eq, int service) {
    (void)eq; (void)service; return &fake_qp;
}
int dmesh_destroy_qp(dmesh_qp_t *qp) {
    (void)qp;
    atomic_fetch_add(&fake_destroy_calls, 1);
    if (atomic_load(&fake_poll_active) != 0)
        atomic_store(&fake_destroy_during_poll, 1);
    return 0;
}
int dmesh_abort_qp(dmesh_qp_t *qp) { (void)qp; return 0; }
int dmesh_send_fin(dmesh_qp_t *qp) {
    (void)qp;
    if (atomic_load(&fake_fail_fin)) {
        errno = EIO;
        return -1;
    }
    return 0;
}
int dmesh_poll_eq(dmesh_eq_t *eq, dmesh_event_t *events, int max_events) {
    (void)eq;
    int active = atomic_fetch_add(&fake_poll_active, 1) + 1;
    int maximum = atomic_load(&fake_poll_max_active);
    while (active > maximum &&
           !atomic_compare_exchange_weak(&fake_poll_max_active, &maximum, active)) {}
    int delay = atomic_load(&fake_poll_delay_us);
    if (delay > 0) usleep((useconds_t)delay);
    int n = 0;
    while (n < max_events && fake_event_pos < fake_event_count)
        events[n++] = fake_events[fake_event_pos++];
    atomic_fetch_sub(&fake_poll_active, 1);
    return n;
}
void *dmesh_alloc(dmesh_qp_t *qp, uint32_t len) {
    (void)qp;
    atomic_fetch_add(&fake_alloc_calls, 1);
    int failures = atomic_load(&fake_fail_allocs);
    while (failures > 0 &&
           !atomic_compare_exchange_weak(&fake_fail_allocs, &failures, failures - 1)) {}
    if (failures > 0 || atomic_load(&fake_fail_forever)) {
        errno = EAGAIN;
        return NULL;
    }
    assert(len <= sizeof(fake_reservation));
    return fake_reservation;
}
int dmesh_post_send(dmesh_qp_t *qp, const void *buf, uint32_t len) {
    (void)qp;
    assert(buf == fake_reservation);
    assert(fake_posted_len + len <= sizeof(fake_posted));
    memcpy(fake_posted + fake_posted_len, buf, len);
    fake_posted_len += len;
    atomic_fetch_add(&fake_post_calls, 1);
    return 0;
}
int dmesh_flush(dmesh_qp_t *qp) {
    (void)qp;
    atomic_fetch_add(&fake_flush_calls, 1);
    atomic_store(&fake_inflight, 1);
    return 0;
}
int dmesh_tx_inflight(dmesh_qp_t *qp) {
    (void)qp;
    return atomic_load(&fake_inflight);
}
void dmesh_release_rx_buffer(dmesh_channel_t *channel, dmesh_event_t *event) {
    (void)channel;
    if (event && event->_rx_token >= 0) {
        event->_rx_token = -1;
        atomic_fetch_add(&fake_release_calls, 1);
    }
}

static pfd_t *test_pfd(void) {
    pfd_t *e = pfd_new(&fake_qp);
    assert(e != NULL);
    fake_qp.user_data = e;
    g_ch = &fake_channel;
    return e;
}

static void test_pfd_free(pfd_t *e) {
    release_rx_list(e->rx_head);
    real_close(e->sigfd);
    real_close(e->efd);
    pthread_mutex_destroy(&e->tx_mu);
    pthread_mutex_destroy(&e->mu);
    free(e);
    fake_qp.user_data = NULL;
}

static int fd_ready(pfd_t *e, short events) {
    struct pollfd p = { .fd = e->efd, .events = events };
    int n = poll(&p, 1, 0);
    return n > 0 && (p.revents & events) != 0;
}

static void fake_emit_event(dmesh_event_type_t type, dmesh_qp_t *qp) {
    assert(fake_event_count < (int)(sizeof(fake_events) /
                                         sizeof(fake_events[0])));
    fake_events[fake_event_count++] = (dmesh_event_t){
        .qp = qp,
        .type = type,
        ._rx_token = -1,
    };
}

static void test_native_chunking_delegates_batching(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    const char payload[] = "abcdefghij";
    assert(shim_send(e, payload, sizeof(payload) - 1, 0) ==
           (ssize_t)(sizeof(payload) - 1));
    assert(atomic_load(&fake_alloc_calls) == 3);
    assert(atomic_load(&fake_post_calls) == 3);
    assert(atomic_load(&fake_flush_calls) == 0);
    assert(fake_posted_len == sizeof(payload) - 1);
    assert(memcmp(fake_posted, payload, sizeof(payload) - 1) == 0);
    test_pfd_free(e);
}

static void test_nonblocking_eagain_and_pollout_edge(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    atomic_store(&fake_fail_allocs, 1);

    assert(fd_ready(e, POLLOUT));
    errno = 0;
    assert(shim_send(e, "x", 1, MSG_DONTWAIT) == -1);
    assert(errno == EAGAIN);
    assert(atomic_load(&fake_alloc_calls) == 1);
    assert(!fd_ready(e, POLLOUT));

    g_eq = (dmesh_eq_t *)(uintptr_t)1;
    fake_emit_event(DMESH_EVENT_TX_READY, &fake_qp);
    dispatcher_drain_eq(NULL, 0);
    assert(fd_ready(e, POLLOUT));
    assert(shim_send(e, "x", 1, MSG_DONTWAIT) == 1);
    assert(atomic_load(&fake_alloc_calls) == 2);
    assert(atomic_load(&fake_post_calls) == 1);
    assert(atomic_load(&fake_flush_calls) == 0);
    test_pfd_free(e);
}

struct send_thread_arg {
    pfd_t *e;
    atomic_int done;
    ssize_t result;
};

static void *blocking_sender(void *arg) {
    struct send_thread_arg *a = arg;
    a->result = shim_send(a->e, "blocking", 8, 0);
    atomic_store(&a->done, 1);
    return NULL;
}

static void test_blocking_send_waits_for_event(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    atomic_store(&fake_fail_allocs, 1);
    struct send_thread_arg arg = { .e = e, .done = 0, .result = -1 };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, blocking_sender, &arg) == 0);

    for (int i = 0; i < 1000 && atomic_load(&fake_alloc_calls) == 0; i++)
        usleep(1000);
    assert(atomic_load(&fake_alloc_calls) == 1);
    usleep(20000);
    assert(atomic_load(&fake_alloc_calls) == 1);  /* no timer/busy retry */
    assert(!atomic_load(&arg.done));

    g_eq = (dmesh_eq_t *)(uintptr_t)1;
    fake_emit_event(DMESH_EVENT_TX_READY, &fake_qp);
    dispatcher_drain_eq(NULL, 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(arg.result == 8);
    assert(atomic_load(&fake_alloc_calls) == 3);  /* two 4-byte posts after wake */
    assert(atomic_load(&fake_flush_calls) == 0);
    test_pfd_free(e);
}

static void test_send_timeout_does_not_poll_native(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    e->snd_timeout_ms = 20;
    atomic_store(&fake_fail_forever, 1);
    long start = now_ms();
    errno = 0;
    assert(shim_send(e, "x", 1, 0) == -1);
    long elapsed = now_ms() - start;
    assert(errno == EAGAIN);
    assert(elapsed >= 10 && elapsed < 500);
    assert(atomic_load(&fake_alloc_calls) == 1);
    test_pfd_free(e);
}

static void test_rx_partial_peek_credit_and_fin(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    static const uint8_t bytes[] = "abcdef";
    dmesh_event_t event = {
        .qp = &fake_qp,
        .type = DMESH_EVENT_RECV,
        .buf = bytes,
        .len = 6,
        ._rx_token = 17,
    };
    assert(pfd_queue_rx(e, &event, 0) == 0);
    assert(fd_ready(e, POLLIN));

    char out[8] = {0};
    assert(shim_recv(e, out, 2, MSG_PEEK | MSG_DONTWAIT) == 2);
    assert(memcmp(out, "ab", 2) == 0);
    assert(atomic_load(&fake_release_calls) == 0);
    memset(out, 0, sizeof(out));
    assert(shim_recv(e, out, 2, MSG_DONTWAIT) == 2);
    assert(memcmp(out, "ab", 2) == 0);
    assert(atomic_load(&fake_release_calls) == 0);
    memset(out, 0, sizeof(out));
    assert(shim_recv(e, out, 4, MSG_DONTWAIT) == 4);
    assert(memcmp(out, "cdef", 4) == 0);
    assert(atomic_load(&fake_release_calls) == 1);

    errno = 0;
    assert(shim_recv(e, out, 1, MSG_DONTWAIT) == -1);
    assert(errno == EAGAIN);
    assert(!fd_ready(e, POLLIN));
    dmesh_event_t fin = { .qp = &fake_qp, .type = DMESH_EVENT_RECV_FIN,
                       ._rx_token = -1 };
    assert(pfd_rx_fin(e, &fin, 0) == 0);
    assert(fd_ready(e, POLLIN));
    assert(shim_recv(e, out, 1, MSG_DONTWAIT) == 0);
    test_pfd_free(e);
}

static void test_data_after_fin_fails_closed(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    dmesh_event_t fin = { .qp = &fake_qp, .type = DMESH_EVENT_RECV_FIN,
                       ._rx_token = -1 };
    assert(pfd_rx_fin(e, &fin, 0) == 0);

    static const uint8_t byte[] = "x";
    dmesh_event_t data = { .qp = &fake_qp, .type = DMESH_EVENT_RECV, .buf = byte,
                        .len = 1, ._rx_token = 23 };
    assert(pfd_queue_rx(e, &data, 0) == -1);
    assert(e->io_error == EPROTO);
    assert(atomic_load(&fake_release_calls) == 1);
    char out;
    errno = 0;
    assert(shim_recv(e, &out, 1, MSG_DONTWAIT) == -1);
    assert(errno == EPROTO);
    test_pfd_free(e);
}

static void test_shutdown_read_releases_and_discards_rx(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    static const uint8_t before[] = "before";
    dmesh_event_t queued = {
        .qp = &fake_qp,
        .type = DMESH_EVENT_RECV,
        .buf = before,
        .len = sizeof(before) - 1,
        ._rx_token = 31,
    };
    assert(pfd_queue_rx(e, &queued, 0) == 0);

    int fd = real_dup(e->efd);
    assert(fd >= 0 && fd < PRELOAD_MAX_FDS);
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[fd] = e;
    e->refs = 1;
    pthread_mutex_unlock(&g_tbl_mu);

    assert(shutdown(fd, SHUT_RD) == 0);
    assert(e->rd_closed == 1);
    assert(e->rx_head == NULL && e->rx_tail == NULL);
    assert(atomic_load(&fake_release_calls) == 1);

    static const uint8_t after[] = "after";
    dmesh_event_t late = {
        .qp = &fake_qp,
        .type = DMESH_EVENT_RECV,
        .buf = after,
        .len = sizeof(after) - 1,
        ._rx_token = 32,
    };
    assert(pfd_queue_rx(e, &late, 0) == 0);
    assert(e->rx_head == NULL && e->rx_tail == NULL);
    assert(atomic_load(&fake_release_calls) == 2);

    char byte;
    assert(shim_recv(e, &byte, 1, MSG_DONTWAIT) == 0);
    errno = 0;
    assert(shutdown(fd, 99) == -1 && errno == EINVAL);

    pthread_mutex_lock(&g_tbl_mu);
    g_fds[fd] = NULL;
    e->refs = 0;
    pthread_mutex_unlock(&g_tbl_mu);
    real_close(fd);
    test_pfd_free(e);
}

static void test_failed_shutdown_write_is_sticky(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    int fd = real_dup(e->efd);
    assert(fd >= 0 && fd < PRELOAD_MAX_FDS);
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[fd] = e;
    e->refs = 1;
    pthread_mutex_unlock(&g_tbl_mu);

    atomic_store(&fake_fail_fin, 1);
    errno = 0;
    assert(shutdown(fd, SHUT_WR) == -1 && errno == ECONNRESET);
    assert(e->wr_closed == 1 && e->io_error == ECONNRESET);
    assert(fd_ready(e, POLLIN));
    errno = 0;
    assert(shim_send(e, "x", 1, MSG_DONTWAIT) == -1);
    assert(errno == ECONNRESET);

    pthread_mutex_lock(&g_tbl_mu);
    g_fds[fd] = NULL;
    e->refs = 0;
    pthread_mutex_unlock(&g_tbl_mu);
    real_close(fd);
    test_pfd_free(e);
}

static void test_fcntl_getfl_without_third_argument(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    assert(e->efd >= 0 && e->efd < PRELOAD_MAX_FDS);
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[e->efd] = e;
    e->refs = 1;
    pthread_mutex_unlock(&g_tbl_mu);
    assert((fcntl(e->efd, F_GETFL) & O_NONBLOCK) == 0);
    assert(e->active_ops == 0);
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[e->efd] = NULL;
    e->refs = 0;
    pthread_mutex_unlock(&g_tbl_mu);
    test_pfd_free(e);
}

static void test_fcntl_dupfd_cannot_escape_tracking_bound(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    assert(e->efd >= 0 && e->efd < PRELOAD_MAX_FDS);
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[e->efd] = e;
    e->refs = 1;
    pthread_mutex_unlock(&g_tbl_mu);

    errno = 0;
    assert(fcntl(e->efd, F_DUPFD_CLOEXEC, PRELOAD_MAX_FDS) == -1);
    assert(errno == EMFILE);
    assert(e->refs == 1);

    pthread_mutex_lock(&g_tbl_mu);
    g_fds[e->efd] = NULL;
    e->refs = 0;
    pthread_mutex_unlock(&g_tbl_mu);
    test_pfd_free(e);
}

static void test_install_fd_preserves_cloexec(void) {
    for (int cloexec = 0; cloexec <= 1; cloexec++) {
        fake_reset();
        pfd_t *e = test_pfd();
        int target = real_dup(e->sigfd);
        assert(target >= 0 && target < PRELOAD_MAX_FDS);
        assert(real_fcntl(target, F_SETFD, cloexec ? FD_CLOEXEC : 0) == 0);
        assert(!!(real_fcntl(target, F_GETFD) & FD_CLOEXEC) == cloexec);

        assert(install_fd(target, e) == 0);
        assert(!!(real_fcntl(target, F_GETFD) & FD_CLOEXEC) == cloexec);

        pthread_mutex_lock(&g_tbl_mu);
        assert(g_fds[target] == e && e->refs == 1);
        g_fds[target] = NULL;
        e->refs = 0;
        pthread_mutex_unlock(&g_tbl_mu);
        real_close(target);
        test_pfd_free(e);
    }
}

static void test_retire_waits_for_active_wrapper(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    assert(e->efd >= 0 && e->efd < PRELOAD_MAX_FDS);
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[e->efd] = e;
    e->refs = 1;
    pthread_mutex_unlock(&g_tbl_mu);

    pfd_t *held = pfd_get(e->efd);
    assert(held == e && e->active_ops == 1);
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[e->efd] = NULL;
    e->refs = 0;
    pthread_mutex_unlock(&g_tbl_mu);
    real_close(e->sigfd);
    real_close(e->efd);
    fake_qp.user_data = NULL;
    pfd_retire(e);
    assert(e->retired == 1);               /* storage is still live for `held` */
    pfd_put(held);                         /* drops the last reference; frees */
}

static void test_rx_fragments_preserve_order(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    static const uint8_t first[] = "a";
    static const uint8_t second[] = "b";
    dmesh_event_t a = { .qp = &fake_qp, .type = DMESH_EVENT_RECV, .buf = first,
                     .len = 1, ._rx_token = 1 };
    dmesh_event_t b = { .qp = &fake_qp, .type = DMESH_EVENT_RECV, .buf = second,
                     .len = 1, ._rx_token = 2 };
    assert(pfd_queue_rx(e, &a, 0) == 0);
    assert(pfd_queue_rx(e, &b, 0) == 0);
    char out[2];
    assert(shim_recv(e, out, sizeof(out), MSG_WAITALL | MSG_DONTWAIT) == 2);
    assert(memcmp(out, "ab", sizeof(out)) == 0);
    assert(atomic_load(&fake_release_calls) == 2);
    test_pfd_free(e);
}

static void test_dispatcher_batches_rx_signal(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    static const uint8_t bytes[] = "abcde";
    g_eq = (dmesh_eq_t *)(uintptr_t)1;
    for (int i = 0; i < 5; i++) {
        fake_events[fake_event_count++] = (dmesh_event_t){
            .qp = &fake_qp,
            .type = DMESH_EVENT_RECV,
            .buf = &bytes[i],
            .len = 1,
            ._rx_token = i + 1,
        };
    }

    dispatcher_drain_eq(NULL, 0);
    uint8_t signals[16];
    assert(real_read(e->efd, signals, sizeof(signals)) == 1);

    char out[5];
    assert(shim_recv(e, out, sizeof(out), MSG_WAITALL | MSG_DONTWAIT) == 5);
    assert(memcmp(out, bytes, sizeof(out)) == 0);
    assert(atomic_load(&fake_release_calls) == 5);
    test_pfd_free(e);
}

static void test_tx_batching_is_library_owned(void) {
    fake_reset();
    pfd_t *e = test_pfd();

    assert(shim_send(e, "a", 1, 0) == 1);
    assert(atomic_load(&fake_flush_calls) == 0);

    assert(shim_send(e, "b", 1, 0) == 1);
    assert(shim_send(e, "c", 1, 0) == 1);
    assert(atomic_load(&fake_flush_calls) == 0);
    assert(atomic_load(&fake_post_calls) == 3);
    test_pfd_free(e);
}

static void test_async_tx_error_becomes_socket_error(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    g_eq = (dmesh_eq_t *)(uintptr_t)1;
    fake_emit_event(DMESH_EVENT_TX_ERROR, &fake_qp);
    dispatcher_drain_eq(NULL, 0);
    errno = 0;
    assert(shim_send(e, "x", 1, MSG_DONTWAIT) == -1);
    assert(errno == EIO);
    assert(fd_ready(e, POLLIN));
    test_pfd_free(e);
}

static void test_blocking_recv_drains_eq(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    static const uint8_t byte = 'x';
    g_eq = (dmesh_eq_t *)(uintptr_t)1;
    fake_events[fake_event_count++] = (dmesh_event_t){
        .qp = &fake_qp,
        .type = DMESH_EVENT_RECV,
        .buf = &byte,
        .len = 1,
        ._rx_token = 1,
    };

    char out = 0;
    assert(shim_recv(e, &out, 1, 0) == 1);
    assert(out == 'x');
    assert(atomic_load(&fake_suppress_depth) == 0);
    assert(atomic_load(&fake_suppress_calls) == 2);
    assert(atomic_load(&fake_release_calls) == 1);
    test_pfd_free(e);
}

static void test_nonblocking_recv_drains_eq(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    static const uint8_t byte = 'n';
    g_eq = (dmesh_eq_t *)(uintptr_t)1;
    fake_events[fake_event_count++] = (dmesh_event_t){
        .qp = &fake_qp,
        .type = DMESH_EVENT_RECV,
        .buf = &byte,
        .len = 1,
        ._rx_token = 1,
    };

    char out = 0;
    assert(shim_recv(e, &out, 1, MSG_DONTWAIT) == 1);
    assert(out == 'n');
    assert(atomic_load(&fake_suppress_depth) == 0);
    assert(atomic_load(&fake_suppress_calls) == 2);
    assert(atomic_load(&fake_release_calls) == 1);
    test_pfd_free(e);
}

struct drain_thread_arg {
    pfd_t *e;
    int ready;
    int drained;
};

static void *drain_thread(void *arg) {
    struct drain_thread_arg *a = arg;
    a->ready = shim_try_drain(a->e, DRAIN_RX, &a->drained);
    return NULL;
}

static void wait_for_fake_poll(void) {
    for (int i = 0; i < 1000 && atomic_load(&fake_poll_active) == 0; i++)
        usleep(100);
    assert(atomic_load(&fake_poll_active) == 1);
}

static void test_eq_consumers_are_serialized(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    static const uint8_t byte = 'q';
    g_eq = (dmesh_eq_t *)(uintptr_t)1;
    atomic_store(&fake_poll_delay_us, 30000);
    fake_events[fake_event_count++] = (dmesh_event_t){
        .qp = &fake_qp,
        .type = DMESH_EVENT_RECV,
        .buf = &byte,
        .len = 1,
        ._rx_token = 1,
    };

    struct drain_thread_arg first = { .e = e };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, drain_thread, &first) == 0);
    wait_for_fake_poll();
    int second_drained = -1;
    assert(shim_try_drain(e, DRAIN_RX, &second_drained) == 0);
    assert(second_drained == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(first.ready == 1 && first.drained == 1);
    assert(atomic_load(&fake_poll_max_active) == 1);

    char out = 0;
    assert(shim_recv(e, &out, 1, MSG_DONTWAIT) == 1);
    assert(out == 'q');
    test_pfd_free(e);
}

static void test_close_waits_for_eq_consumer(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    g_eq = (dmesh_eq_t *)(uintptr_t)1;
    atomic_store(&fake_poll_delay_us, 30000);
    pthread_mutex_lock(&g_tbl_mu);
    e->active_ops = 1;
    pthread_mutex_unlock(&g_tbl_mu);
    struct drain_thread_arg arg = { .e = e };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, drain_thread, &arg) == 0);
    wait_for_fake_poll();
    dispatcher_reap_pfd(e);
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load(&fake_destroy_calls) == 1);
    assert(atomic_load(&fake_destroy_during_poll) == 0);
    assert(atomic_load(&fake_poll_max_active) == 1);
    pfd_put(e);
}

static void test_preload_tx_stats(void) {
    unsigned long long stats[7] = {0};
    g_ch = NULL;
    assert(dmesh_preload_tx_stats(stats) == 0);
    assert(stats[0] == 0 && stats[1] == 0 && stats[2] == 0);
    assert(stats[3] == 0 && stats[4] == 0 && stats[5] == 0 && stats[6] == 0);

    g_ch = &fake_channel;
    assert(dmesh_preload_tx_stats(stats) == 0);
    assert(stats[0] == 1 && stats[1] == 2 && stats[2] == 3);
    assert(stats[3] == 4 && stats[4] == 5 && stats[5] == 6 && stats[6] == 7);
    g_ch = NULL;
}

static void test_fork_child_fails_inherited_socket_closed(void) {
    fake_reset();
    pfd_t *e = test_pfd();
    int fd = real_dup(e->efd);
    assert(fd >= 0 && fd < PRELOAD_MAX_FDS);
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[fd] = e;
    e->refs = 1;
    pthread_mutex_unlock(&g_tbl_mu);

    /* Exercise the child transition directly.  The production constructor
     * registers these handlers with pthread_atfork. */
    preload_atfork_prepare();
    preload_atfork_parent();
    assert(e->active_ops == 0 && !e->fork_locked);
    preload_atfork_prepare();
    preload_atfork_child();
    assert(g_ch == NULL && g_eq == NULL && g_wake_fd == -1);
    assert(e->orphaned && e->conn == NULL && e->io_error == ECONNRESET);
    char byte;
    errno = 0;
    assert(shim_recv(e, &byte, 1, MSG_DONTWAIT) == -1);
    assert(errno == ECONNRESET);
    assert(close(fd) == 0); /* direct orphan reap; no vanished dispatcher */
    fake_qp.user_data = NULL;
}

int main(void) {
    ENSURE_REAL();
    test_preload_tx_stats();
    test_native_chunking_delegates_batching();
    test_nonblocking_eagain_and_pollout_edge();
    test_blocking_send_waits_for_event();
    test_send_timeout_does_not_poll_native();
    test_rx_partial_peek_credit_and_fin();
    test_data_after_fin_fails_closed();
    test_shutdown_read_releases_and_discards_rx();
    test_failed_shutdown_write_is_sticky();
    test_rx_fragments_preserve_order();
    test_dispatcher_batches_rx_signal();
    test_tx_batching_is_library_owned();
    test_async_tx_error_becomes_socket_error();
    test_blocking_recv_drains_eq();
    test_nonblocking_recv_drains_eq();
    test_eq_consumers_are_serialized();
    test_close_waits_for_eq_consumer();
    test_fcntl_getfl_without_third_argument();
    test_fcntl_dupfd_cannot_escape_tracking_bound();
    test_install_fd_preserves_cloexec();
    test_retire_waits_for_active_wrapper();
    test_fork_child_fails_inherited_socket_closed();
    puts("preload_api_contract_test: PASS");
    return 0;
}
