/* Compile on the DPU against the deployed C source; no runtime mutation. */
#include <stddef.h>
#include "../../../../doca/dpu_proxy.c"
int main(void) {
    printf("{\"worker_objs\":%zu,\"objs_proxy\":%zu,\"proxy_chunk_free\":%zu,\"pool_chunks\":%u}\n",
           offsetof(struct px_worker_state, objs), offsetof(struct objects, proxy),
           offsetof(struct dmesh_proxy, chunk_free), PX_ARENA_CHUNKS);
    return 0;
}
