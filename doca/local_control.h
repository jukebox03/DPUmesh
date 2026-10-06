#ifndef DMESH_LOCAL_CONTROL_H
#define DMESH_LOCAL_CONTROL_H
#include <stdint.h>
#include "comch_common.h"

/* Fixed, bounded control frames for the paired host: liveness and
 * registration teardown. Identities do not travel here; they arrive on Comch
 * as assertions sealed under this session's id (dmesh_local_control_session). */
#define DMESH_LOCAL_MAGIC "DMESHLC2"
#define DMESH_LOCAL_VERSION 6
/* Operation 2 is retired and never reassigned. */
enum { DMESH_LOCAL_PING = 1, DMESH_LOCAL_UNREGISTER = 3, DMESH_LOCAL_STATUS = 4 };
enum { DMESH_LOCAL_OK = 0, DMESH_LOCAL_INVALID, DMESH_LOCAL_STALE,
       DMESH_LOCAL_PENDING };
struct dmesh_local_request {
    uint8_t operation, reserved[7], sequence[8],
            session[DMESH_CONTROL_SESSION_SIZE], connection_id[DMESH_REG_NONCE_SIZE];
};
struct dmesh_local_response {
    uint8_t magic[8], session[DMESH_CONTROL_SESSION_SIZE], sequence[8], status[4];
};
_Static_assert(sizeof(struct dmesh_local_request) == 64, "local request ABI");
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
/* Copy the authenticated session id into `out`. Returns 0 only while a peer
 * holds an authenticated session; every new session has a fresh id. */
int dmesh_local_control_session(const struct dmesh_local_control *control,
                                uint8_t out[DMESH_CONTROL_SESSION_SIZE]);
void dmesh_local_control_destroy(struct dmesh_local_control *control);
#endif
