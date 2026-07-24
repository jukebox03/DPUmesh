#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <dpumesh/dmesh_topology.h>

static void
check_topology(int n, int f, int a)
{
    assert(dmesh_topology_valid(f, n, a));
    for (int pod = 0; pod < 32; pod++) {
        unsigned seen = 0;
        for (int ring = 0; ring < f; ring++) {
            int eu = dmesh_dpa_eu_for_ring(pod, f, ring, n, a);
            assert(eu >= 0 && eu < n);
            assert(eu % a == ring % a);
            assert((seen & (1u << eu)) == 0);
            seen |= 1u << eu;
        }
    }
    for (uint32_t port = 0; port < 65536u; port += 97u) {
        int ring = dmesh_forward_ring((uint16_t)port, f);
        assert(dmesh_worker_for_ring(ring, a) ==
               dmesh_worker_for_port((uint16_t)port, a));
    }
}

int
main(void)
{
    check_topology(8, 2, 2);
    check_topology(16, 4, 2);
    check_topology(16, 4, 4);
    check_topology(32, 8, 4);
    check_topology(32, 8, 8);

    assert(!dmesh_topology_valid(2, 8, 4));
    assert(!dmesh_topology_valid(4, 10, 4));
    assert(dmesh_dpa_eu_for_ring(0, 2, 2, 8, 2) == -1);

    puts("topology_test: PASS");
    return 0;
}
