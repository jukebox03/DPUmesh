#ifndef DMESH_IPC_H
#define DMESH_IPC_H

#include <stdint.h>

#include "doca/comch_common.h"

#define DMESH_IPC_MAGIC "DMESHBR1"
#define DMESH_IPC_VERSION 3
#define DMESH_IPC_SERVICE_LEN 64
#define DMESH_IPC_ERROR_TEXT_LEN 128

enum dmesh_ipc_type {
    DMESH_IPC_HELLO = 1,
    DMESH_IPC_READY = 2,
    DMESH_IPC_ERROR = 3,
    DMESH_IPC_RESOLVE = 4,
    DMESH_IPC_RESOLVE_ACK = 5,
    DMESH_IPC_TRANSPORT_DOWN = 6,
};

struct dmesh_ipc_hello {
    char magic[8];
    uint8_t type;
    uint8_t version;
    uint8_t pad[2];
    char service[DMESH_IPC_SERVICE_LEN];
};

struct dmesh_ipc_ready {
    uint8_t type;
    uint8_t version;
    uint8_t k_rings;
    uint8_t landing_stripes;
    uint16_t fd_count;
    uint16_t reserved;
    int32_t pod_id;
    int32_t service_id;
    uint64_t dpa_mmap_handle;
    uint32_t ring_slots;
    uint32_t rev_ring_bytes;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
};

struct dmesh_ipc_error {
    uint8_t type;
    uint8_t version;
    uint8_t pad[2];
    uint32_t code;
    char text[DMESH_IPC_ERROR_TEXT_LEN];
};

struct dmesh_ipc_resolve {
    uint8_t type;
    uint8_t version;
    uint8_t pad[2];
    uint32_t request_id;
    struct dmesh_resolve_msg payload;
};

struct dmesh_ipc_resolve_ack {
    uint8_t type;
    uint8_t version;
    uint8_t pad[2];
    uint32_t request_id;
    struct dmesh_resolve_ack_msg payload;
};

struct dmesh_ipc_transport_down {
    uint8_t type;
    uint8_t version;
    uint8_t pad[2];
    uint32_t reason;
};

_Static_assert(sizeof(struct dmesh_ipc_hello) == 76,
               "broker HELLO wire ABI changed");
_Static_assert(sizeof(struct dmesh_ipc_ready) == 48,
               "broker READY wire ABI changed");
_Static_assert(sizeof(struct dmesh_ipc_error) == 136,
               "broker ERROR wire ABI changed");
_Static_assert(sizeof(struct dmesh_ipc_resolve) ==
                   8 + sizeof(struct dmesh_resolve_msg),
               "broker RESOLVE wire ABI changed");
_Static_assert(sizeof(struct dmesh_ipc_resolve_ack) ==
                   8 + sizeof(struct dmesh_resolve_ack_msg),
               "broker RESOLVE_ACK wire ABI changed");

#endif /* DMESH_IPC_H */
