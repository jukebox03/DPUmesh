#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doca/comch_server.h"
#include "doca/object.h"

int
main(void)
{
    struct objects *objs = calloc(1, sizeof(*objs));
    assert(objs != NULL);
    objs->num_dpa_threads = 4;
    objs->k_rings = 2;
    for (int i = 0; i < POD_ID_SPACE; i++)
        objs->pod_id_to_slot[i] = -1;

    struct doca_comch_connection *conn =
        (struct doca_comch_connection *)(uintptr_t)0x1000;
    assert(pods_add_connection(objs, conn) == 0);

    int assigned = pods_register(objs, conn, -1, 7);
    assert(assigned == 0);
    assert(objs->pods[0].registered == 1);
    assert(objs->pod_id_to_slot[assigned] == 0);

    /* Identical REGISTER is a replay, not a conflicting second tenant. */
    assert(pods_register(objs, conn, -1, 7) == assigned);
    assert(pods_register(objs, conn, assigned, 7) == assigned);
    assert(pods_register(objs, conn, -1, 8) == -1);

    /* UNREGISTER is replay-safe while cleanup is pending, and registration cannot
     * reopen a slot whose imported resources are still quiescing. */
    assert(pods_unregister_connection(objs, conn, assigned) == 0);
    assert(pods_unregister_connection(objs, conn, assigned) == 0);
    assert(objs->pods[0].cleanup_pending == 1);
    assert(pods_register(objs, conn, -1, 7) == -1);

    free(objs);
    puts("native_control_state_test: PASS");
    return 0;
}
