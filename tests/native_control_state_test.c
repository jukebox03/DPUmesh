#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doca/comch_server.h"
#include "doca/object.h"
#include "doca/topology.h"

/* The compact id is the DPU's interning of the held generation, so a serving
 * registration needs one. */
static void
install_generation(struct objects *objs)
{
    static const char body[] =
        "version=100\n"
        "service=test-bench/echo-dpumesh,10.96.0.11:9092\n";
    struct dmesh_topology_tables *tables = NULL;
    assert(dmesh_topology_parse(body, sizeof(body) - 1,
                                objs->topology.interned,
                                &tables) == DMESH_TOPOLOGY_ADOPTED);
    objs->topology.tables = tables;
    objs->topology.enabled = 1;
}

int
main(void)
{
    struct objects *objs = calloc(1, sizeof(*objs));
    assert(objs != NULL);
    objs->num_dpa_threads = 4;
    objs->k_rings = 2;
    objs->n_data_workers = 2;
    for (int i = 0; i < POD_ID_SPACE; i++)
        objs->pod_id_to_slot[i] = -1;

    struct doca_comch_connection *conn =
        (struct doca_comch_connection *)(uintptr_t)0x1000;
    assert(pods_add_connection(objs, conn) == 0);

    /* The slot lifecycle below runs on an admitted registration. */
    objs->pods[0].registration_grant_verified = 1;
    snprintf(objs->pods[0].granted_service,
             sizeof(objs->pods[0].granted_service), "echo-dpumesh");
    snprintf(objs->pods[0].namespace_name,
             sizeof(objs->pods[0].namespace_name), "test-bench");

    /* Serving an identity requires the generation that defines it. */
    assert(pods_register(objs, conn, -1, "echo-dpumesh") == -1);
    install_generation(objs);
    int interned = dmesh_topology_interned_id(objs, "test-bench/echo-dpumesh");
    assert(interned >= 0);

    int assigned = pods_register(objs, conn, -1, "echo-dpumesh");
    assert(assigned == 0);
    assert(objs->pods[0].registered == 1);
    assert(objs->pods[0].service_id == interned);
    assert(objs->pod_id_to_slot[assigned] == 0);
    assert(objs->pods[0].landing_stripes == 2);

    /* Identical REGISTER is a replay, not a conflicting second tenant. */
    assert(pods_register(objs, conn, -1, "echo-dpumesh") == assigned);
    assert(pods_register(objs, conn, assigned, "echo-dpumesh") == assigned);
    assert(pods_register(objs, conn, -1, "another-svc") == -1);

    /* UNREGISTER is replay-safe while cleanup is pending, and registration cannot
     * reopen a slot whose imported resources are still quiescing. */
    assert(pods_unregister_connection(objs, conn, assigned) == 0);
    assert(pods_unregister_connection(objs, conn, assigned) == 0);
    assert(objs->pods[0].cleanup_pending == 1);
    assert(pods_register(objs, conn, -1, "echo-dpumesh") == -1);

    dmesh_topology_tables_free(objs->topology.tables);
    free(objs);

    /* Registration admits only the exact Service the verified, unconsumed
     * connection assertion named. A normal REGISTER retry remains idempotent
     * after the assertion has been consumed. */
    objs = calloc(1, sizeof(*objs));
    assert(objs != NULL);
    objs->num_dpa_threads = 4;
    objs->k_rings = 2;
    objs->n_data_workers = 2;
    for (int i = 0; i < POD_ID_SPACE; i++)
        objs->pod_id_to_slot[i] = -1;
    conn = (struct doca_comch_connection *)(uintptr_t)0x2000;
    assert(pods_add_connection(objs, conn) == 0);
    install_generation(objs);
    assert(pods_register(objs, conn, -1, "echo-dpumesh") == -1);
    objs->pods[0].registration_grant_verified = 1;
    snprintf(objs->pods[0].granted_service,
             sizeof(objs->pods[0].granted_service), "echo-dpumesh");
    snprintf(objs->pods[0].namespace_name,
             sizeof(objs->pods[0].namespace_name), "test-bench");
    /* The name is the identity: a Service other than the asserted one is
     * refused, and so is a client-only registration under a serving grant. */
    assert(pods_register(objs, conn, -1, "another-svc") == -1);
    assert(pods_register(objs, conn, -1, "") == -1);
    assert(pods_register(objs, conn, -1, "echo-dpumesh") == 0);
    assert(objs->pods[0].registration_grant_consumed == 1);
    assert(pods_register(objs, conn, -1, "echo-dpumesh") == 0);
    dmesh_topology_tables_free(objs->topology.tables);
    free(objs);

    puts("native_control_state_test: PASS");
    return 0;
}
