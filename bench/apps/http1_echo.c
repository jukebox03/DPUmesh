/* HTTP/1.1 echo server for the protocol-aware path.
 *
 * The DPU's protocol-aware treatment decides what it is looking at from the
 * bytes; every other workload in this tree speaks either the binary Greeter
 * frame or gRPC, so this server is the one that drives the HTTP/1 branch of
 * that decision: one request, one response, keep-alive, no routing of its own.
 *
 * The reply size is a request header rather than a build-time constant, so one
 * server serves every point the client offers.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define ECHO_PORT_DEFAULT 9103
#define HEADER_MAX  8192
#define REPLY_MAX   (4 * 1024 * 1024)
#define REPLY_HEADER_NAME "X-Reply-Size:"

static ssize_t write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

/* One header value, case-insensitively, out of a complete header block. */
static long header_value(const char *headers, const char *name, long fallback) {
    size_t name_len = strlen(name);
    for (const char *line = headers; line && *line; ) {
        if (strncasecmp(line, name, name_len) == 0)
            return strtol(line + name_len, NULL, 10);
        const char *next = strstr(line, "\r\n");
        line = next ? next + 2 : NULL;
    }
    return fallback;
}

static void *conn_fn(void *arg) {
    int fd = (int)(intptr_t)arg;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    char buf[HEADER_MAX];
    size_t held = 0;
    uint8_t *body = NULL;
    size_t body_cap = 0;
    char *reply = malloc(REPLY_MAX);
    if (!reply) { close(fd); return NULL; }

    for (;;) {
        /* Headers end at the first blank line; anything after it is body, and
         * a pipelined next request may already be sitting behind that. */
        char *end = NULL;
        while (!(end = memmem(buf, held, "\r\n\r\n", 4))) {
            if (held == sizeof buf - 1) goto done;
            ssize_t n = read(fd, buf + held, sizeof buf - 1 - held);
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; goto done; }
            held += (size_t)n;
        }
        size_t header_len = (size_t)(end - buf) + 4;
        buf[header_len - 1] = '\0';
        long content_length = header_value(buf, "Content-Length:", 0);
        long reply_size = header_value(buf, REPLY_HEADER_NAME, 8);
        buf[header_len - 1] = '\n';
        if (content_length < 0 || reply_size < 0 || reply_size > REPLY_MAX - 256)
            goto done;

        if ((size_t)content_length > body_cap) {
            uint8_t *grown = realloc(body, (size_t)content_length);
            if (!grown) goto done;
            body = grown; body_cap = (size_t)content_length;
        }
        size_t carried = held - header_len;
        size_t take = carried < (size_t)content_length ? carried : (size_t)content_length;
        if (take) memcpy(body, buf + header_len, take);
        memmove(buf, buf + header_len + take, carried - take);
        held = carried - take;
        for (size_t got = take; got < (size_t)content_length; ) {
            ssize_t n = read(fd, body + got, (size_t)content_length - got);
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; goto done; }
            got += (size_t)n;
        }

        int head = snprintf(reply, 256,
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/octet-stream\r\n"
                            "Content-Length: %ld\r\n\r\n", reply_size);
        memset(reply + head, 0, (size_t)reply_size);
        if (write_all(fd, reply, (size_t)head + (size_t)reply_size) < 0) goto done;
    }
done:
    free(reply); free(body); close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    int port = ECHO_PORT_DEFAULT;
    if (getenv("ECHO_PORT")) port = atoi(getenv("ECHO_PORT"));
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--port") || !strcmp(argv[i], "-p")) port = atoi(argv[i + 1]);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(srv, 128) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[http1_echo] LISTEN on :%d\n", port);

    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        pthread_t t;
        if (pthread_create(&t, NULL, conn_fn, (void *)(intptr_t)c) != 0) { close(c); continue; }
        pthread_detach(t);
    }
}
