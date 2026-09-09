#ifndef DMESH_LOCAL_CONTROL_H
#define DMESH_LOCAL_CONTROL_H
#include <stdint.h>
#include "comch_common.h"

/* Fixed, bounded control frames. The assertion layout is reused only as a
 * canonical metadata encoding; direct REGISTER requires a zero signature and
 * is accepted exclusively on the authenticated host control connection. */
#define DMESH_LOCAL_MAGIC "DMESHLC1"
#define DMESH_LOCAL_VERSION 4
enum { DMESH_LOCAL_PING = 1, DMESH_LOCAL_REGISTER, DMESH_LOCAL_UNREGISTER,
       DMESH_LOCAL_STATUS };
enum { DMESH_LOCAL_OK = 0, DMESH_LOCAL_INVALID, DMESH_LOCAL_STALE,
       DMESH_LOCAL_PENDING };
struct dmesh_local_request {
    uint8_t operation, reserved[7], sequence[8], session[16], connection_id[32];
    struct dmesh_workload_assert_msg identity;
};
struct dmesh_local_response {
    uint8_t magic[8], session[16], sequence[8], status[4];
};
_Static_assert(sizeof(struct dmesh_local_request) == 1609, "local request ABI");
_Static_assert(sizeof(struct dmesh_local_response) == 36, "local response ABI");
struct dmesh_local_control;
typedef unsigned (*dmesh_local_dispatch)(void *, const struct dmesh_local_request *);
typedef void (*dmesh_local_retire)(void *);
typedef int (*dmesh_local_available)(void *);
struct dmesh_local_control *dmesh_local_control_create(
    const char *address, unsigned port, const char *ca, const char *certificate,
    const char *key, const char *host_uri, dmesh_local_dispatch dispatch,
    dmesh_local_retire retire, dmesh_local_available available, void *owner);
int dmesh_local_control_progress(struct dmesh_local_control *control);
void dmesh_local_control_destroy(struct dmesh_local_control *control);
#endif
