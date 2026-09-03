#include "peer_tls.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

/* The credential is a key, not a name or a date, so the certificate carries a
 * hundred-year life rather than a renewal schedule. A node whose key is
 * withdrawn stops being believed when the generation stops binding it, which
 * is a control-plane event and not an expiry. */
#define PEER_TLS_CERT_DAYS   36525
/* X.509 bounds a common name at 64 characters and a Kubernetes node name may
 * be longer. The name is descriptive, so it is truncated rather than refused. */
#define PEER_TLS_CN_MAX      64

struct peer_tls_ctx {
    SSL_CTX *ssl_ctx;
    uint8_t  public_key[32];
};

struct peer_tls_conn {
    SSL     *ssl;
    BIO     *rbio;                  /* ciphertext in; owned by ssl */
    BIO     *wbio;                  /* ciphertext out; owned by ssl */
    uint8_t  peer_key[32];
    uint8_t  established;
    uint8_t  faulted;
};

#define PEER_TLS_ERROR(...)                                                    \
    do {                                                                       \
        if (error && error_len)                                                \
            snprintf(error, error_len, __VA_ARGS__);                           \
    } while (0)

/* The chain and certificate lifetime are not the reason to believe a peer, so
 * a self-signed certificate and clock skew are not errors here. Every other
 * failure still is: a malformed or unusable certificate cannot produce the
 * key the caller needs to pin. CertificateVerify proves possession, and the
 * caller pins that key against the held topology generation. */
static int peer_tls_verify(int preverified, X509_STORE_CTX *store)
{
    if (preverified)
        return 1;
    switch (X509_STORE_CTX_get_error(store)) {
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
    case X509_V_ERR_CERT_NOT_YET_VALID:
    case X509_V_ERR_CERT_HAS_EXPIRED:
        return 1;
    default:
        return 0;
    }
}

static X509 *peer_tls_certificate(EVP_PKEY *key, const char *node_name)
{
    X509 *cert = X509_new();
    if (!cert)
        return NULL;
    char cn[PEER_TLS_CN_MAX + 1];
    snprintf(cn, sizeof(cn), "%s", node_name && node_name[0] ? node_name : "dpumesh-node");
    X509_NAME *subject = X509_get_subject_name(cert);
    /* A serial distinguishes certificates issued by one authority; there is no
     * authority here and nothing consults it. */
    if (!X509_set_version(cert, 2) ||
        !ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) ||
        !X509_gmtime_adj(X509_getm_notBefore(cert), 0) ||
        !X509_time_adj_ex(X509_getm_notAfter(cert), PEER_TLS_CERT_DAYS, 0, NULL) ||
        !X509_set_pubkey(cert, key) ||
        !X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                    (const unsigned char *)cn, -1, -1, 0) ||
        !X509_set_issuer_name(cert, subject) ||
        /* Ed25519 signs the message itself; OpenSSL wants a NULL digest. */
        X509_sign(cert, key, NULL) == 0) {
        X509_free(cert);
        return NULL;
    }
    return cert;
}

int peer_tls_ctx_new(const uint8_t seed[32], const char *node_name,
                     struct peer_tls_ctx **out, char *error, size_t error_len)
{
    if (!seed || !out)
        return -1;
    *out = NULL;
    ERR_clear_error();

    struct peer_tls_ctx *ctx = calloc(1, sizeof(*ctx));
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    X509 *cert = NULL;
    if (!ctx || !key) {
        PEER_TLS_ERROR("cannot load the node credential");
        goto fail;
    }
    size_t len = sizeof(ctx->public_key);
    if (EVP_PKEY_get_raw_public_key(key, ctx->public_key, &len) != 1 ||
        len != sizeof(ctx->public_key)) {
        PEER_TLS_ERROR("cannot derive the node public key");
        goto fail;
    }
    cert = peer_tls_certificate(key, node_name);
    if (!cert) {
        PEER_TLS_ERROR("cannot self-sign the node certificate");
        goto fail;
    }
    ctx->ssl_ctx = SSL_CTX_new(TLS_method());
    if (!ctx->ssl_ctx) {
        PEER_TLS_ERROR("cannot create the TLS context");
        goto fail;
    }
    if (!SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_3_VERSION) ||
        !SSL_CTX_set_max_proto_version(ctx->ssl_ctx, TLS1_3_VERSION)) {
        PEER_TLS_ERROR("cannot restrict the context to TLS 1.3");
        goto fail;
    }
    if (SSL_CTX_use_certificate(ctx->ssl_ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey(ctx->ssl_ctx, key) != 1 ||
        SSL_CTX_check_private_key(ctx->ssl_ctx) != 1) {
        PEER_TLS_ERROR("the node credential does not match its certificate");
        goto fail;
    }
    /* Both ends present a certificate: this is the mutual half. The verdict is
     * still the caller's, from the key peer_tls_peer_key hands back. */
    SSL_CTX_set_verify(ctx->ssl_ctx,
                       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       peer_tls_verify);
    /* No resumption: a channel that reconnects agrees a new key rather than
     * reviving the one a previous incarnation used. */
    SSL_CTX_set_options(ctx->ssl_ctx, SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(ctx->ssl_ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_num_tickets(ctx->ssl_ctx, 0);

    X509_free(cert);
    EVP_PKEY_free(key);
    *out = ctx;
    return 0;

fail:
    if (cert)
        X509_free(cert);
    if (key)
        EVP_PKEY_free(key);
    if (ctx) {
        if (ctx->ssl_ctx)
            SSL_CTX_free(ctx->ssl_ctx);
        OPENSSL_cleanse(ctx, sizeof(*ctx));
        free(ctx);
    }
    return -1;
}

void peer_tls_ctx_free(struct peer_tls_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->ssl_ctx)
        SSL_CTX_free(ctx->ssl_ctx);
    OPENSSL_cleanse(ctx, sizeof(*ctx));
    free(ctx);
}

void peer_tls_ctx_public_key(const struct peer_tls_ctx *ctx, uint8_t key[32])
{
    if (ctx && key)
        memcpy(key, ctx->public_key, sizeof(ctx->public_key));
}

int peer_tls_conn_new(struct peer_tls_ctx *ctx, int initiator,
                      struct peer_tls_conn **out)
{
    if (!ctx || !out)
        return -1;
    *out = NULL;
    ERR_clear_error();

    struct peer_tls_conn *conn = calloc(1, sizeof(*conn));
    if (!conn)
        return -1;
    conn->ssl = SSL_new(ctx->ssl_ctx);
    conn->rbio = BIO_new(BIO_s_mem());
    conn->wbio = BIO_new(BIO_s_mem());
    if (!conn->ssl || !conn->rbio || !conn->wbio) {
        if (conn->ssl)
            SSL_free(conn->ssl);
        if (conn->rbio)
            BIO_free(conn->rbio);
        if (conn->wbio)
            BIO_free(conn->wbio);
        free(conn);
        return -1;
    }
    /* An empty memory BIO reads as end-of-file by default, which TLS would take
     * for a closed peer. Saying "retry" instead is what makes an incomplete
     * arrival a pause rather than a fault. */
    BIO_set_mem_eof_return(conn->rbio, -1);
    SSL_set_bio(conn->ssl, conn->rbio, conn->wbio);   /* ssl owns them now */
    if (initiator)
        SSL_set_connect_state(conn->ssl);
    else
        SSL_set_accept_state(conn->ssl);
    *out = conn;
    return 0;
}

void peer_tls_conn_free(struct peer_tls_conn *conn)
{
    if (!conn)
        return;
    if (conn->ssl)
        SSL_free(conn->ssl);            /* frees both BIOs */
    OPENSSL_cleanse(conn, sizeof(*conn));
    free(conn);
}

/* The peer's key is read once, when the handshake completes. A certificate
 * that cannot yield a raw Ed25519 key is a fault rather than a refusal: the
 * caller has nothing to pin, so there is no decision left to make. */
static int peer_tls_capture_key(struct peer_tls_conn *conn)
{
    X509 *cert = SSL_get1_peer_certificate(conn->ssl);
    if (!cert)
        return -1;
    EVP_PKEY *pkey = X509_get0_pubkey(cert);
    size_t len = sizeof(conn->peer_key);
    int ok = pkey && EVP_PKEY_get_base_id(pkey) == EVP_PKEY_ED25519 &&
             EVP_PKEY_get_raw_public_key(pkey, conn->peer_key, &len) == 1 &&
             len == sizeof(conn->peer_key);
    X509_free(cert);
    return ok ? 0 : -1;
}

int peer_tls_handshake(struct peer_tls_conn *conn)
{
    if (!conn || conn->faulted)
        return -1;
    if (conn->established)
        return 1;
    ERR_clear_error();
    int rc = SSL_do_handshake(conn->ssl);
    if (rc == 1) {
        if (peer_tls_capture_key(conn) != 0) {
            conn->faulted = 1;
            return -1;
        }
        conn->established = 1;
        return 1;
    }
    int err = SSL_get_error(conn->ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return 0;
    conn->faulted = 1;
    return -1;
}

int peer_tls_established(const struct peer_tls_conn *conn)
{
    return conn && conn->established && !conn->faulted;
}

int peer_tls_faulted(const struct peer_tls_conn *conn)
{
    return !conn || conn->faulted;
}

int peer_tls_peer_key(const struct peer_tls_conn *conn, uint8_t key[32])
{
    if (!conn || !key || !conn->established || conn->faulted)
        return -1;
    memcpy(key, conn->peer_key, sizeof(conn->peer_key));
    return 0;
}

int peer_tls_write(struct peer_tls_conn *conn, const void *buf, size_t len)
{
    if (!conn || conn->faulted || !conn->established || !buf || len == 0 ||
        len > INT_MAX)
        return -1;
    ERR_clear_error();
    if (SSL_write(conn->ssl, buf, (int)len) == (int)len)
        return 0;
    conn->faulted = 1;
    return -1;
}

long peer_tls_read(struct peer_tls_conn *conn, void *buf, size_t cap)
{
    if (!conn || conn->faulted)
        return -1;
    if (!conn->established || !buf || cap == 0)
        return 0;
    if (cap > INT_MAX)
        cap = INT_MAX;
    ERR_clear_error();
    int got = SSL_read(conn->ssl, buf, (int)cap);
    if (got > 0)
        return got;
    int err = SSL_get_error(conn->ssl, got);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return 0;
    /* A clean close is a fault too: the seam surfaces a lost peer as a lost
     * channel, and there is no half-open state above this. */
    conn->faulted = 1;
    return -1;
}

long peer_tls_out(struct peer_tls_conn *conn, void *buf, size_t cap)
{
    if (!conn || conn->faulted)
        return -1;
    if (!buf || cap == 0)
        return 0;
    if (cap > INT_MAX)
        cap = INT_MAX;
    int got = BIO_read(conn->wbio, buf, (int)cap);
    return got > 0 ? got : 0;
}

size_t peer_tls_out_pending(struct peer_tls_conn *conn)
{
    if (!conn || conn->faulted)
        return 0;
    long pending = BIO_pending(conn->wbio);
    return pending > 0 ? (size_t)pending : 0;
}

int peer_tls_in(struct peer_tls_conn *conn, const void *buf, size_t len)
{
    if (!conn || conn->faulted)
        return -1;
    if (!buf || len == 0)
        return 0;
    if (len > INT_MAX || BIO_write(conn->rbio, buf, (int)len) != (int)len) {
        conn->faulted = 1;
        return -1;
    }
    return 0;
}
