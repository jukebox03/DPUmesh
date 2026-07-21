#ifndef DPU_L7_H
#define DPU_L7_H

/* Bounded L7 frame decoder used by the ARM proxy. */

#include <stdint.h>

/* Read-only routing context for one frame. */
struct dmesh_l7_ctx {
    int32_t        service;
    int32_t        client_pod;
    uint16_t       client_port;
    const int32_t *hosts;
    int32_t        n_hosts;
};

/* Select a live backend through the proxy load balancer. */
#define DMESH_LB_DEFER (-1)

/* One decoded frame and its optional route override. */
struct dmesh_l7_decision {
    uint32_t total_len;
    int32_t  cluster;
    int32_t  host;
};

enum dmesh_l7_direction {
    DMESH_L7_REQUEST = 0,
    DMESH_L7_RESPONSE = 1,
};

/* Return 1 for a decoded frame, 0 for more head bytes, or -1 for invalid input. */
int dmesh_l7_decode(enum dmesh_l7_direction direction,
                    const uint8_t *buf, uint32_t len,
                    const struct dmesh_l7_ctx *ctx,
                    struct dmesh_l7_decision *out);

#endif /* DPU_L7_H */
