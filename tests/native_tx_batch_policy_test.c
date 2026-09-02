#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* This focused white-box test includes the production cursor implementation so it
 * can seed the otherwise-private per-QP TX state without constructing DOCA hardware. */
#include "../src/core/dmesh_core.c"

static void
seed(struct dpumesh_ctx *ctx, struct dmesh_port_slot *ports, uint8_t *dma)
{
    memset(ctx, 0, sizeof(*ctx));
    memset(ports, 0, 18 * sizeof(*ports));
    ctx->slot_size = 8192;
    ctx->block_size = 65536;
    ctx->blocks_per_conn = 2;
    ctx->dma_buffer = dma;
    ctx->ports = ports;
    atomic_init(&ctx->ports[17].tx_c, 0);
    atomic_init(&ctx->ports[17].tx_s, 0);
    for (int b = 0; b < TX_BLOCKS_PER_CONN; b++)
        atomic_init(&ctx->ports[17].blk_used[b], 0);

    struct dmesh_port_slot *psl = &ctx->ports[17];
    psl->nblk_owned = 2;
    psl->pblk[0] = 0;
    psl->pblk[1] = 1;
}

/* One CLIENT QP on port 17 over a single block, wired to a private ring and EQ.
 * Enough production state to drive the real cursor and publication paths. */
struct fixture {
    struct dpumesh_ctx *ctx;
    struct dmesh_port_slot *ports;
    struct dma_desc *descs;
    struct dma_ring_ctrl *ctrl;
    struct dma_ring ring;
    uint8_t *dma;
    uint16_t *su_seq;
    uint64_t *su_end;
    uint8_t *su_done;
    dmesh_channel_t channel;
    struct dmesh_eq eq;
    dmesh_qp_t qp;
    struct dmesh_port_slot *psl;
};

static struct fixture *
fixture_new(uint32_t ring_size, uint32_t su_depth, int notify_efd)
{
    struct fixture *f = calloc(1, sizeof(*f));
    assert(f != NULL);
    f->ctx = calloc(1, sizeof(*f->ctx));
    f->ports = calloc(18, sizeof(*f->ports));
    f->descs = calloc(ring_size, sizeof(*f->descs));
    f->ctrl = calloc(1, sizeof(*f->ctrl));
    f->dma = calloc(1, 65536);
    f->su_seq = calloc(su_depth, sizeof(*f->su_seq));
    f->su_end = calloc(su_depth, sizeof(*f->su_end));
    f->su_done = calloc(su_depth, sizeof(*f->su_done));
    assert(f->ctx && f->ports && f->descs && f->ctrl && f->dma &&
           f->su_seq && f->su_end && f->su_done);

    f->ring.size = ring_size;
    f->ring.descs = f->descs;
    f->ring.ctrl = f->ctrl;

    f->ctx->ports = f->ports;
    f->ctx->slot_size = 8192;
    f->ctx->block_size = 65536;
    f->ctx->blocks_per_conn = 1;
    f->ctx->num_slots = 8;
    f->ctx->k_rings = 1;
    f->ctx->su_depth = su_depth;
    f->ctx->inbox_ring = 8;
    f->ctx->dma_buffer = f->dma;
    f->ctx->dma_rings[0] = &f->ring;
    atomic_init(&f->ctx->tx_armed_total, 0);

    f->channel.ctx = f->ctx;
    f->eq.ch = &f->channel;
    f->eq.notify_efd = notify_efd;
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++) {
        atomic_init(&f->eq.tx_error[i], 0);
        atomic_init(&f->eq.tx_ready[i], 0);
        atomic_init(&f->eq.tx_armed[i], 0);
    }
    atomic_init(&f->eq.tx_error_count, 0);
    atomic_init(&f->eq.tx_ready_count, 0);
    atomic_init(&f->eq.tx_armed_count, 0);
    atomic_init(&f->eq.wants_notify, notify_efd >= 0 ? 1 : 0);
    atomic_init(&f->eq.suppress_notify, 0);

    f->qp.ep = &f->channel;
    f->qp.eq = &f->eq;
    f->qp.role = DMESH_ROLE_CLIENT;
    f->qp.local_port = 17;
    f->qp.dst_service = 3;

    f->psl = &f->ports[17];
    atomic_init(&f->psl->tx_c, 0);
    atomic_init(&f->psl->tx_s, 0);
    atomic_init(&f->psl->tx_f, 0);
    atomic_init(&f->psl->tx_error, 0);
    atomic_init(&f->psl->su_head, 0);
    atomic_init(&f->psl->su_tail, 0);
    for (int b = 0; b < TX_BLOCKS_PER_CONN; b++)
        atomic_init(&f->psl->blk_used[b], 0);
    f->psl->tx_deadline_ns = 0;
    f->psl->user = &f->qp;
    f->psl->eq = &f->eq;
    f->psl->nblk_owned = 1;
    f->psl->head_blk_next = 1;
    f->psl->pblk[0] = 0;
    f->psl->su_seq = f->su_seq;
    f->psl->su_end = f->su_end;
    f->psl->su_done = f->su_done;
    __atomic_store_n(&f->psl->role, DMESH_ROLE_CLIENT, __ATOMIC_RELEASE);

    /* Registered so the timer thread can find this EQ by count alone. */
    f->ctx->eqs[0] = &f->eq;
    f->ctx->n_eqs = 1;
    assert(pthread_mutex_init(&f->ctx->eq_lock, NULL) == 0);
    f->ctx->eq_lock_initialized = 1;
    assert(pthread_mutex_init(&f->ctx->port_lock, NULL) == 0);
    f->ctx->port_lock_initialized = 1;
    return f;
}

static void
fixture_free(struct fixture *f)
{
    for (int port = 0; port < 18; port++)
        free(f->ports[port].inbox);
    if (f->ctx->port_lock_initialized)
        pthread_mutex_destroy(&f->ctx->port_lock);
    if (f->ctx->eq_lock_initialized) pthread_mutex_destroy(&f->ctx->eq_lock);
    free(f->su_done);
    free(f->su_end);
    free(f->su_seq);
    free(f->dma);
    free(f->ctrl);
    free(f->descs);
    free(f->ports);
    free(f->ctx);
    free(f);
}

/* Commit `bytes` more application bytes and run the post_send publication path. */
static int
fixture_commit(struct fixture *f, uint64_t bytes)
{
    f->psl->tx_w += bytes;
    atomic_store_explicit(&f->psl->blk_used[0], (uint32_t)f->psl->tx_w,
                          memory_order_release);
    atomic_store_explicit(&f->psl->tx_c, f->psl->tx_w, memory_order_release);
    return dmesh_tx_after_commit(&f->qp);
}

/* The timer marks the EQ when a deadline may have passed; the poll path only
 * consults the clock after seeing that mark. */
static void
mark_due(struct fixture *f)
{
    atomic_store_explicit(&f->eq.tx_due_hint, 1, memory_order_release);
}

static uint_fast32_t
armed_count(struct fixture *f)
{
    return atomic_load_explicit(&f->eq.tx_armed_count, memory_order_acquire);
}

struct delayed_ack {
    struct fixture *fixture;
    uint16_t seq;
};

static void *
publish_delayed_ack(void *opaque)
{
    struct delayed_ack *ack = opaque;
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
    (void)nanosleep(&delay, NULL);
    tx_reclaim_ack(ack->fixture->ctx, 17, ack->seq);
    return NULL;
}

/* A FIN needs no source DMA, so merely enqueueing it behind data is not an
 * ordering fence. The production close path waits for the data custody ACK
 * before it publishes the FIN descriptor. */
static void
test_fin_waits_for_submitted_data(void)
{
    struct fixture *f = fixture_new(8, 16, -1);
    assert(fixture_commit(f, 64) == 0);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 1);
    assert(dmesh_tx_inflight(&f->qp));

    struct delayed_ack ack = { .fixture = f, .seq = 1 };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, publish_delayed_ack, &ack) == 0);
    uint64_t started = monotonic_ns();
    assert(dmesh_send_fin(&f->qp) == 0);
    uint64_t elapsed = monotonic_ns() - started;
    assert(pthread_join(thread, NULL) == 0);

    assert(elapsed >= 5000000ull);
    /* The payload ACK drained, but FIN itself now holds the port incarnation. */
    assert(dmesh_tx_inflight(&f->qp));
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 2);
    assert(f->descs[1].size == 0);
    assert(f->descs[1].seq == 2);
    assert(f->qp.fin_sent == 1);
    tx_reclaim_ack(f->ctx, 17, 2);
    assert(!dmesh_tx_inflight(&f->qp));
    fixture_free(f);
}

/* Abort is the bounded-close escape hatch: it is ordered on the forward ring
 * but does not wait for destination custody to return. */
static void
test_abort_marker_does_not_wait_for_submitted_data(void)
{
    struct fixture *f = fixture_new(8, 16, -1);
    assert(fixture_commit(f, 64) == 0);
    assert(dmesh_tx_inflight(&f->qp));

    uint64_t started = monotonic_ns();
    assert(dmesh_send_abort_locked(&f->qp) == 0);
    uint64_t elapsed = monotonic_ns() - started;

    assert(elapsed < 5000000ull);
    assert(dmesh_tx_inflight(&f->qp));
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 2);
    assert(f->descs[1].size == 0);
    assert(f->descs[1].dst_pod_id == DMESH_POD_ABORT);
    assert(f->descs[1].seq == 2);
    tx_reclaim_ack(f->ctx, 17, 1);
    assert(dmesh_tx_inflight(&f->qp));       /* reset still quarantines the port */
    tx_reclaim_ack(f->ctx, 17, 2);
    assert(!dmesh_tx_inflight(&f->qp));
    fixture_free(f);
}

/* A closed client port remains unavailable until the DPU acknowledges the
 * control descriptor that retires its old (pod, port) key. This prevents a new
 * protocol-aware session from being spliced into a refused session whose
 * Linkerd task is still finishing. */
static void
test_close_ack_quarantines_port_reuse(void)
{
    struct fixture *f = fixture_new(8, 16, -1);
    f->ctx->port_span = 18;
    f->ctx->next_port = 17;

    assert(dmesh_send_fin(&f->qp) == 0);
    assert(dmesh_tx_inflight(&f->qp));

    /* Model dmesh_release_qp after it publishes FREE. There are no data blocks
     * in this FIN-only case, so only the control FIFO can hold the incarnation. */
    __atomic_store_n(&f->psl->role, DMESH_ROLE_FREE, __ATOMIC_RELEASE);
    f->psl->user = NULL;
    f->psl->eq = NULL;
    f->psl->nblk_owned = 0;

    int other_owner = 1;
    uint16_t other = dpumesh_alloc_port(f->ctx, DMESH_ROLE_CLIENT,
                                        &other_owner, &f->eq);
    assert(other != 0 && other != 17);

    tx_reclaim_ack(f->ctx, 17, 1);
    assert(!dmesh_tx_inflight(&f->qp));
    f->ctx->next_port = 17;
    int fresh_owner = 2;
    assert(dpumesh_alloc_port(f->ctx, DMESH_ROLE_CLIENT,
                              &fresh_owner, &f->eq) == 17);
    fixture_free(f);
}

static void
test_tail_publication_policy(void)
{
    struct fixture *f = fixture_new(8, 16, -1);
    struct dmesh_port_slot *psl = f->psl;

    /* An idle stream has no outstanding unit for a successor to arrive behind,
     * so its first partial publishes immediately and nothing is retained. */
    assert(fixture_commit(f, 64) == 0);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 1);
    assert(f->descs[0].size == 64);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_acquire) == 128);
    assert(psl->tx_w == 128);
    assert(armed_count(f) == 0);
    assert(dmesh_eq_next_deadline_ns(&f->eq) == -1);

    /* That unit is now in flight, so the next partial is retained instead. */
    assert(fixture_commit(f, 64) == 0);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 1);
    assert(armed_count(f) == 1);
    uint64_t deadline = psl->tx_deadline_ns;
    assert(deadline > monotonic_ns());
    int64_t wait_ns = dmesh_eq_next_deadline_ns(&f->eq);
    assert(wait_ns > 0 && wait_ns <= (int64_t)TX_TAIL_DELAY_NS);

    /* Continued commits neither publish the tail early nor push its deadline
     * out: the stamp is taken once per retention. */
    assert(fixture_commit(f, 64) == 0);
    assert(psl->tx_deadline_ns == deadline);
    assert(armed_count(f) == 1);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 1);

    /* Without the timer's mark the poll pass does not even read the clock. */
    dpumesh_publish_due_tails(&f->eq);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 1);
    assert(armed_count(f) == 1);

    /* Marked, but before the deadline, the tail is still left alone. */
    mark_due(f);
    dpumesh_publish_due_tails(&f->eq);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 1);
    assert(armed_count(f) == 1);

    /* Once it expires, the owner publishes it from its own poll pass. */
    psl->tx_deadline_ns = monotonic_ns() - 1;
    mark_due(f);
    dpumesh_publish_due_tails(&f->eq);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 2);
    assert(f->descs[1].size == 128);          /* both retained commits, coalesced */
    assert(armed_count(f) == 0);
    assert(dmesh_eq_next_deadline_ns(&f->eq) == -1);

    /* An explicit flush forces a retained tail without waiting. */
    assert(fixture_commit(f, 64) == 0);
    assert(armed_count(f) == 1);
    assert(dmesh_flush(&f->qp) == 0);
    assert(armed_count(f) == 0);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 3);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_acquire) == psl->tx_w);

    /* A deferred failure is a one-shot EQ event but stays sticky on the QP. */
    tx_error_publish(psl, 17, EIO);
    assert(dpumesh_next_tx_error(&f->eq) == &f->qp);
    assert(dpumesh_next_tx_error(&f->eq) == NULL);
    errno = 0;
    assert(dmesh_tx_qp_valid(&f->qp) == -1 && errno == EIO);

    fixture_free(f);
}

/* A commit that publishes every committed byte clears the armed bit while the
 * stamp holds the stream in coalescing mode. A partial committed after that is
 * retained by the stamp alone; once the stream falls quiet, the acknowledgement
 * that empties the QP arms it. */
static void
test_tail_retained_when_the_stream_falls_quiet(void)
{
    struct fixture *f = fixture_new(8, 16, -1);
    struct dmesh_port_slot *psl = f->psl;

    assert(fixture_commit(f, 64) == 0);       /* idle → published, tx_s = 128 */
    assert(fixture_commit(f, 64) == 0);       /* in flight → retained + armed */
    assert(armed_count(f) == 1);

    /* Commit exactly one transport unit past the send cursor: the drain ships
     * all of it, so tx_s catches tx_w with earlier units still un-ACKed. */
    assert(fixture_commit(f, 8128) == 0);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 2);
    assert(f->descs[1].size == 8192);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_acquire) == psl->tx_w);
    assert(armed_count(f) == 0);              /* nothing retained right now ... */
    assert(psl->tx_deadline_ns != 0);         /* ... but still coalescing */

    /* One more partial, then the writer stops. It is retained, unarmed. */
    assert(fixture_commit(f, 64) == 0);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 2);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_relaxed) <
           atomic_load_explicit(&psl->tx_c, memory_order_acquire));

    /* The two shipped units are acknowledged; the QP goes fully idle. */
    tx_reclaim_ack(f->ctx, 17, 1);
    assert(armed_count(f) == 0);              /* one unit still outstanding */
    tx_reclaim_ack(f->ctx, 17, 2);
    assert(armed_count(f) == 1);              /* idle with a tail → armed */

    /* Reachable: once its deadline passes, the deadline pass alone publishes
     * it, with no further commit, flush or allocation pressure. */
    psl->tx_deadline_ns = monotonic_ns() - 1;
    mark_due(f);
    dpumesh_publish_due_tails(&f->eq);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 3);
    assert(f->descs[2].size == 64);
    assert(armed_count(f) == 0);
    assert(psl->tx_deadline_ns == 0);

    fixture_free(f);
}

/* The final ACK may make a stamped stream idle before the next partial commit.
 * No later ACK then exists to arm that tail, so the commit itself must recognize
 * the idle stream instead of retaining bytes behind the stale stamp. */
static void
test_tail_committed_after_the_stream_goes_idle(void)
{
    struct fixture *f = fixture_new(8, 16, -1);
    struct dmesh_port_slot *psl = f->psl;

    assert(fixture_commit(f, 64) == 0);       /* idle -> published */
    assert(fixture_commit(f, 64) == 0);       /* in flight -> retained */
    assert(fixture_commit(f, 8128) == 0);     /* complete unit -> published */
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 2);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_acquire) == psl->tx_w);
    assert(psl->tx_deadline_ns != 0);         /* coalescing stamp survives */

    tx_reclaim_ack(f->ctx, 17, 1);
    tx_reclaim_ack(f->ctx, 17, 2);
    assert(!dmesh_tx_inflight_locked(psl));
    assert(armed_count(f) == 0);              /* no tail existed at ACK time */
    assert(psl->tx_deadline_ns != 0);         /* owner has not cleared the stamp */

    assert(fixture_commit(f, 64) == 0);       /* later idle tail -> immediate */
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 3);
    assert(f->descs[2].size == 64);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_acquire) == psl->tx_w);
    assert(armed_count(f) == 0);
    assert(psl->tx_deadline_ns == 0);

    fixture_free(f);
}

/* The deadline pass and a public TX call can land on the same QP from different
 * threads; the gate keeps exactly one of them in the transmit state. */
static void
test_deadline_pass_yields_to_an_active_tx_call(void)
{
    struct fixture *f = fixture_new(8, 16, -1);

    assert(fixture_commit(f, 64) == 0);      /* idle -> published */
    assert(fixture_commit(f, 64) == 0);      /* in flight -> retained */
    assert(armed_count(f) == 1);
    f->psl->tx_deadline_ns = monotonic_ns() - 1;

    /* Gate held, as it is between dmesh_alloc and dmesh_post_send. */
    assert(dmesh_tx_qp_valid(&f->qp) == 0);
    mark_due(f);
    dpumesh_publish_due_tails(&f->eq);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 1);
    assert(armed_count(f) == 1);             /* retention survives the pass */

    /* The owner's own call publishes it instead. */
    dmesh_tx_call_done(&f->qp);
    assert(dmesh_flush(&f->qp) == 0);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 2);
    assert(armed_count(f) == 0);

    fixture_free(f);
}

static void
test_close_paths_release_the_armed_bit(void)
{
    struct fixture *f = fixture_new(8, 16, -1);

    assert(fixture_commit(f, 64) == 0);      /* idle → published */
    assert(fixture_commit(f, 64) == 0);      /* in flight → retained */
    assert(armed_count(f) == 1);
    assert(atomic_load_explicit(&f->ctx->tx_armed_total,
                                memory_order_acquire) == 1);

    /* Freeing the port drops the EQ-side record while the binding is still
     * valid, so a later timer pass cannot see a slot that no longer exists. */
    assert(pthread_mutex_init(&f->ctx->port_lock, NULL) == 0);
    f->ctx->port_lock_initialized = 1;
    dpumesh_free_port(f->ctx, 17);
    assert(armed_count(f) == 0);
    assert(atomic_load_explicit(&f->ctx->tx_armed_total,
                                memory_order_acquire) == 0);
    assert(__atomic_load_n(&f->psl->eq, __ATOMIC_ACQUIRE) == NULL);
    pthread_mutex_destroy(&f->ctx->port_lock);
    f->ctx->port_lock_initialized = 0;

    fixture_free(f);
}

static void
test_timer_wakes_only_armed_eqs(void)
{
    int efd = eventfd(0, EFD_NONBLOCK);
    assert(efd >= 0);
    struct fixture *f = fixture_new(8, 16, efd);

    assert(tx_timer_start(f->ctx) == 0);

    /* Nothing retained: the timer stays parked and writes no readiness. */
    usleep(4 * TX_TIMER_TICK_NS / 1000);
    uint64_t drained = 0;
    assert(read(efd, &drained, sizeof drained) < 0 && errno == EAGAIN);

    assert(fixture_commit(f, 64) == 0);      /* idle → published */
    assert(fixture_commit(f, 64) == 0);      /* in flight → retained */
    assert(armed_count(f) == 1);

    /* Retained: the owner's EQ is woken so it can publish its own tail. The
     * timer never submits and never touches the port slot itself. */
    int woken = 0;
    for (int i = 0; i < 200 && !woken; i++) {
        if (read(efd, &drained, sizeof drained) > 0) woken = 1;
        else usleep(1000);
    }
    assert(woken);
    assert(armed_count(f) == 1);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 1);

    /* The wake is what lets the owner run its publication path. */
    f->psl->tx_deadline_ns = monotonic_ns() - 1;
    mark_due(f);
    dpumesh_publish_due_tails(&f->eq);
    assert(__atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE) == 2);
    assert(armed_count(f) == 0);

    tx_timer_stop(f->ctx);
    fixture_free(f);
    close(efd);
}

static void
test_sustained_commits_preserve_stream_bytes(void)
{
    enum { messages = 200, ring_size = 512 };
    struct fixture *f = fixture_new(ring_size, ring_size, -1);
    assert(tx_timer_start(f->ctx) == 0);

    /* Commits continue across many deadlines while the timer runs. Publication
     * stays on this thread, either from post_send or from the poll pass. */
    for (int i = 0; i < messages; i++) {
        assert(fixture_commit(f, 64) == 0);
        mark_due(f);
        dpumesh_publish_due_tails(&f->eq);
        usleep(50);
    }
    assert(dmesh_flush(&f->qp) == 0);
    tx_timer_stop(f->ctx);

    uint64_t tickets = __atomic_load_n(&f->ring.enq_pos, __ATOMIC_ACQUIRE);
    uint64_t bytes = 0;
    for (uint64_t i = 0; i < tickets; i++) {
        assert(__atomic_load_n(&f->descs[i].publish_seq, __ATOMIC_ACQUIRE) ==
               i + 1);
        bytes += f->descs[i].size;
    }
    assert(bytes == (uint64_t)messages * 64);
    assert(atomic_load_explicit(&f->psl->tx_s, memory_order_acquire) ==
           f->psl->tx_w);
    assert((f->psl->tx_w & (DPA_DMA_COPY_ALIGN - 1u)) == 0);
    assert(armed_count(f) == 0);

    fixture_free(f);
}

static uint8_t
stream_pattern(uint64_t offset)
{
    uint64_t x = offset * 0x9e3779b97f4a7c15ull + 0xd1b54a32d192ed03ull;
    x ^= x >> 29;
    return (uint8_t)x;
}

/* One TX_ACK entry names a run of consecutive sequences. The reverse-ring drain
 * reclaims every unit in it; a zero count names one. */
static void
test_range_ack_reclaims_the_whole_run(void)
{
    struct fixture *f = fixture_new(8, 16, -1);
    struct dmesh_port_slot *psl = f->psl;
    struct dmesh_rev_ring_entry *entries = calloc(4, sizeof(*entries));
    struct dmesh_rev_ring_ctrl *rctrl = calloc(1, sizeof(*rctrl));
    assert(entries != NULL && rctrl != NULL);
    struct rev_ring rev = { .size = 4, .entries = entries, .ctrl = rctrl };
    f->ctx->landing_stripes = 1;
    f->ctx->rev_rings[0] = &rev;

    for (uint16_t s = 1; s <= 4; s++)
        dpumesh_tx_track(f->ctx, 17, s, 8192);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_relaxed) == 4u * 8192u);

    entries[0].kind = DMESH_REV_ENTRY_TX_ACK;
    entries[0].payload.ack.port = 17;
    entries[0].payload.ack.seq = 1;
    entries[0].payload.ack.seq_count = 4;
    __atomic_store_n(&entries[0].publish_seq, 1u, __ATOMIC_RELEASE);
    assert(drain_rev_rings_span(f->ctx, 0, 1, 64) == 1);
    assert(atomic_load_explicit(&psl->tx_f, memory_order_acquire) == 4u * 8192u);
    assert(atomic_load_explicit(&psl->su_tail, memory_order_acquire) ==
           atomic_load_explicit(&psl->su_head, memory_order_acquire));
    assert(rev.head == 1 && rctrl->consumer_head == 1);

    for (uint16_t s = 5; s <= 6; s++)
        dpumesh_tx_track(f->ctx, 17, s, 8192);
    entries[1].kind = DMESH_REV_ENTRY_TX_ACK;
    entries[1].payload.ack.port = 17;
    entries[1].payload.ack.seq = 5;
    entries[1].payload.ack.seq_count = 0;
    __atomic_store_n(&entries[1].publish_seq, 2u, __ATOMIC_RELEASE);
    assert(drain_rev_rings_span(f->ctx, 0, 1, 64) == 1);
    assert(atomic_load_explicit(&psl->tx_f, memory_order_acquire) == 5u * 8192u);
    assert((uint16_t)(atomic_load_explicit(&psl->su_head, memory_order_acquire) -
                      atomic_load_explicit(&psl->su_tail, memory_order_acquire)) == 1);

    f->ctx->rev_rings[0] = NULL;
    f->ctx->landing_stripes = 0;
    free(rctrl);
    free(entries);
    fixture_free(f);
}

static void
test_large_commits_preserve_stream_bytes(void)
{
    enum {
        port = 17,
        block_size = 512 * 1024,
        blocks = 8,
        slot_size = 8192,
        ring_size = 4096,
        commits = 2048,
        commit_size = 32 * 1024 + 513,
        ack_window = 256,
    };

    struct dpumesh_ctx *ctx = calloc(1, sizeof(*ctx));
    struct dmesh_port_slot *ports = calloc(port + 1, sizeof(*ports));
    struct dma_desc *descs = calloc(ring_size, sizeof(*descs));
    struct dma_ring_ctrl *ctrl = calloc(1, sizeof(*ctrl));
    uint8_t *dma = calloc(blocks, block_size);
    uint32_t *block_next = calloc(blocks, sizeof(*block_next));
    uint16_t *su_seq = calloc(512, sizeof(*su_seq));
    uint64_t *su_end = calloc(512, sizeof(*su_end));
    uint8_t *su_done = calloc(512, sizeof(*su_done));
    uint16_t pending[512] = {0};
    assert(ctx && ports && descs && ctrl && dma && block_next &&
           su_seq && su_end && su_done);

    struct dma_ring ring = {
        .size = ring_size,
        .descs = descs,
        .ctrl = ctrl,
    };
    ctx->ports = ports;
    ctx->slot_size = slot_size;
    ctx->block_size = block_size;
    ctx->blocks_per_conn = blocks;
    ctx->n_blocks = blocks;
    ctx->num_slots = blocks * block_size / slot_size;
    ctx->k_rings = 1;
    ctx->su_depth = 512;
    ctx->recycle_reserve = 1;
    ctx->dma_buffer = dma;
    ctx->block_next = block_next;
    ctx->dma_rings[0] = &ring;
    for (int i = 0; i < blocks; i++)
        block_next[i] = (uint32_t)(i + 1);
    atomic_init(&ctx->block_free, 0);
    atomic_init(&ctx->pool_epoch, 0);
    atomic_init(&ctx->pool_waiter_count, 0);
    atomic_init(&ctx->pool_wait_cursor, 0);
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++)
        atomic_init(&ctx->pool_waiters[i], 0);

    dmesh_channel_t channel = { .ctx = ctx };
    struct dmesh_eq eq = { .ch = &channel, .notify_efd = -1 };
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++) {
        atomic_init(&eq.tx_error[i], 0);
        atomic_init(&eq.tx_ready[i], 0);
    }
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++)
        atomic_init(&eq.tx_armed[i], 0);
    atomic_init(&eq.tx_error_count, 0);
    atomic_init(&eq.tx_ready_count, 0);
    atomic_init(&eq.tx_armed_count, 0);
    atomic_init(&eq.tx_due_hint, 0);
    atomic_init(&eq.wants_notify, 0);
    atomic_init(&eq.suppress_notify, 0);

    dmesh_qp_t qp = {
        .ep = &channel,
        .eq = &eq,
        .role = DMESH_ROLE_CLIENT,
        .local_port = port,
        .dst_service = 3,
    };
    struct dmesh_port_slot *psl = &ports[port];
    atomic_init(&psl->tx_c, 0);
    atomic_init(&psl->tx_s, 0);
    atomic_init(&psl->tx_f, 0);
    atomic_init(&psl->tx_error, 0);
    atomic_init(&psl->tx_wait_state, DMESH_TX_WAIT_IDLE);
    atomic_init(&psl->tx_wait_reason, DMESH_TX_WAIT_NONE);
    atomic_init(&psl->tx_wait_tail_blk, 0);
    atomic_init(&psl->tx_wait_tx_w, 0);
    atomic_init(&psl->tx_wait_pool_epoch, 0);
    atomic_init(&psl->su_head, 0);
    atomic_init(&psl->su_tail, 0);
    for (int i = 0; i < TX_BLOCKS_PER_CONN; i++) {
        atomic_init(&psl->blk_used[i], 0);
        psl->pblk[i] = -1;
    }
    psl->user = &qp;
    psl->eq = &eq;
    psl->su_seq = su_seq;
    psl->su_end = su_end;
    psl->su_done = su_done;
    __atomic_store_n(&psl->role, DMESH_ROLE_CLIENT, __ATOMIC_RELEASE);
    atomic_init(&ctx->tx_armed_total, 0);
    ctx->eqs[0] = &eq;
    ctx->n_eqs = 1;
    assert(pthread_mutex_init(&ctx->eq_lock, NULL) == 0);
    ctx->eq_lock_initialized = 1;
    assert(tx_timer_start(ctx) == 0);

    uint64_t source_offset = 0;
    uint64_t copied_offset = 0;
    uint64_t ticket = 0;
    uint32_t ack_head = 0, ack_tail = 0;

#define CONSUME_PUBLISHED() do {                                                \
        for (;;) {                                                             \
            struct dma_desc *desc = &descs[ticket % ring_size];                \
            if (__atomic_load_n(&desc->publish_seq, __ATOMIC_ACQUIRE) !=       \
                    ticket + 1)                                                \
                break;                                                         \
            assert(desc->size > 0 && desc->size <= slot_size);                 \
            assert(dpa_dma_aligned_copy_len(desc->addr, desc->size) <=        \
                   DPA_DMA_COPY_MAX);                                         \
            const uint8_t *src = (const uint8_t *)ctx->dma_buffer +           \
                                 (size_t)desc->addr;                          \
            for (uint32_t j = 0; j < desc->size; j++)                          \
                assert(src[j] == stream_pattern(copied_offset + j));            \
            copied_offset += desc->size;                                       \
            pending[ack_head++ & 511u] = desc->seq;                            \
            ticket++;                                                          \
            __atomic_store_n(&ctrl->consumer_head, ticket, __ATOMIC_RELEASE);  \
        }                                                                      \
        while (ack_head - ack_tail > ack_window)                               \
            tx_reclaim_ack(ctx, port, pending[ack_tail++ & 511u]);             \
    } while (0)

    for (int i = 0; i < commits; i++) {
        uint8_t *dst;
        for (;;) {
            dst = dpumesh_tx_reserve(ctx, port, commit_size);
            if (dst != NULL) break;
            assert(errno == EAGAIN);
            dmesh_tx_pressure(&qp);
            CONSUME_PUBLISHED();
            while (ack_tail != ack_head)
                tx_reclaim_ack(ctx, port, pending[ack_tail++ & 511u]);
            sched_yield();
        }
        for (uint32_t j = 0; j < commit_size; j++)
            dst[j] = stream_pattern(source_offset + j);
        assert(dpumesh_tx_commit(ctx, port, dst, commit_size) == 0);
        assert(dmesh_tx_after_commit(&qp) == 0);
        atomic_store_explicit(&eq.tx_due_hint, 1, memory_order_release);
        dpumesh_publish_due_tails(&eq);
        source_offset += commit_size;
        CONSUME_PUBLISHED();
    }
    assert(dmesh_flush(&qp) == 0);
    for (int wait = 0; copied_offset != source_offset && wait < 1000; wait++) {
        CONSUME_PUBLISHED();
        if (copied_offset != source_offset)
            usleep(1000);
    }
    CONSUME_PUBLISHED();
    assert(copied_offset == source_offset);
    while (ack_tail != ack_head)
        tx_reclaim_ack(ctx, port, pending[ack_tail++ & 511u]);
    assert(atomic_load_explicit(&psl->tx_f, memory_order_acquire) == psl->tx_w);

#undef CONSUME_PUBLISHED
    tx_timer_stop(ctx);
    pthread_mutex_destroy(&ctx->eq_lock);
    free(su_done);
    free(su_end);
    free(su_seq);
    free(block_next);
    free(dma);
    free(ctrl);
    free(descs);
    free(ports);
    free(ctx);
}

static void
test_broker_doorbell_forwarding(void)
{
    struct dpumesh_ctx *ctx = calloc(1, sizeof(*ctx));
    assert(ctx != NULL);
    ctx->broker_doorbell_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(ctx->broker_doorbell_efd >= 0);

    /* No new REV_DOORBELL since the last forward: no tick. */
    ctx->broker_seen_doorbells = 0;
    broker_forward_doorbells(ctx);
    uint64_t count = 0;
    errno = 0;
    assert(read(ctx->broker_doorbell_efd, &count, sizeof(count)) < 0);
    assert(errno == EAGAIN);

    /* A batch of doorbells collapses into one tick, and forwarding is
     * idempotent until the count moves again. */
    __atomic_store_n(&ctx->doca_objs.rev_doorbell_count, 3, __ATOMIC_RELEASE);
    broker_forward_doorbells(ctx);
    broker_forward_doorbells(ctx);
    assert(read(ctx->broker_doorbell_efd, &count, sizeof(count)) ==
           (ssize_t)sizeof(count));
    assert(count == 1);

    __atomic_store_n(&ctx->doca_objs.rev_doorbell_count, 4, __ATOMIC_RELEASE);
    broker_forward_doorbells(ctx);
    assert(read(ctx->broker_doorbell_efd, &count, sizeof(count)) ==
           (ssize_t)sizeof(count));
    assert(count == 1);

    close(ctx->broker_doorbell_efd);
    free(ctx);
}

int
main(void)
{
    /* Production defaults: a 64 MiB shared pool, 512 KiB contiguous extents,
     * and eight lazily-owned extents (4 MiB/QP). Small-record provisioning is
     * capped at half the forward ring so one QP reaches explicit admission
     * before it can monopolize the shared publication ring. */
    struct dpumesh_ctx *defaults = calloc(1, sizeof(*defaults));
    assert(defaults != NULL);
    init_config(defaults, NULL, NULL);
    assert(defaults->num_slots == 8192);
    assert(defaults->slot_size == 8192);
    assert(defaults->block_size == 512 * 1024);
    assert(defaults->n_blocks == 128);
    assert(defaults->blocks_per_conn == 8);
    assert(defaults->su_depth == 2048);
    assert(defaults->su_depth * 2u == DMA_RING_SIZE);
    free(defaults);

    /* The public channel uses the default slot count. A non-power-of-two K
     * must still yield a valid whole-credit geometry without exceeding 64 MiB. */
    assert(setenv("DPUMESH_RINGS_PER_POD", "12", 1) == 0);
    defaults = calloc(1, sizeof(*defaults));
    assert(defaults != NULL);
    init_config(defaults, NULL, NULL);
    assert(defaults->num_slots == 8184);
    assert((defaults->num_slots * defaults->slot_size) %
           (12 * DPUMESH_SLOT_SIZE) == 0);
    free(defaults);
    assert(unsetenv("DPUMESH_RINGS_PER_POD") == 0);

    struct dmesh_port_slot rx = {0};
    sw_descriptor_t d = { .seq = 7, .body_buf_slot = 100, .body_len = 10 };
    assert(rx_seq_accept(&rx, &d));
    d.body_buf_slot = 110;
    d.body_len = 5;
    assert(rx_seq_accept(&rx, &d));
    d.body_buf_slot = 100;
    d.body_len = 10;
    assert(!rx_seq_accept(&rx, &d));
    d.seq = 8;
    d.body_buf_slot = 200;
    d.body_len = 0;
    assert(rx_seq_accept(&rx, &d));
    assert(!rx_seq_accept(&rx, &d));
    d.seq = 7;
    assert(!rx_seq_accept(&rx, &d));

    /* Rejected descriptors return their landing credit. */
    struct dpumesh_ctx *credit_ctx = calloc(1, sizeof(*credit_ctx));
    struct dmesh_port_slot *credit_ports = calloc(18, sizeof(*credit_ports));
    struct dma_desc *credit_descs = calloc(2, sizeof(*credit_descs));
    assert(credit_ctx != NULL && credit_ports != NULL && credit_descs != NULL);
    struct dma_ring credit_ring = {
        .size = 1,
        .descs = credit_descs,
    };
    credit_ctx->ports = credit_ports;
    credit_ctx->k_rings = 1;
    /* Full landing geometry: the credit mapping rejects an offset outside it. */
    credit_ctx->landing_stripes = 1;
    credit_ctx->rx_credit_shards = 1;
    credit_ctx->rx_region_size = 8192;
    credit_ctx->rx_dma_buf_size = 8192;
    credit_ctx->dma_rings[0] = &credit_ring;
    atomic_init(&credit_ports[17].role, DMESH_ROLE_CLIENT);
    credit_ports[17].rx_seq_valid = 1;
    credit_ports[17].rx_seq = 9;
    sw_descriptor_t stale = {
        .dst_port = 17,
        .seq = 8,
        .body_buf_slot = 0,
        .body_len = 1,
    };
    rx_deliver_desc(credit_ctx, &stale, 0);
    volatile uint64_t *returned =
        (volatile uint64_t *)(credit_descs + DMA_RING_CREDIT_SLOT(1));
    assert(*returned == 1);
    free(credit_descs);
    free(credit_ports);
    free(credit_ctx);

    struct dpumesh_ctx *geometry = calloc(1, sizeof(*geometry));
    assert(geometry != NULL);
    geometry->k_rings = 8;
    geometry->rx_dma_buf_size = 8u * DPUMESH_SLOT_SIZE;
    assert(configure_landing_geometry(geometry, 2) == DOCA_SUCCESS);
    assert(geometry->rx_region_size == 4u * DPUMESH_SLOT_SIZE);
    for (int r = 0; r < geometry->k_rings; r++) {
        struct dma_ring *ring = calloc(1, sizeof(*ring));
        assert(ring != NULL);
        ring->size = 1;
        ring->descs = calloc(2, sizeof(*ring->descs));
        assert(ring->descs != NULL);
        geometry->dma_rings[r] = ring;
    }
    for (int stripe = 0; stripe < 2; stripe++) {
        for (int shard = 0; shard < 4; shard++) {
            int pos = (int)((size_t)stripe * geometry->rx_region_size +
                            (size_t)shard * DPUMESH_SLOT_SIZE);
            assert(rx_credit_shard_index(geometry, pos) ==
                   stripe + shard * 2);
            rx_credit_return(geometry, pos);
        }
    }
    for (int r = 0; r < geometry->k_rings; r++) {
        volatile uint64_t *counter =
            (volatile uint64_t *)(geometry->dma_rings[r]->descs +
                                  DMA_RING_CREDIT_SLOT(1));
        assert(*counter == 1);
        free(geometry->dma_rings[r]->descs);
        free(geometry->dma_rings[r]);
    }
    free(geometry);

    static uint8_t dma[2 * 65536];
    struct dpumesh_ctx *ctx = calloc(1, sizeof(*ctx));
    struct dmesh_port_slot *ports = calloc(18, sizeof(*ports));
    assert(ctx != NULL);
    assert(ports != NULL);
    size_t moff = 0;
    uint32_t len = 0;
    struct dmesh_port_slot *psl;

    /* post_send mode keeps the newest fillable partial; flush mode forces it. */
    seed(ctx, ports, dma);
    psl = &ctx->ports[17];
    atomic_store_explicit(&psl->tx_c, 7000, memory_order_relaxed);
    atomic_store_explicit(&psl->blk_used[0], 7000, memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 0);
    assert(dpumesh_tx_next_send(ctx, 17, 1, &moff, &len) == 1);
    assert(moff == 0 && len == 7000);

    /* A full slot is immediately eligible, while its trailing partial remains. */
    seed(ctx, ports, dma);
    psl = &ctx->ports[17];
    atomic_store_explicit(&psl->tx_c, 9000, memory_order_relaxed);
    atomic_store_explicit(&psl->blk_used[0], 9000, memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 1);
    assert(moff == 0 && len == 8192);
    atomic_fetch_add_explicit(&psl->tx_s, len, memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 0);
    assert(dpumesh_tx_next_send(ctx, 17, 1, &moff, &len) == 1);
    assert(moff == 8192 && len == 808);

    /* An unaligned send head is capped so its aligned DPA DMA window remains
     * within one hardware operation. The following descriptor starts aligned. */
    seed(ctx, ports, dma);
    psl = &ctx->ports[17];
    atomic_store_explicit(&psl->tx_s, 64, memory_order_relaxed);
    atomic_store_explicit(&psl->tx_c, 64 + 8192, memory_order_relaxed);
    atomic_store_explicit(&psl->blk_used[0], 64 + 8192,
                          memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 1);
    assert(moff == 64 && len == 8192 - 64);
    assert(dpa_dma_aligned_copy_len(moff, len) == DPA_DMA_COPY_MAX);
    atomic_fetch_add_explicit(&psl->tx_s, len, memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 0);
    assert(dpumesh_tx_next_send(ctx, 17, 1, &moff, &len) == 1);
    assert(moff == 8192 && len == 64);
    assert(dpa_dma_aligned_copy_len(moff, len) == DPA_DMA_COPY_ALIGN);

    /* A short physical-block tail is sealed once later-block bytes commit. It must
     * ship before those later bytes even in full-only mode; only the newest partial
     * remains buffered. */
    seed(ctx, ports, dma);
    psl = &ctx->ports[17];
    atomic_store_explicit(&psl->tx_s, 7 * 8192, memory_order_relaxed);
    atomic_store_explicit(&psl->tx_c, 65536 + 1000,
                          memory_order_relaxed);
    atomic_store_explicit(&psl->blk_used[0], 60000,
                          memory_order_relaxed);
    atomic_store_explicit(&psl->blk_used[1], 1000,
                          memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 1);
    assert(moff == 7 * 8192 && len == 60000 - 7 * 8192);
    atomic_fetch_add_explicit(&psl->tx_s, len, memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 0);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_relaxed) == 65536);
    /* The production selector skipped the logical pad. */
    assert(dpumesh_tx_next_send(ctx, 17, 1, &moff, &len) == 1);
    assert(moff == 65536 && len == 1000);

    free(ports);
    free(ctx);
    test_tail_publication_policy();
    test_tail_retained_when_the_stream_falls_quiet();
    test_tail_committed_after_the_stream_goes_idle();
    test_deadline_pass_yields_to_an_active_tx_call();
    test_fin_waits_for_submitted_data();
    test_abort_marker_does_not_wait_for_submitted_data();
    test_close_ack_quarantines_port_reuse();
    test_close_paths_release_the_armed_bit();
    test_timer_wakes_only_armed_eqs();
    test_sustained_commits_preserve_stream_bytes();
    test_large_commits_preserve_stream_bytes();
    test_range_ack_reclaims_the_whole_run();
    test_broker_doorbell_forwarding();
    puts("native_tx_batch_policy_test: PASS");
    return 0;
}
