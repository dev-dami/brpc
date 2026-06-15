/**
 * brpc_tls.h — TLS transport layer for brpc
 *
 * Provides OpenSSL-based TLS wrapping for brpc channels. Once a socket
 * is wrapped with TLS, all reads/writes go through SSL_read/SSL_write.
 *
 * Usage (client):
 *   brpc_tls_init();
 *   brpc_tls_ctx_t *ctx = brpc_tls_ctx_create_client(NULL, NULL);
 *   brpc_tls_t *tls = brpc_tls_connect(ctx, fd, "localhost");
 *   // Use brpc_tls_read/write instead of read/write
 *   brpc_tls_close(tls);
 *   brpc_tls_free(tls);
 *   brpc_tls_ctx_destroy(ctx);
 *
 * Usage (server):
 *   brpc_tls_init();
 *   brpc_tls_ctx_t *ctx = brpc_tls_ctx_create_server("cert.pem", "key.pem");
 *   brpc_tls_t *tls = brpc_tls_accept(ctx, fd);
 *   brpc_tls_read/write...
 *   brpc_tls_close(tls);
 *   brpc_tls_free(tls);
 *   brpc_tls_ctx_destroy(ctx);
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Forward declarations (opaque pointers)
 * -------------------------------------------------------------------------- */

typedef struct brpc_tls_ctx brpc_tls_ctx_t;
typedef struct brpc_tls     brpc_tls_t;

/* --------------------------------------------------------------------------
 * Global init
 * -------------------------------------------------------------------------- */

/**
 * Initialize OpenSSL. Call once before any TLS operations.
 * Safe to call multiple times (no-op after first call).
 *
 * @return 0 on success, -1 on error.
 */
int brpc_tls_init(void);

/**
 * Shutdown OpenSSL. Call once at program exit (optional).
 */
void brpc_tls_shutdown(void);

/* --------------------------------------------------------------------------
 * TLS context (shared configuration)
 * -------------------------------------------------------------------------- */

/**
 * Create a TLS context for a client.
 *
 * @param ca_file   Path to CA certificate file (PEM), or NULL to skip
 *                  server verification (NOT recommended for production).
 * @param ca_dir    Path to CA certificate directory, or NULL.
 * @return TLS context, or NULL on error.
 */
brpc_tls_ctx_t *brpc_tls_ctx_create_client(const char *ca_file,
                                           const char *ca_dir);

/**
 * Create a TLS context for a server.
 *
 * @param cert_file Path to server certificate file (PEM).
 * @param key_file  Path to server private key file (PEM).
 * @return TLS context, or NULL on error.
 */
brpc_tls_ctx_t *brpc_tls_ctx_create_server(const char *cert_file,
                                           const char *key_file);

/**
 * Destroy a TLS context.
 */
void brpc_tls_ctx_destroy(brpc_tls_ctx_t *ctx);

/* --------------------------------------------------------------------------
 * TLS connection wrapping
 * -------------------------------------------------------------------------- */

/**
 * Perform TLS handshake on a connected client socket.
 *
 * @param ctx       TLS context.
 * @param fd        Connected socket file descriptor.
 * @param hostname  Server hostname for SNI (or NULL).
 * @return TLS connection, or NULL on error.
 */
brpc_tls_t *brpc_tls_connect(brpc_tls_ctx_t *ctx, int fd,
                             const char *hostname);

/**
 * Perform TLS handshake on a connected server socket.
 *
 * @param ctx  TLS context.
 * @param fd   Connected socket file descriptor.
 * @return TLS connection, or NULL on error.
 */
brpc_tls_t *brpc_tls_accept(brpc_tls_ctx_t *ctx, int fd);

/**
 * Read decrypted data from the TLS connection.
 *
 * @return Number of bytes read, 0 on EOF, -1 on error.
 */
int brpc_tls_read(brpc_tls_t *tls, void *buf, size_t len);

/**
 * Write data through the TLS connection.
 *
 * @return Number of bytes written, -1 on error.
 */
int brpc_tls_write(brpc_tls_t *tls, const void *buf, size_t len);

/**
 * Perform TLS shutdown on a connection (send close_notify).
 *
 * @return 0 on success, -1 on error.
 */
int brpc_tls_close(brpc_tls_t *tls);

/**
 * Free the TLS connection state.
 * Does NOT close the underlying fd.
 */
void brpc_tls_free(brpc_tls_t *tls);

/**
 * Get the underlying file descriptor.
 */
int brpc_tls_fd(const brpc_tls_t *tls);

/**
 * Get the last OpenSSL error string.
 */
const char *brpc_tls_error_string(void);

#ifdef __cplusplus
}
#endif
