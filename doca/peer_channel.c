#include "peer_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

static const char *const PEER_REFUSAL_NAME[DMESH_PEER_REFUSE_MAX] = {
    [DMESH_PEER_OK]                  = "ok",
    [DMESH_PEER_REFUSE_INCARNATION]  = "incarnation",
    [DMESH_PEER_REFUSE_STATE]        = "state",
    [DMESH_PEER_REFUSE_MALFORMED]    = "malformed",
    [DMESH_PEER_REFUSE_NOT_ON_PEER]  = "not-on-peer",
    [DMESH_PEER_REFUSE_NO_POD]       = "no-pod",
    [DMESH_PEER_REFUSE_STREAMS]      = "streams",
    [DMESH_PEER_REFUSE_STAGING]      = "staging",
    [DMESH_PEER_REFUSE_RATE]         = "open-rate",
    [DMESH_PEER_REFUSE_HANDLE]       = "handle",
    [DMESH_PEER_REFUSE_INFLIGHT]     = "tx-inflight",
    [DMESH_PEER_REFUSE_NODE_UNBOUND] = "node-unbound",
    [DMESH_PEER_REFUSE_NODE_KEY]     = "node-key",
    [DMESH_PEER_REFUSE_TRANSPORT]    = "transport",
};

const char *dmesh_peer_refusal_name(enum dmesh_peer_refusal reason)
{
    if (reason < 0 || reason >= DMESH_PEER_REFUSE_MAX || !PEER_REFUSAL_NAME[reason])
        return "unknown";
    return PEER_REFUSAL_NAME[reason];
}

/* Every refusal is counted twice — once against the peer that caused it and
 * once for the node — because the two questions differ: whether one peer is
 * misbehaving, and whether this DPU is refusing more than it used to. */
static enum dmesh_peer_refusal
peer_refuse(struct dmesh_peer_table *table, struct dmesh_peer_channel *channel,
            enum dmesh_peer_refusal reason)
{
    if (channel)
        channel->refused[reason]++;
    table->refused[reason]++;
    if (table->ops && table->ops->event)
        table->ops->event(table->ops_ctx, dmesh_peer_refusal_name(reason));
    return reason;
}

static uint64_t peer_now(const struct dmesh_peer_table *table)
{
    return table->ops && table->ops->now_ns ? table->ops->now_ns(table->ops_ctx) : 0;
}

/* A fixed-size text field a peer supplies is only a name once it is
 * NUL-terminated inside its own bound; nothing downstream may assume it. */
static int peer_text_ok(const char *field, size_t size)
{
    for (size_t i = 0; i < size; i++)
        if (field[i] == '\0')
            return i > 0;
    return 0;
}

/* ---- node credential -------------------------------------------------- */

static int peer_public_from_private(const uint8_t seed[32], uint8_t public_key[32])
{
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    if (!key)
        return -1;
    size_t len = 32;
    int ok = EVP_PKEY_get_raw_public_key(key, public_key, &len) == 1 && len == 32;
    EVP_PKEY_free(key);
    return ok ? 0 : -1;
}

int dmesh_peer_node_key_load(const char *path, uint8_t public_key[32],
                             uint8_t seed_out[32],
                             char *error, size_t error_len)
{
#define PEER_KEY_ERROR(...)                                                    \
    do {                                                                       \
        if (error && error_len)                                                \
            snprintf(error, error_len, __VA_ARGS__);                           \
    } while (0)

    uint8_t seed[32];
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno != ENOENT) {
            PEER_KEY_ERROR("open(%s): %s", path, strerror(errno));
            return -1;
        }
        /* First boot. The private half is generated here and never leaves;
         * only the public half travels, through the node agent, into the
         * generation's node= line. */
        EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
        size_t len = sizeof(seed);
        if (!key || EVP_PKEY_get_raw_private_key(key, seed, &len) != 1 || len != sizeof(seed)) {
            EVP_PKEY_free(key);
            PEER_KEY_ERROR("cannot generate a node credential");
            return -1;
        }
        EVP_PKEY_free(key);
        int out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
        if (out < 0) {
            OPENSSL_cleanse(seed, sizeof(seed));
            PEER_KEY_ERROR("create(%s): %s", path, strerror(errno));
            return -1;
        }
        ssize_t written = write(out, seed, sizeof(seed));
        int synced = fsync(out);
        close(out);
        if (written != (ssize_t)sizeof(seed) || synced != 0) {
            OPENSSL_cleanse(seed, sizeof(seed));
            PEER_KEY_ERROR("write(%s) incomplete", path);
            return -1;
        }
    } else {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 077) != 0 || st.st_size != (off_t)sizeof(seed)) {
            close(fd);
            PEER_KEY_ERROR("%s must be a 32-byte regular file owned by uid %u, mode 0400",
                           path, (unsigned)geteuid());
            return -1;
        }
        ssize_t got = read(fd, seed, sizeof(seed));
        close(fd);
        if (got != (ssize_t)sizeof(seed)) {
            PEER_KEY_ERROR("read(%s) incomplete", path);
            return -1;
        }
    }
    int ok = peer_public_from_private(seed, public_key);
    if (ok == 0 && seed_out)
        memcpy(seed_out, seed, sizeof(seed));
    OPENSSL_cleanse(seed, sizeof(seed));
    if (ok != 0) {
        PEER_KEY_ERROR("%s does not derive a public key", path);
        return -1;
    }
    return 0;
#undef PEER_KEY_ERROR
}

int dmesh_peer_node_key_publish(const char *path, const uint8_t public_key[32])
{
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", public_key[i]);
    hex[64] = '\n';

    char temporary[4096];
    if (snprintf(temporary, sizeof(temporary), "%s.new", path) >= (int)sizeof(temporary))
        return -1;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        return -1;
    ssize_t written = write(fd, hex, sizeof(hex));
    int synced = fsync(fd);
    close(fd);
    if (written != (ssize_t)sizeof(hex) || synced != 0 || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

/* ---- table ------------------------------------------------------------ */

void dmesh_peer_table_init(struct dmesh_peer_table *table, const char *node_name,
                           const uint8_t static_public_key[32],
                           const struct dmesh_peer_transport *transport,
                           void *transport_ctx,
                           const struct dmesh_peer_ops *ops, void *ops_ctx)
{
    memset(table, 0, sizeof(*table));
    snprintf(table->node_name, sizeof(table->node_name), "%s", node_name ? node_name : "");
    if (static_public_key)
        memcpy(table->static_public_key, static_public_key, 32);
    table->transport = transport;
    table->transport_ctx = transport_ctx;
    table->ops = ops;
    table->ops_ctx = ops_ctx;
}

static void peer_channel_free(struct dmesh_peer_table *table,
                              struct dmesh_peer_channel *channel)
{
    if (channel->conn && table->transport && table->transport->close)
        table->transport->close(channel->conn);
    channel->conn = NULL;
    free(channel->handles);
    free(channel->tx);
    free(channel->rx);
    free(channel->rx_frame);
    free(channel->tx_frame);
    channel->handles = NULL;
    channel->tx = NULL;
    channel->rx = NULL;
    channel->rx_frame = NULL;
    channel->tx_frame = NULL;
    channel->rx_len = 0;
    channel->tx_len = 0;
    channel->in_use = 0;
    channel->state = DMESH_PEER_CLOSED;
}

void dmesh_peer_table_fini(struct dmesh_peer_table *table)
{
    for (uint32_t i = 0; i < DMESH_CHANNEL_MAX; i++)
        if (table->channels[i].in_use)
            peer_channel_free(table, &table->channels[i]);
}

struct dmesh_peer_channel *dmesh_peer_find(struct dmesh_peer_table *table,
                                           const char *node_name)
{
    for (uint32_t i = 0; i < DMESH_CHANNEL_MAX; i++) {
        struct dmesh_peer_channel *channel = &table->channels[i];
        if (channel->in_use && strcmp(channel->node_name, node_name) == 0)
            return channel;
    }
    return NULL;
}

int dmesh_peer_prologue(const char *local_node, const char *peer_node,
                        uint32_t incarnation, uint8_t *out, size_t out_len)
{
    int written = snprintf((char *)out, out_len, "dpumesh-peer-v1\n%s\n%s\n%u\n",
                           local_node ? local_node : "", peer_node ? peer_node : "",
                           incarnation);
    if (written < 0 || (size_t)written >= out_len)
        return -1;
    return written;
}

/* ---- lifetime --------------------------------------------------------- */

/* Losing a channel ends the streams on it. Streams are not migrated across a
 * reconnection: the transport's contract is that a connection can fail and the
 * application retries, so preserving session state across one buys nothing. */
void dmesh_peer_reset(struct dmesh_peer_table *table,
                      struct dmesh_peer_channel *channel, const char *why)
{
    if (table->ops && table->ops->source_reset)
        table->ops->source_reset(table->ops_ctx, channel, why);
    if (channel->handles) {
        for (uint32_t i = 0; i < DMESH_PEER_STREAMS_MAX; i++) {
            struct dmesh_peer_handle *handle = &channel->handles[i];
            if (!handle->in_use)
                continue;
            if (table->ops && table->ops->poison)
                table->ops->poison(table->ops_ctx, handle, why);
            channel->poisoned++;
            handle->in_use = 0;
        }
    }
    channel->handle_count = 0;
    channel->staging_bytes = 0;
    /* Everything this source pinned for the peer is released: the bytes will
     * never be acknowledged by a connection that no longer exists. */
    if (channel->tx) {
        for (uint32_t i = 0; i < DMESH_PEER_TX_SLOTS; i++) {
            struct dmesh_peer_txslot *slot = &channel->tx[i];
            if (!slot->in_use)
                continue;
            if (table->ops && table->ops->release)
                table->ops->release(table->ops_ctx, slot->kind, slot->cookie, slot->bytes);
            slot->in_use = 0;
        }
        channel->tx_free = DMESH_PEER_TX_NIL;
        for (uint32_t i = DMESH_PEER_TX_SLOTS; i-- > 0;) {
            channel->tx[i].next = channel->tx_free;
            channel->tx_free = i;
        }
    }
    channel->inflight_bytes = 0;
    channel->stalled = 0;
    channel->ack_staged = 0;
    if (channel->rx) {
        memset(channel->rx, 0,
               DMESH_PEER_TX_SLOTS * sizeof(*channel->rx));
        channel->rx_free = DMESH_PEER_TX_NIL;
        for (uint32_t i = DMESH_PEER_TX_SLOTS; i-- > 0;) {
            channel->rx[i].next = channel->rx_free;
            channel->rx_free = i;
        }
    }
    channel->rx_len = 0;
    channel->tx_len = 0;
    if (channel->conn && table->transport && table->transport->close)
        table->transport->close(channel->conn);
    channel->conn = NULL;
    channel->state = DMESH_PEER_CLOSED;
}

void dmesh_peer_transport_failed(struct dmesh_peer_table *table,
                                 struct dmesh_peer_channel *channel,
                                 const char *why)
{
    if (!table || !channel || !channel->in_use)
        return;
    peer_refuse(table, channel, DMESH_PEER_REFUSE_TRANSPORT);
    dmesh_peer_reset(table, channel, why);
}

static int peer_channel_alloc_tables(struct dmesh_peer_channel *channel)
{
    if (!channel->handles) {
        channel->handles = calloc(DMESH_PEER_STREAMS_MAX, sizeof(*channel->handles));
        if (!channel->handles)
            return -1;
    }
    if (!channel->tx) {
        channel->tx = calloc(DMESH_PEER_TX_SLOTS, sizeof(*channel->tx));
        if (!channel->tx)
            return -1;
        channel->tx_free = DMESH_PEER_TX_NIL;
        for (uint32_t i = DMESH_PEER_TX_SLOTS; i-- > 0;) {
            channel->tx[i].next = channel->tx_free;
            channel->tx_free = i;
        }
    }
    if (!channel->rx) {
        channel->rx = calloc(DMESH_PEER_TX_SLOTS, sizeof(*channel->rx));
        if (!channel->rx)
            return -1;
        channel->rx_free = DMESH_PEER_TX_NIL;
        for (uint32_t i = DMESH_PEER_TX_SLOTS; i-- > 0;) {
            channel->rx[i].next = channel->rx_free;
            channel->rx_free = i;
        }
    }
    size_t frame_cap = sizeof(struct dmesh_peer_msg_header) + DMESH_PEER_FRAME_MAX;
    if (!channel->rx_frame) {
        channel->rx_frame = malloc(frame_cap);
        if (!channel->rx_frame)
            return -1;
    }
    if (!channel->tx_frame) {
        channel->tx_frame = malloc(frame_cap);
        if (!channel->tx_frame)
            return -1;
    }
    return 0;
}

/* A bound on the channel count with idle eviction keeps a DPU's channels
 * following the peers it actually speaks to rather than the size of the
 * cluster. Beyond the bound the least recently active is evicted. */
static struct dmesh_peer_channel *peer_channel_slot(struct dmesh_peer_table *table)
{
    struct dmesh_peer_channel *oldest = NULL;
    for (uint32_t i = 0; i < DMESH_CHANNEL_MAX; i++) {
        struct dmesh_peer_channel *channel = &table->channels[i];
        if (!channel->in_use)
            return channel;
        if (channel->handle_count || channel->inflight_bytes)
            continue;
        if (!oldest || channel->last_active_ns < oldest->last_active_ns)
            oldest = channel;
    }
    if (oldest) {
        dmesh_peer_reset(table, oldest, "peer channel evicted");
        peer_channel_free(table, oldest);
        table->evictions++;
    }
    return oldest;
}

void dmesh_peer_evict_idle(struct dmesh_peer_table *table)
{
    uint64_t now = peer_now(table);
    for (uint32_t i = 0; i < DMESH_CHANNEL_MAX; i++) {
        struct dmesh_peer_channel *channel = &table->channels[i];
        if (!channel->in_use || channel->state == DMESH_PEER_CLOSED)
            continue;
        if (now - channel->last_active_ns < DMESH_CHANNEL_IDLE_NS)
            continue;
        /* An idle channel with streams on it is not idle. */
        if (channel->handle_count || channel->inflight_bytes)
            continue;
        dmesh_peer_reset(table, channel, "peer channel idle");
        peer_channel_free(table, channel);
        table->evictions++;
    }
}

void dmesh_peer_table_rebind(struct dmesh_peer_table *table)
{
    if (!table->ops || !table->ops->node_binding)
        return;
    for (uint32_t i = 0; i < DMESH_CHANNEL_MAX; i++) {
        struct dmesh_peer_channel *channel = &table->channels[i];
        if (!channel->in_use || channel->state == DMESH_PEER_CLOSED)
            continue;
        const uint8_t *key = NULL;
        uint32_t ip_be = 0;
        uint16_t port = 0;
        if (!table->ops->node_binding(table->ops_ctx, channel->node_name,
                                      &key, &ip_be, &port) || !key) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_NODE_UNBOUND);
            dmesh_peer_reset(table, channel, "the generation no longer binds the peer");
            continue;
        }
        if (memcmp(channel->bound_key, key, 32) != 0) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_NODE_KEY);
            dmesh_peer_reset(table, channel, "the generation re-keyed the peer");
        }
    }
}

struct dmesh_peer_channel *dmesh_peer_open(struct dmesh_peer_table *table,
                                           const char *node_name,
                                           enum dmesh_peer_refusal *reason)
{
    enum dmesh_peer_refusal ignored;
    if (!reason)
        reason = &ignored;
    *reason = DMESH_PEER_OK;

    struct dmesh_peer_channel *channel = dmesh_peer_find(table, node_name);
    if (channel && channel->state == DMESH_PEER_OPEN) {
        channel->last_active_ns = peer_now(table);
        return channel;
    }
    if (channel && channel->state == DMESH_PEER_AUTHENTICATING)
        return channel;                  /* callers poll; never replace a live handshake */

    /* The binding decides whether a channel may exist at all: a name the held
     * generation does not bind has no key to authenticate against, and no
     * address this DPU would believe. */
    const uint8_t *key = NULL;
    uint32_t ip_be = 0;
    uint16_t port = 0;
    if (!table->ops || !table->ops->node_binding ||
        !table->ops->node_binding(table->ops_ctx, node_name, &key, &ip_be, &port) || !key) {
        *reason = peer_refuse(table, channel, DMESH_PEER_REFUSE_NODE_UNBOUND);
        return NULL;
    }

    if (!channel) {
        channel = peer_channel_slot(table);
        if (!channel) {
            *reason = peer_refuse(table, NULL, DMESH_PEER_REFUSE_STATE);
            return NULL;
        }
        memset(channel, 0, sizeof(*channel));
        snprintf(channel->node_name, sizeof(channel->node_name), "%s", node_name);
        channel->in_use = 1;
        /* A full bucket at creation, refilled at PEER_OPEN_RATE per second. */
        channel->open_tokens_milli = (uint64_t)DMESH_PEER_OPEN_RATE * 1000ull;
        channel->open_refill_ns = peer_now(table);
    }
    if (peer_channel_alloc_tables(channel) != 0) {
        *reason = peer_refuse(table, channel, DMESH_PEER_REFUSE_STATE);
        return NULL;
    }
    memcpy(channel->bound_key, key, 32);
    channel->ip_be = ip_be;
    channel->port = port;
    channel->incarnation++;                 /* on entry to AUTHENTICATING */
    channel->initiated_local = 1;
    channel->state = DMESH_PEER_AUTHENTICATING;
    channel->last_active_ns = peer_now(table);

    /* A reopen while a handshake is still pending supersedes its transport
     * connection, which must be closed rather than overwritten. */
    if (channel->conn && table->transport && table->transport->close) {
        table->transport->close(channel->conn);
        channel->conn = NULL;
    }

    uint8_t prologue[DMESH_PEER_PROLOGUE_MAX];
    int prologue_len = dmesh_peer_prologue(table->node_name, node_name,
                                           channel->incarnation, prologue, sizeof(prologue));
    if (prologue_len < 0) {
        *reason = peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
        channel->state = DMESH_PEER_CLOSED;
        return NULL;
    }
    void *conn = NULL;
    if (!table->transport || !table->transport->connect ||
        table->transport->connect(table->transport_ctx, ip_be, port, prologue,
                                  (size_t)prologue_len, &conn) != 0) {
        if (conn && table->transport && table->transport->close)
            table->transport->close(conn);
        *reason = peer_refuse(table, channel, DMESH_PEER_REFUSE_TRANSPORT);
        channel->state = DMESH_PEER_CLOSED;
        return NULL;
    }
    channel->conn = conn;
    channel->handshakes++;
    return channel;
}

struct dmesh_peer_channel *
dmesh_peer_accept(struct dmesh_peer_table *table, const char *node_name,
                  uint32_t incarnation, void *conn, const uint8_t peer_key[32],
                  enum dmesh_peer_refusal *reason)
{
    enum dmesh_peer_refusal ignored;
    if (!reason)
        reason = &ignored;
    *reason = DMESH_PEER_OK;
    if (!table) {
        *reason = DMESH_PEER_REFUSE_MALFORMED;
        return NULL;
    }
    if (!node_name || !*node_name || incarnation == 0 || !conn || !peer_key) {
        *reason = peer_refuse(table, NULL, DMESH_PEER_REFUSE_MALFORMED);
        if (conn && table->transport && table->transport->close)
            table->transport->close(conn);
        return NULL;
    }
    const uint8_t *bound = NULL;
    uint32_t ip_be = 0;
    uint16_t port = 0;
    if (!table->ops || !table->ops->node_binding ||
        !table->ops->node_binding(table->ops_ctx, node_name, &bound, &ip_be, &port) ||
        !bound) {
        *reason = peer_refuse(table, NULL, DMESH_PEER_REFUSE_NODE_UNBOUND);
        if (table->transport && table->transport->close)
            table->transport->close(conn);
        return NULL;
    }
    struct dmesh_peer_channel *channel = dmesh_peer_find(table, node_name);
    if (channel && channel->state != DMESH_PEER_CLOSED) {
        /* Simultaneous opens converge on the connection initiated by the
         * lexicographically smaller node. Both ends retain the same transport
         * rather than replacing each other forever. */
        int local_is_initiator = strcmp(table->node_name, node_name) < 0;
        int existing_preferred = channel->initiated_local == local_is_initiator;
        int incoming_preferred = !local_is_initiator;
        if (existing_preferred || !incoming_preferred) {
            if (table->transport && table->transport->close)
                table->transport->close(conn);
            *reason = peer_refuse(table, channel, DMESH_PEER_REFUSE_STATE);
            return NULL;
        }
        dmesh_peer_reset(table, channel,
                         "canonical peer connection replaced local open");
    }
    if (!channel) {
        channel = peer_channel_slot(table);
        if (!channel) {
            *reason = peer_refuse(table, NULL, DMESH_PEER_REFUSE_STATE);
            if (table->transport && table->transport->close)
                table->transport->close(conn);
            return NULL;
        }
        memset(channel, 0, sizeof(*channel));
        snprintf(channel->node_name, sizeof(channel->node_name), "%s", node_name);
        channel->in_use = 1;
        channel->open_tokens_milli = (uint64_t)DMESH_PEER_OPEN_RATE * 1000ull;
        channel->open_refill_ns = peer_now(table);
    }
    if (peer_channel_alloc_tables(channel) != 0) {
        *reason = peer_refuse(table, channel, DMESH_PEER_REFUSE_STATE);
        if (table->transport && table->transport->close)
            table->transport->close(conn);
        return NULL;
    }
    memcpy(channel->bound_key, bound, 32);
    channel->ip_be = ip_be;
    channel->port = port;
    channel->incarnation = incarnation;
    channel->initiated_local = 0;
    channel->conn = conn;
    channel->state = DMESH_PEER_AUTHENTICATING;
    channel->last_active_ns = peer_now(table);
    channel->handshakes++;
    *reason = dmesh_peer_authenticated(table, channel, peer_key);
    return *reason == DMESH_PEER_OK ? channel : NULL;
}

enum dmesh_peer_refusal dmesh_peer_authenticated(struct dmesh_peer_table *table,
                                                 struct dmesh_peer_channel *channel,
                                                 const uint8_t peer_key[32])
{
    if (channel->state != DMESH_PEER_AUTHENTICATING)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_STATE);
    /* The one rule added on top of the stock protocol. A name the generation
     * does not bind never reached here; a key that differs stops here. */
    if (memcmp(channel->bound_key, peer_key, 32) != 0) {
        peer_refuse(table, channel, DMESH_PEER_REFUSE_NODE_KEY);
        dmesh_peer_reset(table, channel, "peer static key is not the bound one");
        return DMESH_PEER_REFUSE_NODE_KEY;
    }
    channel->state = DMESH_PEER_OPEN;
    channel->last_active_ns = peer_now(table);
    channel->opened++;
    return DMESH_PEER_OK;
}

/* ---- destination ------------------------------------------------------ */

static enum dmesh_peer_refusal
peer_live(struct dmesh_peer_table *table, struct dmesh_peer_channel *channel,
          uint32_t incarnation)
{
    if (!table || !channel)
        return DMESH_PEER_REFUSE_STATE;
    if (channel->state != DMESH_PEER_OPEN)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_STATE);
    if (incarnation != channel->incarnation)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_INCARNATION);
    return DMESH_PEER_OK;
}

/* A refused stream costs a lookup and a reply, so what needs bounding is the
 * rate. A peer that is refused often becomes slow, not disconnected: tearing
 * the channel down would turn ordinary generation skew into an outage. */
static int peer_open_allowed(struct dmesh_peer_table *table,
                             struct dmesh_peer_channel *channel)
{
    uint64_t now = peer_now(table);
    uint64_t elapsed = now - channel->open_refill_ns;
    if (elapsed) {
        channel->open_refill_ns = now;
        uint64_t gained = (elapsed / 1000000ull) * DMESH_PEER_OPEN_RATE;
        uint64_t ceiling = (uint64_t)DMESH_PEER_OPEN_RATE * 1000ull;
        channel->open_tokens_milli =
            channel->open_tokens_milli + gained > ceiling
                ? ceiling
                : channel->open_tokens_milli + gained;
    }
    if (channel->open_tokens_milli < 1000ull)
        return 0;
    channel->open_tokens_milli -= 1000ull;
    return 1;
}

enum dmesh_peer_refusal
dmesh_peer_stream_open(struct dmesh_peer_table *table,
                       struct dmesh_peer_channel *channel,
                       uint32_t incarnation,
                       const struct dmesh_peer_stream_open *open,
                       uint32_t *handle)
{
    *handle = 0;
    enum dmesh_peer_refusal live = peer_live(table, channel, incarnation);
    if (live != DMESH_PEER_OK)
        return live;
    if (!peer_text_ok(open->src_pod_uid, sizeof(open->src_pod_uid)) ||
        !peer_text_ok(open->dst_pod_uid, sizeof(open->dst_pod_uid)) ||
        !peer_text_ok(open->src_service_key, sizeof(open->src_service_key)) ||
        open->reserved != 0 || open->source_token == 0 || open->dst_port == 0)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
    if (!peer_open_allowed(table, channel))
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_RATE);
    if (channel->handle_count >= DMESH_PEER_STREAMS_MAX)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_STREAMS);

    /* The identity check: the source may speak only for the Pods the held
     * generation places on its own node. This is the whole reason the
     * generation is signed by a key no DPU holds. */
    if (!table->ops->pod_on_node(table->ops_ctx, open->src_pod_uid, channel->node_name))
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_NOT_ON_PEER);

    uint32_t pod_generation = 0;
    int32_t dst_pod_idx = table->ops->local_pod(table->ops_ctx, open->dst_pod_uid,
                                                &pod_generation);
    if (dst_pod_idx < 0)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_NO_POD);

    for (uint32_t i = 0; i < DMESH_PEER_STREAMS_MAX; i++) {
        struct dmesh_peer_handle *slot = &channel->handles[i];
        if (slot->in_use)
            continue;
        memset(slot, 0, sizeof(*slot));
        slot->incarnation = channel->incarnation;
        slot->dst_pod_idx = dst_pod_idx;
        slot->dst_pod_generation = pod_generation;
        slot->dst_port = open->dst_port;
        uint32_t wire_handle = i + 1;
        if (strcmp(table->node_name, channel->node_name) > 0)
            wire_handle |= DMESH_PEER_HANDLE_OWNER_BIT;
        slot->wire_handle = wire_handle;
        slot->up_port = table->ops->destination_opened
                            ? table->ops->destination_opened(
                                  table->ops_ctx, channel, wire_handle, open,
                                  dst_pod_idx)
                            : (table->ops->upstream_for
                                   ? table->ops->upstream_for(
                                         table->ops_ctx, dst_pod_idx,
                                         open->dst_port)
                                   : 0);
        if (slot->up_port == 0) {
            memset(slot, 0, sizeof(*slot));
            return peer_refuse(table, channel, DMESH_PEER_REFUSE_NO_POD);
        }
        memcpy(slot->src_pod_uid, open->src_pod_uid, sizeof(slot->src_pod_uid));
        slot->src_pod_uid[sizeof(slot->src_pod_uid) - 1] = '\0';
        slot->in_use = 1;
        channel->handle_count++;
        channel->last_active_ns = peer_now(table);
        /* Handle 0 names no stream on the wire, so the space starts at one. */
        *handle = wire_handle;
        return DMESH_PEER_OK;
    }
    return peer_refuse(table, channel, DMESH_PEER_REFUSE_STREAMS);
}

static struct dmesh_peer_handle *
peer_handle_of(struct dmesh_peer_channel *channel, uint32_t handle)
{
    uint32_t index = handle & DMESH_PEER_HANDLE_INDEX_MASK;
    if (index == 0 || index > DMESH_PEER_STREAMS_MAX ||
        (handle & ~(DMESH_PEER_HANDLE_OWNER_BIT |
                    DMESH_PEER_HANDLE_INDEX_MASK)) != 0 || !channel->handles)
        return NULL;
    struct dmesh_peer_handle *slot = &channel->handles[index - 1];
    if (!slot->in_use || slot->incarnation != channel->incarnation ||
        slot->wire_handle != handle)
        return NULL;
    return slot;
}

static uint32_t peer_local_owner_bit(const struct dmesh_peer_table *table,
                                     const struct dmesh_peer_channel *channel)
{
    return strcmp(table->node_name, channel->node_name) > 0
               ? DMESH_PEER_HANDLE_OWNER_BIT
               : 0u;
}

static int peer_handle_wire_ok(uint32_t handle)
{
    uint32_t index = handle & DMESH_PEER_HANDLE_INDEX_MASK;
    return index > 0 && index <= DMESH_PEER_STREAMS_MAX &&
           (handle & ~(DMESH_PEER_HANDLE_OWNER_BIT |
                       DMESH_PEER_HANDLE_INDEX_MASK)) == 0;
}

static void peer_rx_abandon_handle(struct dmesh_peer_channel *channel,
                                   uint32_t handle, int completed_too);

static void peer_handle_release(struct dmesh_peer_channel *channel,
                                struct dmesh_peer_handle *handle)
{
    if (!handle->in_use)
        return;
    /* A terminal handle may still own asynchronous destination landings. Drop
     * those slots now so a completion that races teardown becomes a harmless
     * no-op. Completed slots stay until ACK publication on an orderly FIN. */
    peer_rx_abandon_handle(channel, handle->wire_handle, 0);
    channel->staging_bytes -= handle->staging_bytes;
    handle->staging_bytes = 0;
    handle->in_use = 0;
    if (channel->handle_count)
        channel->handle_count--;
}

/* Acknowledge only once the bytes have landed in the destination Pod's host
 * RX mapping. Batching them is the same economy the reverse ring already
 * applies: one acknowledgement per released extent. */
static void peer_ack_append(struct dmesh_peer_channel *channel,
                            uint32_t handle, uint32_t seq)
{
    struct dmesh_peer_ack_entry *staged = NULL;
    if (channel->ack_staged) {
        struct dmesh_peer_ack_entry *last = &channel->ack_stage[channel->ack_staged - 1];
        if (last->handle == handle && last->seq_first + last->seq_count == seq)
            staged = last;
    }
    if (staged) {
        staged->seq_count++;
    } else {
        staged = &channel->ack_stage[channel->ack_staged++];
        staged->handle = handle;
        staged->seq_first = seq;
        staged->seq_count = 1;
        staged->reserved = 0;
    }
}

static struct dmesh_peer_rxslot *
peer_rx_alloc(struct dmesh_peer_channel *channel, uint32_t handle,
              uint32_t seq, uint32_t bytes, int source_side)
{
    if (!channel->rx || channel->rx_free == DMESH_PEER_TX_NIL)
        return NULL;
    uint32_t index = channel->rx_free;
    struct dmesh_peer_rxslot *slot = &channel->rx[index];
    channel->rx_free = slot->next;
    memset(slot, 0, sizeof(*slot));
    slot->next = DMESH_PEER_TX_NIL;
    slot->handle = handle;
    slot->seq = seq;
    slot->bytes = bytes;
    slot->source_side = (uint8_t)(source_side != 0);
    slot->in_use = 1;
    return slot;
}

static void peer_rx_free_slot(struct dmesh_peer_channel *channel,
                              struct dmesh_peer_rxslot *slot)
{
    uint32_t index = (uint32_t)(slot - channel->rx);
    memset(slot, 0, sizeof(*slot));
    slot->next = channel->rx_free;
    channel->rx_free = index;
}

static void peer_rx_abandon_handle(struct dmesh_peer_channel *channel,
                                   uint32_t handle, int completed_too)
{
    if (!channel->rx)
        return;
    for (uint32_t i = 0; i < DMESH_PEER_TX_SLOTS; i++) {
        struct dmesh_peer_rxslot *slot = &channel->rx[i];
        if (!slot->in_use || slot->source_side || slot->handle != handle ||
            (!completed_too && slot->completed))
            continue;
        /* The handle owns the aggregate staging charge; its release below
         * subtracts that charge once after all of its slots are detached. */
        peer_rx_free_slot(channel, slot);
    }
}

static struct dmesh_peer_rxslot *
peer_rx_find(struct dmesh_peer_channel *channel, uint32_t handle,
             uint32_t seq, int source_side)
{
    if (!channel->rx)
        return NULL;
    for (uint32_t i = 0; i < DMESH_PEER_TX_SLOTS; i++) {
        struct dmesh_peer_rxslot *slot = &channel->rx[i];
        if (slot->in_use && slot->handle == handle && slot->seq == seq &&
            slot->source_side == (uint8_t)(source_side != 0))
            return slot;
    }
    return NULL;
}

static void peer_ack_drain(struct dmesh_peer_table *table,
                           struct dmesh_peer_channel *channel)
{
    if (!channel->rx)
        return;
    for (uint32_t i = 0; i < DMESH_PEER_TX_SLOTS; i++) {
        struct dmesh_peer_rxslot *slot = &channel->rx[i];
        if (!slot->in_use || !slot->completed)
            continue;
        if (channel->ack_staged == DMESH_STREAM_ACK_BATCH &&
            dmesh_peer_ack_flush(table, channel) != 0)
            return;
        peer_ack_append(channel, slot->handle, slot->seq);
        peer_rx_free_slot(channel, slot);
    }
    if (channel->ack_staged == DMESH_STREAM_ACK_BATCH)
        (void)dmesh_peer_ack_flush(table, channel);
}

enum dmesh_peer_refusal
dmesh_peer_data(struct dmesh_peer_table *table, struct dmesh_peer_channel *channel,
                uint32_t incarnation, uint32_t handle, uint32_t seq,
                const uint8_t *bytes, uint32_t len)
{
    enum dmesh_peer_refusal live = peer_live(table, channel, incarnation);
    if (live != DMESH_PEER_OK)
        return live;
    if (len == 0 || len > DMESH_PEER_EXTENT_MAX)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
    if (!peer_handle_wire_ok(handle))
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_HANDLE);
    struct dmesh_peer_handle *slot = peer_handle_of(channel, handle);
    if (!slot) {
        if ((handle & DMESH_PEER_HANDLE_OWNER_BIT) ==
            peer_local_owner_bit(table, channel))
            return peer_refuse(table, channel, DMESH_PEER_REFUSE_HANDLE);
        if (!table->ops || !table->ops->source_deliver)
            return peer_refuse(table, channel, DMESH_PEER_REFUSE_HANDLE);
        if (channel->staging_bytes + len > DMESH_PEER_STAGING_MAX)
            return peer_refuse(table, channel, DMESH_PEER_REFUSE_STAGING);
        struct dmesh_peer_rxslot *rx = peer_rx_alloc(channel, handle, seq,
                                                      len, 1);
        if (!rx)
            return peer_refuse(table, channel, DMESH_PEER_REFUSE_STAGING);
        channel->staging_bytes += len;
        int delivered = table->ops->source_deliver(
            table->ops_ctx, channel, handle, bytes, len, seq);
        if (delivered < 0) {
            channel->staging_bytes -= len;
            peer_rx_free_slot(channel, rx);
            return peer_refuse(table, channel, DMESH_PEER_REFUSE_HANDLE);
        }
        channel->last_active_ns = peer_now(table);
        if (delivered > 0)
            return DMESH_PEER_OK;
        channel->staging_bytes -= len;
        rx->completed = 1;
        peer_ack_drain(table, channel);
        return DMESH_PEER_OK;
    }
    /* The pod generation rejects a handle whose destination slot has been
     * re-tenanted since the stream opened: the bytes would land in a Pod that
     * never opened this stream. */
    if (table->ops->pod_generation &&
        table->ops->pod_generation(table->ops_ctx, slot->dst_pod_idx) !=
            slot->dst_pod_generation) {
        peer_refuse(table, channel, DMESH_PEER_REFUSE_HANDLE);
        peer_handle_release(channel, slot);
        return DMESH_PEER_REFUSE_HANDLE;
    }
    if (channel->staging_bytes + len > DMESH_PEER_STAGING_MAX)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_STAGING);
    if (slot->rx_seq_valid && seq != slot->rx_seq + 1u)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
    if (slot->rx_fin)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_STATE);
    struct dmesh_peer_rxslot *rx = peer_rx_alloc(channel, handle, seq, len, 0);
    if (!rx)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_STAGING);

    channel->staging_bytes += len;
    slot->staging_bytes += len;
    int delivered = table->ops->deliver(table->ops_ctx, slot, bytes, len, seq);
    if (delivered < 0) {
        channel->staging_bytes -= len;
        slot->staging_bytes -= len;
        peer_rx_free_slot(channel, rx);
        peer_refuse(table, channel, DMESH_PEER_REFUSE_NO_POD);
        if (table->ops->poison)
            table->ops->poison(table->ops_ctx, slot, "peer stream cannot be delivered");
        channel->poisoned++;
        peer_handle_release(channel, slot);
        return DMESH_PEER_REFUSE_NO_POD;
    }
    slot->rx_seq = seq;
    slot->rx_seq_valid = 1;
    channel->last_active_ns = peer_now(table);
    /* Held: the landing is asynchronous, so the charge stays until the node
     * reports it through dmesh_peer_delivered — which is what makes
     * PEER_STAGING_MAX a real bound — and the acknowledgement waits with it. */
    if (delivered > 0)
        return DMESH_PEER_OK;
    channel->staging_bytes -= len;
    slot->staging_bytes -= len;
    rx->completed = 1;
    peer_ack_drain(table, channel);
    return DMESH_PEER_OK;
}

void dmesh_peer_delivered(struct dmesh_peer_table *table,
                          struct dmesh_peer_channel *channel,
                          uint32_t handle, uint32_t seq, uint32_t len)
{
    if (!table || !channel || channel->state != DMESH_PEER_OPEN ||
        !peer_handle_wire_ok(handle) ||
        (handle & DMESH_PEER_HANDLE_OWNER_BIT) !=
            peer_local_owner_bit(table, channel))
        return;
    struct dmesh_peer_handle *slot = peer_handle_of(channel, handle);
    struct dmesh_peer_rxslot *rx = peer_rx_find(channel, handle, seq, 0);
    if (!slot || !rx || rx->completed || rx->bytes != len)
        return;
    slot->staging_bytes -= len;
    channel->staging_bytes -= len;
    rx->completed = 1;
    peer_ack_drain(table, channel);
    if (slot->rx_fin && slot->tx_fin && slot->staging_bytes == 0)
        peer_handle_release(channel, slot);
    channel->last_active_ns = peer_now(table);
}

void dmesh_peer_source_delivered(struct dmesh_peer_table *table,
                                 struct dmesh_peer_channel *channel,
                                 uint32_t handle, uint32_t seq, uint32_t len)
{
    if (!table || !channel || channel->state != DMESH_PEER_OPEN ||
        !peer_handle_wire_ok(handle) ||
        (handle & DMESH_PEER_HANDLE_OWNER_BIT) ==
            peer_local_owner_bit(table, channel))
        return;
    struct dmesh_peer_rxslot *rx = peer_rx_find(channel, handle, seq, 1);
    if (!rx || rx->completed || rx->bytes != len)
        return;
    channel->staging_bytes -= len;
    rx->completed = 1;
    peer_ack_drain(table, channel);
    channel->last_active_ns = peer_now(table);
}

/* One channel has one ordered writer. A transport-level would-block retains
 * the complete frame here; the next progress pass flushes it before receiving
 * more input or admitting another source frame. */
static int peer_wire_send(struct dmesh_peer_table *table,
                          struct dmesh_peer_channel *channel,
                          uint8_t type, uint32_t handle,
                          const void *payload, uint32_t payload_len)
{
    if (channel->tx_len != 0)
        return 1;
    long built = dmesh_peer_frame_build(channel->tx_frame,
                                        sizeof(struct dmesh_peer_msg_header) +
                                            DMESH_PEER_FRAME_MAX,
                                        type, channel->incarnation, handle,
                                        payload, payload_len);
    if (built < 0 || !table->transport || !table->transport->send ||
        !channel->conn)
        return -1;
    long sent = table->transport->send(channel->conn, channel->tx_frame,
                                       (size_t)built);
    if (sent == built)
        return 0;
    if (sent == 0) {
        channel->tx_len = (uint32_t)built;
        return 0;
    }
    return -1;                 /* partial ordered frames cannot be recovered */
}

int dmesh_peer_ack_flush(struct dmesh_peer_table *table,
                         struct dmesh_peer_channel *channel)
{
    if (!channel->ack_staged)
        return 0;
    uint32_t bytes = channel->ack_staged *
                     (uint32_t)sizeof(struct dmesh_peer_ack_entry);
    int sent = peer_wire_send(table, channel, DMESH_PEER_MSG_STREAM_ACK, 0,
                              channel->ack_stage, bytes);
    if (sent == 0)
        channel->ack_staged = 0;
    return sent;
}

enum dmesh_peer_refusal
dmesh_peer_stream_fin(struct dmesh_peer_table *table,
                      struct dmesh_peer_channel *channel,
                      uint32_t incarnation, uint32_t handle)
{
    enum dmesh_peer_refusal live = peer_live(table, channel, incarnation);
    if (live != DMESH_PEER_OK)
        return live;
    struct dmesh_peer_handle *slot = peer_handle_of(channel, handle);
    if (!slot)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_HANDLE);
    if (slot->rx_fin)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_STATE);
    slot->rx_fin = 1;
    if (table->ops && table->ops->destination_fin &&
        table->ops->destination_fin(table->ops_ctx, slot) < 0)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_NO_POD);
    if (slot->tx_fin && slot->staging_bytes == 0)
        peer_handle_release(channel, slot);
    channel->last_active_ns = peer_now(table);
    return DMESH_PEER_OK;
}

/* Within a node a sweep suffices because the DPU sees the Pod leave; across
 * nodes a destination cannot see it, so the source has to say. */
enum dmesh_peer_refusal
dmesh_peer_pod_gone(struct dmesh_peer_table *table,
                    struct dmesh_peer_channel *channel,
                    uint32_t incarnation, const char *pod_uid)
{
    enum dmesh_peer_refusal live = peer_live(table, channel, incarnation);
    if (live != DMESH_PEER_OK)
        return live;
    if (!pod_uid || !*pod_uid)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
    for (uint32_t i = 0; i < DMESH_PEER_STREAMS_MAX; i++) {
        struct dmesh_peer_handle *slot = &channel->handles[i];
        if (!slot->in_use || strcmp(slot->src_pod_uid, pod_uid) != 0)
            continue;
        if (table->ops->poison)
            table->ops->poison(table->ops_ctx, slot, "peer source Pod is gone");
        channel->poisoned++;
        /* The source drops this Pod's local custody itself, in the same sweep
         * that sends POD_GONE, so neither incomplete nor already-landed
         * extents need an ACK. Reclaim every RX slot before invalidating the
         * handle. */
        peer_rx_abandon_handle(channel, slot->wire_handle, 1);
        peer_handle_release(channel, slot);
    }
    channel->last_active_ns = peer_now(table);
    return DMESH_PEER_OK;
}

/* ---- source ----------------------------------------------------------- */

enum dmesh_peer_refusal
dmesh_peer_tx_charge(struct dmesh_peer_channel *channel, uint32_t handle,
                     uint32_t seq, uint32_t bytes, uint8_t kind, void *cookie)
{
    if (channel->state != DMESH_PEER_OPEN)
        return DMESH_PEER_REFUSE_STATE;
    /* A stalled peer holds bounded resources at a source. Without this bound
     * one dead peer's un-ACKed chunks would drain the shared egress arena that
     * every other L7 connection allocates from. */
    if (channel->inflight_bytes + bytes > DMESH_PEER_TX_INFLIGHT_MAX ||
        channel->tx_free == DMESH_PEER_TX_NIL) {
        channel->stalled = 1;
        channel->refused[DMESH_PEER_REFUSE_INFLIGHT]++;
        return DMESH_PEER_REFUSE_INFLIGHT;
    }
    uint32_t index = channel->tx_free;
    struct dmesh_peer_txslot *slot = &channel->tx[index];
    channel->tx_free = slot->next;
    slot->next = DMESH_PEER_TX_NIL;
    slot->seq = seq;
    slot->bytes = bytes;
    slot->handle = handle;
    slot->cookie = cookie;
    slot->kind = kind;
    slot->in_use = 1;
    channel->inflight_bytes += bytes;
    return DMESH_PEER_OK;
}

static void peer_tx_free_slot(struct dmesh_peer_channel *channel,
                              struct dmesh_peer_txslot *slot)
{
    channel->inflight_bytes -= slot->bytes;
    slot->in_use = 0;
    slot->cookie = NULL;
    slot->next = channel->tx_free;
    channel->tx_free = (uint32_t)(slot - channel->tx);
    if (channel->inflight_bytes < DMESH_PEER_TX_INFLIGHT_MAX)
        channel->stalled = 0;
}

enum dmesh_peer_refusal
dmesh_peer_tx_ack(struct dmesh_peer_table *table, struct dmesh_peer_channel *channel,
                  uint32_t incarnation, const struct dmesh_peer_ack_entry *entry)
{
    /* The channel incarnation is matched on every path, including this one: an
     * acknowledgement from a previous connection names extents this one has
     * already released. */
    enum dmesh_peer_refusal live = peer_live(table, channel, incarnation);
    if (live != DMESH_PEER_OK)
        return live;
    if (!entry || !peer_handle_wire_ok(entry->handle))
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
    if (entry->seq_count == 0 || entry->seq_count > DMESH_PEER_TX_SLOTS)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
    uint32_t retired = 0;
    for (uint32_t i = 0; i < DMESH_PEER_TX_SLOTS; i++) {
        struct dmesh_peer_txslot *slot = &channel->tx[i];
        if (!slot->in_use || slot->handle != entry->handle)
            continue;
        if (slot->seq - entry->seq_first >= entry->seq_count)
            continue;                       /* outside the named run */
        if (table->ops && table->ops->release)
            table->ops->release(table->ops_ctx, slot->kind, slot->cookie, slot->bytes);
        peer_tx_free_slot(channel, slot);
        retired++;
    }
    if (!retired)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_HANDLE);
    channel->last_active_ns = peer_now(table);
    return DMESH_PEER_OK;
}

void dmesh_peer_tx_drop(struct dmesh_peer_table *table,
                        struct dmesh_peer_channel *channel, uint32_t handle)
{
    if (!channel->tx)
        return;
    for (uint32_t i = 0; i < DMESH_PEER_TX_SLOTS; i++) {
        struct dmesh_peer_txslot *slot = &channel->tx[i];
        if (!slot->in_use || slot->handle != handle)
            continue;
        if (table->ops && table->ops->release)
            table->ops->release(table->ops_ctx, slot->kind, slot->cookie, slot->bytes);
        peer_tx_free_slot(channel, slot);
    }
}

static void peer_tx_uncharge(struct dmesh_peer_channel *channel,
                             uint32_t handle, uint32_t seq, void *cookie)
{
    if (!channel->tx)
        return;
    for (uint32_t i = 0; i < DMESH_PEER_TX_SLOTS; i++) {
        struct dmesh_peer_txslot *slot = &channel->tx[i];
        if (!slot->in_use || slot->handle != handle || slot->seq != seq ||
            slot->cookie != cookie)
            continue;
        peer_tx_free_slot(channel, slot); /* the cookie stays with the caller */
        return;
    }
}

enum dmesh_peer_refusal
dmesh_peer_stream_request(struct dmesh_peer_table *table,
                          struct dmesh_peer_channel *channel,
                          const struct dmesh_peer_stream_open *open)
{
    if (!table)
        return DMESH_PEER_REFUSE_STATE;
    if (!channel || !open || channel->state != DMESH_PEER_OPEN)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_STATE);
    if (!peer_text_ok(open->src_pod_uid, sizeof(open->src_pod_uid)) ||
        !peer_text_ok(open->dst_pod_uid, sizeof(open->dst_pod_uid)) ||
        !peer_text_ok(open->src_service_key, sizeof(open->src_service_key)) ||
        open->reserved != 0 || open->source_token == 0 || open->dst_port == 0)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
    int sent = peer_wire_send(table, channel, DMESH_PEER_MSG_STREAM_OPEN, 0,
                              open, sizeof(*open));
    if (sent == 0)
        return DMESH_PEER_OK;
    return peer_refuse(table, channel,
                       sent > 0 ? DMESH_PEER_REFUSE_INFLIGHT
                                : DMESH_PEER_REFUSE_TRANSPORT);
}

enum dmesh_peer_refusal
dmesh_peer_stream_data_send(struct dmesh_peer_table *table,
                            struct dmesh_peer_channel *channel,
                            uint32_t handle, uint32_t seq,
                            const uint8_t *bytes, uint32_t len,
                            uint8_t custody_kind, void *cookie)
{
    if (!table)
        return DMESH_PEER_REFUSE_STATE;
    if (!channel || channel->state != DMESH_PEER_OPEN)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_STATE);
    if (!peer_handle_wire_ok(handle) || !bytes || len == 0 ||
        len > DMESH_PEER_EXTENT_MAX ||
        custody_kind > DMESH_PEER_CUSTODY_L7 || !cookie)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
    if ((handle & DMESH_PEER_HANDLE_OWNER_BIT) ==
            peer_local_owner_bit(table, channel) &&
        !peer_handle_of(channel, handle))
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_HANDLE);
    if (channel->tx_len != 0)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_INFLIGHT);
    enum dmesh_peer_refusal charged = dmesh_peer_tx_charge(
        channel, handle, seq, len, custody_kind, cookie);
    if (charged != DMESH_PEER_OK) {
        peer_refuse(table, channel, charged);
        return charged;
    }
    struct dmesh_peer_data_prefix prefix = { .seq = seq, .reserved = 0 };
    memcpy(channel->tx_frame + sizeof(struct dmesh_peer_msg_header),
           &prefix, sizeof(prefix));
    memcpy(channel->tx_frame + sizeof(struct dmesh_peer_msg_header) + sizeof(prefix),
           bytes, len);
    struct dmesh_peer_msg_header header = {
        .type = DMESH_PEER_MSG_DATA,
        .version = DMESH_PEER_WIRE_VERSION,
        .reserved = 0,
        .length = (uint32_t)sizeof(prefix) + len,
        .incarnation = channel->incarnation,
        .handle = handle,
    };
    memcpy(channel->tx_frame, &header, sizeof(header));
    uint32_t frame_len = (uint32_t)sizeof(header) + header.length;
    long sent = table->transport && table->transport->send && channel->conn
                    ? table->transport->send(channel->conn, channel->tx_frame,
                                             frame_len)
                    : -1;
    if (sent == (long)frame_len)
        return DMESH_PEER_OK;
    if (sent == 0) {
        channel->tx_len = frame_len;
        return DMESH_PEER_OK;
    }
    peer_tx_uncharge(channel, handle, seq, cookie);
    peer_refuse(table, channel, DMESH_PEER_REFUSE_TRANSPORT);
    dmesh_peer_reset(table, channel, "partial or failed peer DATA send");
    return DMESH_PEER_REFUSE_TRANSPORT;
}

enum dmesh_peer_refusal
dmesh_peer_stream_fin_send(struct dmesh_peer_table *table,
                           struct dmesh_peer_channel *channel,
                           uint32_t handle)
{
    if (!table)
        return DMESH_PEER_REFUSE_STATE;
    if (!channel || channel->state != DMESH_PEER_OPEN)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_STATE);
    if (!peer_handle_wire_ok(handle))
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
    if ((handle & DMESH_PEER_HANDLE_OWNER_BIT) ==
            peer_local_owner_bit(table, channel) &&
        !peer_handle_of(channel, handle))
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_HANDLE);
    int sent = peer_wire_send(table, channel, DMESH_PEER_MSG_STREAM_FIN,
                              handle, NULL, 0);
    if (sent == 0) {
        struct dmesh_peer_handle *local = peer_handle_of(channel, handle);
        if (local) {
            local->tx_fin = 1;
            if (local->rx_fin && local->staging_bytes == 0)
                peer_handle_release(channel, local);
        }
        return DMESH_PEER_OK;
    }
    return peer_refuse(table, channel,
                       sent > 0 ? DMESH_PEER_REFUSE_INFLIGHT
                                : DMESH_PEER_REFUSE_TRANSPORT);
}

enum dmesh_peer_refusal
dmesh_peer_pod_gone_send(struct dmesh_peer_table *table,
                         struct dmesh_peer_channel *channel,
                         const char *pod_uid)
{
    if (!table)
        return DMESH_PEER_REFUSE_STATE;
    if (!channel || channel->state != DMESH_PEER_OPEN)
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_STATE);
    if (!pod_uid || !peer_text_ok(pod_uid, DMESH_POD_UID_MAX))
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
    struct dmesh_peer_pod_gone gone;
    memset(&gone, 0, sizeof(gone));
    snprintf(gone.pod_uid, sizeof(gone.pod_uid), "%s", pod_uid);
    int sent = peer_wire_send(table, channel, DMESH_PEER_MSG_POD_GONE,
                              0, &gone, sizeof(gone));
    if (sent == 0)
        return DMESH_PEER_OK;
    return peer_refuse(table, channel,
                       sent > 0 ? DMESH_PEER_REFUSE_INFLIGHT
                                : DMESH_PEER_REFUSE_TRANSPORT);
}

/* ---- frames ----------------------------------------------------------- */

long dmesh_peer_frame_parse(const uint8_t *buf, size_t len,
                            struct dmesh_peer_msg_header *header,
                            const uint8_t **payload)
{
    if (len < sizeof(*header))
        return 0;
    memcpy(header, buf, sizeof(*header));
    if (header->version != DMESH_PEER_WIRE_VERSION ||
        header->type == DMESH_PEER_MSG_INVALID ||
        header->type > DMESH_PEER_MSG_DATA ||
        header->reserved != 0 ||
        header->length > DMESH_PEER_FRAME_MAX)
        return -1;
    if (len < sizeof(*header) + header->length)
        return 0;
    *payload = buf + sizeof(*header);
    return (long)(sizeof(*header) + header->length);
}

long dmesh_peer_frame_build(uint8_t *out, size_t out_len, uint8_t type,
                            uint32_t incarnation, uint32_t handle,
                            const void *payload, uint32_t payload_len)
{
    if (!out || type == DMESH_PEER_MSG_INVALID || type > DMESH_PEER_MSG_DATA ||
        incarnation == 0 || (payload_len != 0 && !payload) ||
        payload_len > DMESH_PEER_FRAME_MAX ||
        out_len < sizeof(struct dmesh_peer_msg_header) + payload_len)
        return -1;
    struct dmesh_peer_msg_header header = {
        .type = type,
        .version = DMESH_PEER_WIRE_VERSION,
        .reserved = 0,
        .length = payload_len,
        .incarnation = incarnation,
        .handle = handle,
    };
    memcpy(out, &header, sizeof(header));
    if (payload_len)
        memcpy(out + sizeof(header), payload, payload_len);
    return (long)(sizeof(header) + payload_len);
}

static int peer_dispatch_frame(struct dmesh_peer_table *table,
                               struct dmesh_peer_channel *channel,
                               const struct dmesh_peer_msg_header *header,
                               const uint8_t *payload)
{
    if (header->incarnation != channel->incarnation) {
        peer_refuse(table, channel, DMESH_PEER_REFUSE_INCARNATION);
        return -1;
    }
    switch (header->type) {
    case DMESH_PEER_MSG_STREAM_OPEN: {
        if (header->handle != 0 || header->length != sizeof(struct dmesh_peer_stream_open)) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
            return -1;
        }
        struct dmesh_peer_stream_open open;
        memcpy(&open, payload, sizeof(open));
        uint32_t handle = 0;
        enum dmesh_peer_refusal status = dmesh_peer_stream_open(
            table, channel, header->incarnation, &open, &handle);
        struct dmesh_peer_stream_open_ack ack = {
            .source_token = open.source_token,
            .handle = handle,
            .status = status,
            .reserved = 0,
        };
        if (peer_wire_send(table, channel, DMESH_PEER_MSG_STREAM_OPEN_ACK,
                           0, &ack, sizeof(ack)) != 0) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_TRANSPORT);
            return -1;
        }
        return 0;
    }
    case DMESH_PEER_MSG_STREAM_OPEN_ACK: {
        if (header->handle != 0 ||
            header->length != sizeof(struct dmesh_peer_stream_open_ack)) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
            return -1;
        }
        struct dmesh_peer_stream_open_ack ack;
        memcpy(&ack, payload, sizeof(ack));
        if (ack.source_token == 0 || ack.reserved != 0 || ack.status < 0 ||
            ack.status >= DMESH_PEER_REFUSE_MAX ||
            (ack.status == DMESH_PEER_OK
                 ? (!peer_handle_wire_ok(ack.handle) ||
                    (ack.handle & DMESH_PEER_HANDLE_OWNER_BIT) ==
                        peer_local_owner_bit(table, channel))
                 : ack.handle != 0)) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
            return -1;
        }
        if (table->ops && table->ops->source_opened)
            table->ops->source_opened(table->ops_ctx, channel, ack.source_token,
                                      ack.handle, ack.status);
        return 0;
    }
    case DMESH_PEER_MSG_DATA: {
        if (header->handle == 0 ||
            header->length <= sizeof(struct dmesh_peer_data_prefix)) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
            return -1;
        }
        struct dmesh_peer_data_prefix prefix;
        memcpy(&prefix, payload, sizeof(prefix));
        if (prefix.reserved != 0) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
            return -1;
        }
        enum dmesh_peer_refusal status = dmesh_peer_data(
            table, channel, header->incarnation, header->handle, prefix.seq,
            payload + sizeof(prefix), header->length - (uint32_t)sizeof(prefix));
        return status == DMESH_PEER_OK ? 0 : -1;
    }
    case DMESH_PEER_MSG_STREAM_FIN: {
        if (header->handle == 0 || header->length != 0) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
            return -1;
        }
        if (!peer_handle_wire_ok(header->handle)) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_HANDLE);
            return -1;
        }
        if ((header->handle & DMESH_PEER_HANDLE_OWNER_BIT) ==
            peer_local_owner_bit(table, channel))
            return dmesh_peer_stream_fin(table, channel, header->incarnation,
                                         header->handle) == DMESH_PEER_OK ? 0 : -1;
        if (table->ops && table->ops->source_fin &&
            table->ops->source_fin(table->ops_ctx, channel, header->handle))
            return 0;
        peer_refuse(table, channel, DMESH_PEER_REFUSE_HANDLE);
        return -1;
    }
    case DMESH_PEER_MSG_STREAM_ACK: {
        if (header->handle != 0 || header->length == 0 ||
            header->length % sizeof(struct dmesh_peer_ack_entry) != 0 ||
            header->length > DMESH_STREAM_ACK_BATCH * sizeof(struct dmesh_peer_ack_entry)) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
            return -1;
        }
        uint32_t count = header->length / (uint32_t)sizeof(struct dmesh_peer_ack_entry);
        for (uint32_t i = 0; i < count; i++) {
            struct dmesh_peer_ack_entry entry;
            memcpy(&entry, payload + i * sizeof(entry), sizeof(entry));
            if (entry.reserved != 0 ||
                dmesh_peer_tx_ack(table, channel, header->incarnation, &entry) !=
                    DMESH_PEER_OK)
                return -1;
        }
        return 0;
    }
    case DMESH_PEER_MSG_POD_GONE: {
        if (header->handle != 0 || header->length != sizeof(struct dmesh_peer_pod_gone)) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
            return -1;
        }
        struct dmesh_peer_pod_gone gone;
        memcpy(&gone, payload, sizeof(gone));
        if (!peer_text_ok(gone.pod_uid, sizeof(gone.pod_uid))) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
            return -1;
        }
        return dmesh_peer_pod_gone(table, channel, header->incarnation,
                                   gone.pod_uid) == DMESH_PEER_OK ? 0 : -1;
    }
    default:
        peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
        return -1;
    }
}

int dmesh_peer_channel_progress(struct dmesh_peer_table *table,
                                struct dmesh_peer_channel *channel,
                                int frame_budget)
{
    if (!table || !channel || !channel->in_use || frame_budget <= 0)
        return -1;
    if (channel->state == DMESH_PEER_AUTHENTICATING) {
        uint8_t key[32];
        if (!table->transport || !table->transport->peer_key || !channel->conn)
            return -1;
        if (table->transport->peer_key(channel->conn, key) < 0)
            return 0;
        if (dmesh_peer_authenticated(table, channel, key) != DMESH_PEER_OK)
            return -1;
    }
    if (channel->state != DMESH_PEER_OPEN)
        return 0;

    if (channel->tx_len != 0) {
        long sent = table->transport && table->transport->send
                        ? table->transport->send(channel->conn, channel->tx_frame,
                                                 channel->tx_len)
                        : -1;
        if (sent == 0)
            return 0;
        if (sent != (long)channel->tx_len) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_TRANSPORT);
            dmesh_peer_reset(table, channel, "peer transport failed while flushing");
            return -1;
        }
        channel->tx_len = 0;
    }

    int progressed = 0;
    size_t frame_cap = sizeof(struct dmesh_peer_msg_header) + DMESH_PEER_FRAME_MAX;
    while (progressed < frame_budget) {
        struct dmesh_peer_msg_header header;
        const uint8_t *payload = NULL;
        long parsed = dmesh_peer_frame_parse(channel->rx_frame, channel->rx_len,
                                             &header, &payload);
        if (parsed < 0) {
            peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
            dmesh_peer_reset(table, channel, "malformed peer frame");
            return -1;
        }
        if (parsed == 0) {
            if (channel->rx_len == frame_cap) {
                peer_refuse(table, channel, DMESH_PEER_REFUSE_MALFORMED);
                dmesh_peer_reset(table, channel, "peer frame exceeded its bound");
                return -1;
            }
            long received = table->transport && table->transport->recv
                                ? table->transport->recv(
                                      channel->conn, channel->rx_frame + channel->rx_len,
                                      frame_cap - channel->rx_len)
                                : -1;
            if (received == 0)
                break;
            if (received < 0 || (size_t)received > frame_cap - channel->rx_len) {
                peer_refuse(table, channel, DMESH_PEER_REFUSE_TRANSPORT);
                dmesh_peer_reset(table, channel, "peer receive transport failed");
                return -1;
            }
            channel->rx_len += (uint32_t)received;
            continue;
        }
        if (peer_dispatch_frame(table, channel, &header, payload) != 0) {
            dmesh_peer_reset(table, channel, "peer frame was refused");
            return -1;
        }
        channel->rx_len -= (uint32_t)parsed;
        if (channel->rx_len)
            memmove(channel->rx_frame, channel->rx_frame + parsed, channel->rx_len);
        channel->last_active_ns = peer_now(table);
        progressed++;
        if (channel->tx_len != 0)
            break;                       /* response ordering before more input */
    }
    if (channel->state == DMESH_PEER_OPEN && channel->tx_len == 0 &&
        (peer_ack_drain(table, channel), channel->ack_staged) &&
        dmesh_peer_ack_flush(table, channel) < 0) {
        peer_refuse(table, channel, DMESH_PEER_REFUSE_TRANSPORT);
        dmesh_peer_reset(table, channel, "peer acknowledgement transport failed");
        return -1;
    }
    return progressed;
}
