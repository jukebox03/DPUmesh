#define _GNU_SOURCE
#include "local_control.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct dmesh_local_control {
    SSL_CTX *ctx;
    SSL *ssl;
    int listener, fd, authenticated;
    char *host_uri;
    uint8_t session[16];
    uint64_t sequence;
    time_t activity;
    size_t received, sent, pending;
    struct dmesh_local_request request, previous;
    struct dmesh_local_response response, previous_response;
    dmesh_local_dispatch dispatch;
    dmesh_local_retire retire;
    dmesh_local_available available;
    void *owner;
};
static uint64_t decode(const uint8_t *p, size_t n)
{ uint64_t v = 0; for (size_t i = 0; i < n; i++) v |= (uint64_t)p[i] << (i * 8); return v; }
static void encode(uint8_t *p, uint64_t v, size_t n)
{ for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(v >> (i * 8)); }
static int zeros(const void *ptr, size_t n)
{ const uint8_t *p = ptr; for (size_t i = 0; i < n; i++) if (p[i]) return 0; return 1; }
static time_t monotonic_seconds(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }
static void disconnect_peer(struct dmesh_local_control *c)
{
    if (c->authenticated) c->retire(c->owner);
    c->authenticated = 0;
    SSL_free(c->ssl); c->ssl = NULL;
    if (c->fd >= 0) close(c->fd);
    c->fd = -1; c->received = c->sent = c->pending = 0; c->sequence = 0;
    memset(c->session, 0, sizeof(c->session));
}
static int peer_allowed(struct dmesh_local_control *c)
{
    if (SSL_get_verify_result(c->ssl) != X509_V_OK) return 0;
    X509 *cert = SSL_get1_peer_certificate(c->ssl);
    if (!cert) return 0;
    GENERAL_NAMES *names = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
    int count = 0, matched = 0;
    for (int i = 0; names && i < sk_GENERAL_NAME_num(names); i++) {
        const GENERAL_NAME *name = sk_GENERAL_NAME_value(names, i);
        if (name->type != GEN_URI) continue;
        count++;
        const ASN1_STRING *uri = name->d.uniformResourceIdentifier;
        matched = ASN1_STRING_length(uri) == (int)strlen(c->host_uri) &&
                  memcmp(ASN1_STRING_get0_data(uri), c->host_uri, strlen(c->host_uri)) == 0;
    }
    GENERAL_NAMES_free(names); X509_free(cert);
    return count == 1 && matched;
}
struct dmesh_local_control *dmesh_local_control_create(
    const char *address, unsigned port, const char *ca, const char *certificate,
    const char *key, const char *host_uri, dmesh_local_dispatch dispatch,
    dmesh_local_retire retire, dmesh_local_available available, void *owner)
{
    if (!address || !ca || !certificate || !key || !host_uri || !*host_uri ||
        !port || port > 65535 || !dispatch || !retire || !available) return NULL;
    struct dmesh_local_control *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = c->listener = -1; c->host_uri = strdup(host_uri);
    c->dispatch = dispatch; c->retire = retire; c->available = available; c->owner = owner;
    c->ctx = SSL_CTX_new(TLS_server_method());
    if (!c->host_uri || !c->ctx ||
        !SSL_CTX_set_min_proto_version(c->ctx, TLS1_3_VERSION) ||
        !SSL_CTX_load_verify_locations(c->ctx, ca, NULL) ||
        !SSL_CTX_use_certificate_chain_file(c->ctx, certificate) ||
        !SSL_CTX_use_PrivateKey_file(c->ctx, key, SSL_FILETYPE_PEM) ||
        !SSL_CTX_check_private_key(c->ctx)) goto failed;
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    SSL_CTX_set_session_cache_mode(c->ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_num_tickets(c->ctx, 0);
    SSL_CTX_set_max_early_data(c->ctx, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, address, &a.sin_addr) != 1) goto failed;
    c->listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    if (c->listener < 0 || setsockopt(c->listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        bind(c->listener, (struct sockaddr *)&a, sizeof(a)) || listen(c->listener, 8)) goto failed;
    return c;
failed:
    dmesh_local_control_destroy(c); return NULL;
}
static void reply(struct dmesh_local_control *c, uint64_t seq, unsigned status)
{
    memcpy(c->response.magic, DMESH_LOCAL_MAGIC, 8);
    memcpy(c->response.session, c->session, 16);
    encode(c->response.sequence, seq, 8); encode(c->response.status, status, 4);
    c->sent = 0; c->pending = sizeof(c->response);
}
static int wants_io(SSL *ssl, int result)
{ int e = SSL_get_error(ssl, result); return e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE; }
int dmesh_local_control_progress(struct dmesh_local_control *c)
{
    if (!c) return 0;
    int fd = accept4(c->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0) {
        if (c->ssl || !c->available(c->owner)) close(fd);
        else {
            c->fd = fd; c->ssl = SSL_new(c->ctx);
            if (!c->ssl || !SSL_set_fd(c->ssl, fd)) { disconnect_peer(c); return 1; }
            SSL_set_accept_state(c->ssl); c->activity = monotonic_seconds();
        }
    }
    if (!c->ssl) return 0;
    if (monotonic_seconds() - c->activity >= (c->authenticated ? 15 : 3)) goto failed;
    if (!c->authenticated) {
        int r = SSL_accept(c->ssl);
        if (r != 1) { if (wants_io(c->ssl, r)) return 0; goto failed; }
        if (!peer_allowed(c) || !c->available(c->owner) || RAND_bytes(c->session, 16) != 1) goto failed;
        c->authenticated = 1; c->activity = monotonic_seconds(); reply(c, 0, DMESH_LOCAL_OK);
    }
    if (c->pending) {
        int r = SSL_write(c->ssl, (uint8_t *)&c->response + c->sent, c->pending - c->sent);
        if (r <= 0) { if (wants_io(c->ssl, r)) return 0; goto failed; }
        c->sent += r;
        if (c->sent == c->pending) c->pending = c->sent = 0;
        return 1;
    }
    int r = SSL_read(c->ssl, (uint8_t *)&c->request + c->received, sizeof(c->request) - c->received);
    if (r <= 0) { if (wants_io(c->ssl, r)) return 0; goto failed; }
    c->received += r;
    if (c->received != sizeof(c->request)) return 1;
    c->received = 0;
    uint64_t seq = decode(c->request.sequence, 8);
    if (memcmp(c->session, c->request.session, 16) || !zeros(c->request.reserved, 7)) goto failed;
    if (seq && seq == c->sequence && !memcmp(&c->request, &c->previous, sizeof(c->request))) {
        c->response = c->previous_response; c->pending = sizeof(c->response);
    } else {
        if (!seq || seq <= c->sequence) goto failed;
        unsigned status;
        if (c->request.operation == DMESH_LOCAL_PING)
            status = zeros(c->request.connection_id, 32) && zeros(&c->request.identity, sizeof(c->request.identity)) ? DMESH_LOCAL_OK : DMESH_LOCAL_INVALID;
        else status = c->dispatch(c->owner, &c->request);
        c->sequence = seq; c->previous = c->request;
        reply(c, seq, status); c->previous_response = c->response;
    }
    c->activity = monotonic_seconds();
    return 1;
failed:
    disconnect_peer(c); return 1;
}
void dmesh_local_control_destroy(struct dmesh_local_control *c)
{
    if (!c) return;
    disconnect_peer(c);
    if (c->listener >= 0) close(c->listener);
    SSL_CTX_free(c->ctx); free(c->host_uri); free(c);
}
