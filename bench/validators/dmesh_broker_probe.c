#include <errno.h>
#include <stdio.h>

#include <dpumesh/dmesh.h>

int main(void)
{
    dmesh_channel_t *channel = dmesh_create_channel();
    if (channel == NULL) {
        perror("dmesh_broker_probe: init");
        return 1;
    }
    int pod_id = dmesh_pod_id(channel);
    if (pod_id < 0) {
        fprintf(stderr, "dmesh_broker_probe: invalid pod id %d\n", pod_id);
        (void)dmesh_destroy_channel(channel);
        return 1;
    }
    if (dmesh_destroy_channel(channel) != 0) {
        perror("dmesh_broker_probe: destroy");
        return 1;
    }
    printf("dmesh_broker_probe: READY pod_id=%d teardown=ok\n", pod_id);
    return 0;
}
