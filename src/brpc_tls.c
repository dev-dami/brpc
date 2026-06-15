/**
 * brpc_tls.c — OpenSSL TLS transport for brpc
 *
 * Wraps a connected socket fd with SSL_read/SSL_write so the rest
 * of brpc can use TLS transparently.
 */

#include "brpc_tls.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <string.h>
#include <stdlib.h>

/* --------------------------------------------------------------------------
 * Internal structures
 * -------------------------------------------------------------------------- */

struct brpc_tls_ctx {
    SSL_CTX *ssl_ctx;
};

struct brpc_tls {
    SSL     *ssl;
    int      fd;
};

static int tls_initialized = 0;

/* --------------------------------------------------------------------------
 * Global init
 * -------------------------------------------------------------------------- */

int brpc_tls_init(void)
{
    if (tls_initialized) return 0;

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    tls_initialized = 1;
    return 0;
}

void brpc_tls_shutdown_global(void)
{
    if (!tls_initialized) return;
    EVP_cleanup();
    ERR_free_strings();
    tls_initialized = 0;
}

void brpc_tls_shutdown(void)
{
    brpc_tls_shutdown_global();
}

/* --------------------------------------------------------------------------
 * TLS context
 * -------------------------------------------------------------------------- */

brpc_tls_ctx_t *brpc_tls_ctx_create_client(const char *ca_file,
                                           const char *ca_dir)
{
    brpc_tls_ctx_t *ctx = (brpc_tls_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    /* Load CA certificate for server verification. */
    if (ca_file || ca_dir) {
        if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, ca_dir) != 1) {
            SSL_CTX_free(ctx->ssl_ctx);
            free(ctx);
            return NULL;
        }
        SSL_CTX_set_verify(ctx->ssl_ctx,
                           SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           NULL);
    } else {
        /* No CA provided — skip verification (for trusted networks). */
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_NONE, NULL);
    }

    return ctx;
}

brpc_tls_ctx_t *brpc_tls_ctx_create_server(const char *cert_file,
                                           const char *key_file)
{
    brpc_tls_ctx_t *ctx = (brpc_tls_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    /* Load server certificate. */
    if (SSL_CTX_use_certificate_file(ctx->ssl_ctx, cert_file,
                                     SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    /* Load server private key. */
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key_file,
                                    SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    /* Verify private key matches certificate. */
    if (SSL_CTX_check_private_key(ctx->ssl_ctx) != 1) {
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void brpc_tls_ctx_destroy(brpc_tls_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
    }
    free(ctx);
}

/* --------------------------------------------------------------------------
 * TLS connection
 * -------------------------------------------------------------------------- */

brpc_tls_t *brpc_tls_connect(brpc_tls_ctx_t *ctx, int fd,
                             const char *hostname)
{
    if (!ctx || fd < 0) return NULL;

    brpc_tls_t *tls = (brpc_tls_t *)calloc(1, sizeof(*tls));
    if (!tls) return NULL;

    tls->ssl = SSL_new(ctx->ssl_ctx);
    if (!tls->ssl) {
        free(tls);
        return NULL;
    }

    tls->fd = fd;

    /* Set SNI hostname. */
    if (hostname) {
        SSL_set_tlsext_host_name(tls->ssl, hostname);
    }

    /* Associate the fd with the SSL object. */
    SSL_set_fd(tls->ssl, fd);

    /* Perform the handshake. */
    int rc = SSL_connect(tls->ssl);
    if (rc != 1) {
        /* Non-fatal: SSL_ERROR_WANT_READ/WANT_WRITE would need retry
         * in an event loop. For now, treat any failure as fatal. */
        SSL_free(tls->ssl);
        free(tls);
        return NULL;
    }

    return tls;
}

brpc_tls_t *brpc_tls_accept(brpc_tls_ctx_t *ctx, int fd)
{
    if (!ctx || fd < 0) return NULL;

    brpc_tls_t *tls = (brpc_tls_t *)calloc(1, sizeof(*tls));
    if (!tls) return NULL;

    tls->ssl = SSL_new(ctx->ssl_ctx);
    if (!tls->ssl) {
        free(tls);
        return NULL;
    }

    tls->fd = fd;
    SSL_set_fd(tls->ssl, fd);

    int rc = SSL_accept(tls->ssl);
    if (rc != 1) {
        SSL_free(tls->ssl);
        free(tls);
        return NULL;
    }

    return tls;
}

int brpc_tls_read(brpc_tls_t *tls, void *buf, size_t len)
{
    if (!tls || !tls->ssl) return -1;

    int n = SSL_read(tls->ssl, buf, (int)len);
    if (n > 0) return n;

    int err = SSL_get_error(tls->ssl, n);
    switch (err) {
    case SSL_ERROR_ZERO_RETURN:
        return 0;  /* Peer closed the TLS connection. */
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
        return -1;  /* Would block — caller should retry. */
    default:
        return -1;
    }
}

int brpc_tls_write(brpc_tls_t *tls, const void *buf, size_t len)
{
    if (!tls || !tls->ssl) return -1;

    int n = SSL_write(tls->ssl, buf, (int)len);
    if (n > 0) return n;

    int err = SSL_get_error(tls->ssl, n);
    (void)err;
    return -1;
}

int brpc_tls_close(brpc_tls_t *tls)
{
    if (!tls || !tls->ssl) return -1;

    SSL_shutdown(tls->ssl);
    return 0;
}

void brpc_tls_free(brpc_tls_t *tls)
{
    if (!tls) return;
    if (tls->ssl) {
        SSL_free(tls->ssl);
    }
    free(tls);
}

int brpc_tls_fd(const brpc_tls_t *tls)
{
    return tls ? tls->fd : -1;
}

const char *brpc_tls_error_string(void)
{
    unsigned long err = ERR_get_error();
    if (err == 0) return "no error";
    return ERR_error_string(err, NULL);
}
