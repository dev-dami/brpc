/**
 * brpc_compress.c — zlib compression/decompression for brpc frames
 */

#include "brpc_compress.h"
#include <zlib.h>
#include <string.h>

int brpc_compress_zlib(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t *dst_len)
{
    if (!src || !dst || !dst_len) return -1;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    /* windowBits = 15 + 16 for gzip format. */
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return -1;
    }

    strm.next_in  = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = dst;
    strm.avail_out = (uInt)*dst_len;

    int rc = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);

    if (rc != Z_STREAM_END) {
        return -1;
    }

    *dst_len = strm.total_out;
    return 0;
}

int brpc_decompress_zlib(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t *dst_len)
{
    if (!src || !dst || !dst_len) return -1;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    /* windowBits = 15 + 16 for gzip format. */
    if (inflateInit2(&strm, 15 + 16) != Z_OK) {
        return -1;
    }

    strm.next_in  = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = dst;
    strm.avail_out = (uInt)*dst_len;

    int rc = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    if (rc != Z_STREAM_END) {
        return -1;
    }

    *dst_len = strm.total_out;
    return 0;
}

size_t brpc_compress_max_size(size_t src_len)
{
    /* zlib worst case: src_len + 0.1% + 11 bytes, plus gzip overhead. */
    return src_len + (src_len / 1000) + 64;
}
