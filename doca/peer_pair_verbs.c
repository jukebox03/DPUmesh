#include "peer_pair_transport.h"
int peer_pair_transport_new(const struct peer_pair_transport_config *cfg,
    struct ibv_context *verbs, uint8_t port, uint32_t gid, uint16_t mtu,
    struct peer_pair_transport **out, char *error, size_t error_len)
{
    if (!out) return -1;
    *out = NULL;
    struct peer_pair_driver d = {.prepare = peer_wire_verbs_prepare,
        .activate = peer_wire_verbs_activate, .send_drained = peer_wire_verbs_send_drained,
        .close = peer_wire_verbs_close};
    if (peer_wire_verbs_new(verbs, port, gid, mtu, &d.wire, &d.ctx, error, error_len)) return -1;
    if (!peer_pair_transport_new_driver(cfg, &d, out)) return 0;
    d.wire->ctx_free(d.ctx); return -1;
}
