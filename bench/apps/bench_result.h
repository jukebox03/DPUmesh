/* Shared benchmark result validity contract. */
#ifndef DPUMESH_BENCH_RESULT_H
#define DPUMESH_BENCH_RESULT_H

static inline const char *
bench_result_status(long completed, long failures, int failed_workers)
{
    return completed > 0 && failures == 0 && failed_workers == 0 ? "OK" : "ERR";
}

#endif /* DPUMESH_BENCH_RESULT_H */
