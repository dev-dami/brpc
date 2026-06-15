/**
 * brpc_compress.h — Compression support for brpc frames
 *
 * Provides zlib-based compression/decompression for frame payloads.
 * When enabled, the COMPRESSED flag is set in the frame header and
 * the payload is compressed before sending.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compress `src` of length `src_len` into `dst`.
 * `dst_len` is the size of the destination buffer on input and the
 * number of compressed bytes on output.
 *
 * @return 0 on success, -1 on error (buffer too small, etc.).
 */
int brpc_compress_zlib(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t *dst_len);

/**
 * Decompress `src` of length `src_len` into `dst`.
 * `dst_len` is the size of the destination buffer on input and the
 * number of decompressed bytes on output.
 *
 * @return 0 on success, -1 on error.
 */
int brpc_decompress_zlib(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t *dst_len);

/**
 * Estimate the maximum compressed size for a given input size.
 * Returns `src_len + 12.5% + 11 bytes` (zlib worst case).
 */
size_t brpc_compress_max_size(size_t src_len);

#ifdef __cplusplus
}
#endif
