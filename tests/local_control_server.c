#include "doca/local_control.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static volatile sig_atomic_t stopped;
static unsigned calls;
static void stop(int s) { (void)s; stopped = 1; }
static unsigned dispatch(void *owner, const struct dmesh_local_request *r)
{ (void)owner; (void)r; return ++calls; }
static void retire(void *owner) { (void)owner; }
static int available(void *owner) { (void)owner; return 1; }
int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, stop);
    struct dmesh_local_control *c = dmesh_local_control_create(
        "127.0.0.1", (unsigned)atoi(argv[1]), argv[2], argv[3], argv[4],
        "spiffe://dpumesh.io/node/worker-1", dispatch, retire, available, NULL);
    if (!c) return 1;
    puts("ready"); fflush(stdout);
    while (!stopped) { dmesh_local_control_progress(c); usleep(1000); }
    dmesh_local_control_destroy(c);
    return 0;
}
