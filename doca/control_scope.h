#ifndef DMESH_CONTROL_SCOPE_H
#define DMESH_CONTROL_SCOPE_H

#include <stddef.h>

struct objects;

/* Scope of the control-plane credential.
 *
 * A DPU authenticates to the Linkerd control plane with one credential per
 * node and names the workload it is asking about as a plain string, so the
 * upstream API cannot express "only the Pods this node serves". Where the API
 * cannot express the restriction, the component that already binds Pods to
 * nodes mediates the lookup: the DPU asks the controller, and the controller
 * answers only for Pods the generation places on the asking node.
 *
 * The DPU has no route into the cluster CIDRs, so the question travels the one
 * channel every other control message takes: the host runtime's mTLS tunnel.
 * The client certificate binds the request to its node.
 */

enum dmesh_scope_state {
    DMESH_SCOPE_REFUSED = -1,   /* the generation places that Pod elsewhere */
    DMESH_SCOPE_UNKNOWN = 0,    /* not asked, or the controller did not answer */
    DMESH_SCOPE_ALLOWED = 1,
};

/* Read DPUMESH_CONTROLLER_SCOPE_URL. An unset variable leaves mediation
 * disabled, which is the deployment that has no controller: every Pod is then
 * unknown and the destination Service's protection class decides. */
int dmesh_scope_configure(struct objects *objs, char *error, size_t error_len);

/* Ask the controller whether this node may act for `pod_uid`. Runs on the
 * Comch control thread; never on a data worker. */
enum dmesh_scope_state dmesh_scope_query(struct objects *objs, const char *pod_uid);

/* Re-ask for every live registration. Called when a generation is adopted:
 * a Pod that moved is refused from the next answer onward. */
void dmesh_scope_refresh(struct objects *objs);

#endif /* DMESH_CONTROL_SCOPE_H */
