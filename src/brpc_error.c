/**
 * brpc_error.c — Error code to string conversion
 */

#include "brpc_error.h"

const char *brpc_error_string(brpc_error_t err)
{
    switch (err) {
    case BRPC_OK:                   return "ok";
    case BRPC_ERROR_CLOSED:         return "channel closed";
    case BRPC_ERROR_TIMEOUT:        return "timeout";
    case BRPC_ERROR_PROTOCOL:       return "protocol error";
    case BRPC_ERROR_IO:             return "I/O error";
    case BRPC_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case BRPC_ERROR_STREAM_NOT_FOUND: return "stream not found";
    case BRPC_ERROR_STREAM_CLOSED:  return "stream closed";
    case BRPC_ERROR_MAX_STREAMS:    return "max streams reached";
    case BRPC_ERROR_COMPRESSION:    return "compression error";
    case BRPC_ERROR_TLS:            return "TLS error";
    case BRPC_ERROR_ALLOCATION:     return "allocation failed";
    default:                        return "unknown error";
    }
}
