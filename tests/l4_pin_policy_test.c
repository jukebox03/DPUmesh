#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "doca/dpu_proxy.h"

int
main(void)
{
    /* Live pins are stable. */
    assert(dmesh_l4_pinned_backend(11, 1) == 11);

    /* Backend loss is terminal. */
    assert(dmesh_l4_pinned_backend(11, 0) == -1);
    assert(dmesh_l4_pinned_backend(-1, 1) == -1);

    /* ---- destination-side admission ---- */

    int mixed = 1;
    /* A served verdict decides on its own, whichever way and whatever the
     * grading: a policy that refused is a decision, not an absence. */
    assert(dmesh_inbound_admits(1, 1, 1, &mixed) == 1 && mixed == 0);
    assert(dmesh_inbound_admits(0, 0, 0, &mixed) == 0 && mixed == 0);
    assert(dmesh_inbound_admits(1, 0, 1, &mixed) == 1 && mixed == 0);

    /* With no verdict, a protected destination refuses rather than admitting
     * an unauthenticated stream. */
    assert(dmesh_inbound_admits(-1, 1, 0, &mixed) == 0 && mixed == 0);
    assert(dmesh_inbound_admits(-1, 1, 1, &mixed) == 0 && mixed == 0);

    /* A Service outside the protected set carries the stream, which is what
     * being outside it is graded for. A deployment the generation grades not
     * at all is that case for every Service, so turning enforcement on cannot
     * refuse traffic no policy ever named. */
    assert(dmesh_inbound_admits(-1, 0, 0, &mixed) == 1 && mixed == 0);

    /* Except for the one mixed-mode rule: a protected caller cannot
     * authenticate an unprotected callee, so that call stands only where the
     * destination's own policy admitted it explicitly. */
    assert(dmesh_inbound_admits(-1, 0, 1, &mixed) == 0 && mixed == 1);
    assert(dmesh_inbound_admits(1, 0, 1, &mixed) == 1 && mixed == 0);

    /* The caller may pass no out-parameter. */
    assert(dmesh_inbound_admits(-1, 0, 1, NULL) == 0);

    puts("l4_pin_policy_test: PASS");
    return 0;
}
