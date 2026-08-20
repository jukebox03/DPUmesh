/* The mediated control-plane lookup. One bounded HTTP request per Pod, on the
 * control thread, answered by the controller through this node's agent. */
#include "control_scope.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <doca_log.h>

#include "object.h"

DOCA_LOG_REGISTER(CONTROL_SCOPE);

/* An answer is a status line and a short JSON body; nothing here is large, and
 * a peer sending more than this is not answering the question that was asked. */
#define SCOPE_REPLY_MAX 2048
#define SCOPE_TIMEOUT_SEC 3

int
dmesh_scope_configure(struct objects *objs, char *error, size_t error_len)
{
    const char *url = getenv("DPUMESH_CONTROLLER_SCOPE_URL");
    objs->scope_host[0] = '\0';
    objs->scope_port = 0;
    if (url == NULL || *url == '\0')
        return 0;                       /* mediation disabled */

    const char *rest = url;
    if (strncmp(rest, "http://", 7) == 0)
        rest += 7;
    const char *colon = strrchr(rest, ':');
    if (colon == NULL || colon == rest) {
        if (error && error_len)
            snprintf(error, error_len,
                     "DPUMESH_CONTROLLER_SCOPE_URL must be http://HOST:PORT");
        return -1;
    }
    size_t host_len = (size_t)(colon - rest);
    if (host_len >= sizeof(objs->scope_host)) {
        if (error && error_len)
            snprintf(error, error_len, "DPUMESH_CONTROLLER_SCOPE_URL host is too long");
        return -1;
    }
    memcpy(objs->scope_host, rest, host_len);
    objs->scope_host[host_len] = '\0';
    long port = strtol(colon + 1, NULL, 10);
    if (port <= 0 || port > 65535) {
        if (error && error_len)
            snprintf(error, error_len, "DPUMESH_CONTROLLER_SCOPE_URL port is out of range");
        objs->scope_host[0] = '\0';
        return -1;
    }
    objs->scope_port = (uint16_t)port;
    DOCA_LOG_INFO("control-plane scope mediated by %s:%u",
                  objs->scope_host, (unsigned)objs->scope_port);
    return 0;
}

/* A Pod UID is the only thing that travels, and only in its canonical form:
 * anything else would be a request this DPU composed rather than one the
 * generation named. */
static int
scope_uid_ok(const char *pod_uid)
{
    if (pod_uid == NULL)
        return 0;
    size_t n = strlen(pod_uid);
    if (n != 36)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = pod_uid[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

enum dmesh_scope_state
dmesh_scope_query(struct objects *objs, const char *pod_uid)
{
    if (objs->scope_host[0] == '\0' || objs->scope_port == 0)
        return DMESH_SCOPE_UNKNOWN;
    if (!scope_uid_ok(pod_uid))
        return DMESH_SCOPE_REFUSED;

    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)objs->scope_port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *resolved = NULL;
    if (getaddrinfo(objs->scope_host, port, &hints, &resolved) != 0 || resolved == NULL)
        return DMESH_SCOPE_UNKNOWN;

    int fd = socket(resolved->ai_family, resolved->ai_socktype | SOCK_CLOEXEC,
                    resolved->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(resolved);
        return DMESH_SCOPE_UNKNOWN;
    }
    struct timeval timeout;
    memset(&timeout, 0, sizeof(timeout));
    timeout.tv_sec = SCOPE_TIMEOUT_SEC;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int connected = connect(fd, resolved->ai_addr, resolved->ai_addrlen);
    freeaddrinfo(resolved);
    if (connected != 0) {
        close(fd);
        return DMESH_SCOPE_UNKNOWN;
    }

    char request[512];
    int n = snprintf(request, sizeof(request),
                     "GET /workload-scope?pod_uid=%s HTTP/1.0\r\n"
                     "Host: %s\r\nConnection: close\r\n\r\n",
                     pod_uid, objs->scope_host);
    if (n < 0 || (size_t)n >= sizeof(request) ||
        write(fd, request, (size_t)n) != n) {
        close(fd);
        return DMESH_SCOPE_UNKNOWN;
    }

    char reply[SCOPE_REPLY_MAX + 1];
    size_t got = 0;
    while (got < SCOPE_REPLY_MAX) {
        ssize_t r = read(fd, reply + got, SCOPE_REPLY_MAX - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    reply[got] = '\0';
    if (got < 12 || strncmp(reply, "HTTP/1.", 7) != 0)
        return DMESH_SCOPE_UNKNOWN;
    /* 200 is the only answer that widens what this DPU may ask about; a 403 or
     * a 404 is the controller saying the generation does not place that Pod
     * here, which is a decision rather than an outage. */
    if (strncmp(reply + 9, "200", 3) == 0)
        return DMESH_SCOPE_ALLOWED;
    if (strncmp(reply + 9, "403", 3) == 0 || strncmp(reply + 9, "404", 3) == 0)
        return DMESH_SCOPE_REFUSED;
    return DMESH_SCOPE_UNKNOWN;
}

void
dmesh_scope_refresh(struct objects *objs)
{
    if (objs->scope_host[0] == '\0')
        return;
    int np = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
    for (int i = 0; i < np; i++) {
        struct pod_state *pod = &objs->pods[i];
        if (!__atomic_load_n(&pod->registered, __ATOMIC_ACQUIRE) ||
            pod->pod_uid[0] == '\0')
            continue;
        enum dmesh_scope_state state = dmesh_scope_query(objs, pod->pod_uid);
        /* An unanswered question withdraws nothing: the previous answer
         * stands, which is the fail-static contract every feed already has. */
        if (state == DMESH_SCOPE_UNKNOWN)
            continue;
        int8_t previous = __atomic_exchange_n(&pod->scope_state, (int8_t)state,
                                              __ATOMIC_RELEASE);
        if (previous != (int8_t)state)
            DOCA_LOG_INFO("control-plane scope for pod_uid=%s is now %s",
                          pod->pod_uid,
                          state == DMESH_SCOPE_ALLOWED ? "allowed" : "refused");
    }
}
