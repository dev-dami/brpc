/**
 * brpc_error.h — bRPC Error Codes
 *
 * Unified error enum used across the brpc API. Negative values indicate
 * errors; zero (BRPC_OK) indicates success.
 *
 * JSON-RPC 2.0 application errors (parse, method not found, etc.) use
 * the standard -32xxx range and are defined separately in brpc_rpc.h.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum brpc_error {
    BRPC_OK                        =    0,
    BRPC_ERROR_CLOSED              =   -1,
    BRPC_ERROR_TIMEOUT             =   -2,
    BRPC_ERROR_PROTOCOL            =   -3,
    BRPC_ERROR_IO                  =   -4,
    BRPC_ERROR_INVALID_ARGUMENT    =   -5,
    BRPC_ERROR_STREAM_NOT_FOUND    =   -6,
    BRPC_ERROR_STREAM_CLOSED       =   -7,
    BRPC_ERROR_MAX_STREAMS         =   -8,
    BRPC_ERROR_COMPRESSION         =   -9,
    BRPC_ERROR_TLS                 =  -10,
    BRPC_ERROR_ALLOCATION          =  -11,
} brpc_error_t;

/**
 * Return a human-readable string for an error code.
 * Returns "unknown error" for unrecognized codes.
 */
const char *brpc_error_string(brpc_error_t err);

#ifdef __cplusplus
}
#endif
