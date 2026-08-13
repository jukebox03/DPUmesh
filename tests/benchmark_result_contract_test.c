#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../bench/apps/bench_result.h"

int
main(void)
{
    assert(strcmp(bench_result_status(1, 0, 0), "OK") == 0);
    assert(strcmp(bench_result_status(1000, 0, 0), "OK") == 0);
    assert(strcmp(bench_result_status(0, 0, 0), "ERR") == 0);
    assert(strcmp(bench_result_status(1000, 1, 0), "ERR") == 0);
    assert(strcmp(bench_result_status(1000, 0, 1), "ERR") == 0);
    puts("benchmark_result_contract_test: PASS");
    return 0;
}
