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

    puts("l4_pin_policy_test: PASS");
    return 0;
}
